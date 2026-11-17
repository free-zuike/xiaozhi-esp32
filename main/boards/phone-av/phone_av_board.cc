#include "wifi_board.h"
#include "button.h"
#include "audio_codec.h"
#include "camera.h"
#include "display.h"
#include "config.h"
#include "settings.h"
#include "system_info.h"
#include "application.h"
#include "assets.h"
#include "phone_av_audio.h"
#include "phone_av_display.h"
#include "backlight.h"
#include "display/lcd_display.h"
#include <esp_timer.h>

#include "led/led.h"
#include "led/single_led.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <driver/spi_common.h>
#include <esp_http_server.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_wifi.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#if PHONE_AV_HW_DISP_TYPE == 2
#include "esp_lcd_ili9341.h"
#elif PHONE_AV_HW_DISP_TYPE == 3
#include "esp_lcd_gc9a01.h"
#endif

#define TAG "PhoneAvBoard"

constexpr uint16_t kPhoneAvConfigPort = 8080;
constexpr const char* kPhoneAvConfigNs = "phone_av";
constexpr const char* kPhoneAvConfigKey = "host";
constexpr const char* kPhoneAvSrcSpkKey = "src_spk";
constexpr const char* kPhoneAvSrcMicKey = "src_mic";
constexpr const char* kPhoneAvSrcDispKey = "src_disp";

/* The phone address is runtime configuration, not a build-time constant.
 * It is kept in NVS so it survives reboots and firmware updates. */
std::string PhoneAvHost() {
    Settings settings(kPhoneAvConfigNs, false);
    return settings.GetString(kPhoneAvConfigKey, PHONE_AV_HOST);
}

std::string PhoneAvUrl(const char* path) {
    return std::string("http://") + PhoneAvHost() + ":" + std::to_string(PHONE_AV_PORT) + path;
}

namespace {

// Diagnostics endpoint handler; defined below (after PhoneAvBoard is complete).
esp_err_t PhoneAvStatusHandler(httpd_req_t* req);
// Serves files straight out of the real assets partition: the same index.json
// and emoji images that a physical LVGL display would consume.
esp_err_t PhoneAvAssetsHandler(httpd_req_t* req);

std::string PhoneAvJsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (unsigned char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

constexpr bool PhoneAvHwSpkConfigured() {
    return PHONE_AV_HW_SPK_BCLK != GPIO_NUM_NC && PHONE_AV_HW_SPK_LRCK != GPIO_NUM_NC &&
           PHONE_AV_HW_SPK_DIN != GPIO_NUM_NC;
}

constexpr bool PhoneAvHwMicConfigured() {
#if PHONE_AV_HW_MIC_PDM
    return PHONE_AV_HW_MIC_SCK != GPIO_NUM_NC && PHONE_AV_HW_MIC_DIN != GPIO_NUM_NC;
#else
    return PHONE_AV_HW_MIC_SCK != GPIO_NUM_NC && PHONE_AV_HW_MIC_WS != GPIO_NUM_NC &&
           PHONE_AV_HW_MIC_DIN != GPIO_NUM_NC;
#endif
}

constexpr bool PhoneAvHwDispConfigured() {
#if PHONE_AV_HW_DISP_TYPE == 0
    return false;
#else
    return PHONE_AV_HW_DISP_MOSI != GPIO_NUM_NC && PHONE_AV_HW_DISP_CLK != GPIO_NUM_NC &&
           PHONE_AV_HW_DISP_CS != GPIO_NUM_NC && PHONE_AV_HW_DISP_DC != GPIO_NUM_NC;
#endif
}

const char* PhoneAvSourceToken(PhoneAvSource source) {
    return source == PhoneAvSource::kHardware ? "hw" : "phone";
}

PhoneAvSource PhoneAvParseSource(const std::string& value, PhoneAvSource fallback) {
    if (value == "hw") {
        return PhoneAvSource::kHardware;
    }
    if (value == "phone") {
        return PhoneAvSource::kPhone;
    }
    return fallback;
}

PhoneAvSource PhoneAvGetSource(const char* key) {
    Settings settings(kPhoneAvConfigNs, false);
    return PhoneAvParseSource(settings.GetString(key, "phone"), PhoneAvSource::kPhone);
}

std::string PhoneAvUrlDecode(const std::string& in) {
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    };
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (c == '+') {
            out += ' ';
        } else if (c == '%' && i + 2 < in.size()) {
            int hi = hex(in[i + 1]);
            int lo = hex(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
            } else {
                out += c;
            }
        } else {
            out += c;
        }
    }
    return out;
}

std::string PhoneAvFormValue(const std::string& body, const char* key) {
    const std::string prefix = std::string(key) + "=";
    std::size_t pos = 0;
    while ((pos = body.find(prefix, pos)) != std::string::npos) {
        if (pos == 0 || body[pos - 1] == '&') {
            std::size_t start = pos + prefix.size();
            std::size_t end = body.find('&', start);
            if (end == std::string::npos) {
                end = body.size();
            }
            return PhoneAvUrlDecode(body.substr(start, end - start));
        }
        pos += prefix.size();
    }
    return "";
}

bool IsValidPhoneAvHost(const std::string& host) {
    if (host.empty() || host.size() > 64) {
        return false;
    }
    for (char ch : host) {
        auto c = static_cast<unsigned char>(ch);
        if (!std::isalnum(c) && c != '.' && c != '-' && c != ':' && c != '_') {
            return false;
        }
    }
    return true;
}

std::string PhoneAvConfigPage(const std::string& message) {
    const std::string host = PhoneAvHost();
    const bool spk_hw_ok = PhoneAvHwSpkConfigured();
    const bool mic_hw_ok = PhoneAvHwMicConfigured();
    const bool disp_hw_ok = PhoneAvHwDispConfigured();

    // If NVS still says "hw" but the pins are no longer configured, show the
    // effective phone selection so saving the page repairs the setting.
    PhoneAvSource spk = PhoneAvGetSource(kPhoneAvSrcSpkKey);
    PhoneAvSource mic = PhoneAvGetSource(kPhoneAvSrcMicKey);
    PhoneAvSource disp = PhoneAvGetSource(kPhoneAvSrcDispKey);
    if (spk == PhoneAvSource::kHardware && !spk_hw_ok) {
        spk = PhoneAvSource::kPhone;
    }
    if (mic == PhoneAvSource::kHardware && !mic_hw_ok) {
        mic = PhoneAvSource::kPhone;
    }
    if (disp == PhoneAvSource::kHardware && !disp_hw_ok) {
        disp = PhoneAvSource::kPhone;
    }

    auto radio = [](const char* name, const char* value, const char* label, bool checked,
                    bool disabled) {
        std::string out = "<label><input type=\"radio\" name=\"";
        out += name;
        out += "\" value=\"";
        out += value;
        out += "\"";
        if (checked) {
            out += " checked";
        }
        if (disabled) {
            out += " disabled";
        }
        out += "> ";
        out += label;
        out += "</label>";
        return out;
    };

    const std::string spk_controls =
        radio("src_spk", "phone", "手机", spk == PhoneAvSource::kPhone, false) +
        radio("src_spk", "hw", "本地功放", spk == PhoneAvSource::kHardware, !spk_hw_ok) +
        (spk_hw_ok ? "" : "<span class=\"warn\">未配置（config.h）</span>");
    const std::string mic_controls =
        radio("src_mic", "phone", "手机", mic == PhoneAvSource::kPhone, false) +
        radio("src_mic", "hw", "本地麦克风", mic == PhoneAvSource::kHardware, !mic_hw_ok) +
        (mic_hw_ok ? "" : "<span class=\"warn\">未配置（config.h）</span>");
    const std::string disp_controls =
        radio("src_disp", "phone", "8080 虚拟屏", disp == PhoneAvSource::kPhone, false) +
        radio("src_disp", "hw", "本地 LCD", disp == PhoneAvSource::kHardware, !disp_hw_ok) +
        (disp_hw_ok ? "" : "<span class=\"warn\">未配置（config.h）</span>");

    std::string html =
        "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>phone-av 设置</title><style>"
        "body{font-family:sans-serif;max-width:480px;margin:40px auto;padding:0 16px;line-height:1.6}"
        "input{font-size:16px;padding:8px;width:100%;box-sizing:border-box}"
        "button{font-size:16px;padding:10px 20px;margin-top:12px}"
        "code{background:#eee;padding:2px 6px}"
        ".src div{margin:6px 0}.src label{margin-right:14px;white-space:nowrap}"
        ".warn{color:#b00;font-size:13px}</style></head><body>"
        "<h2>手机 AV 地址设置</h2>"
        "<p>当前手机地址：<code>" + host + "</code></p>"
        "<p>端口：<code>" + std::to_string(PHONE_AV_PORT) +
        "</code>（需与手机 App 的「推流端口」一致）</p>"
        "<form method=\"post\" action=\"/set\">"
        "<label>手机 IP 或域名<br><input name=\"host\" value=\"" + host +
        "\" required></label>"
        "<h2>能力来源</h2>"
        "<p>保存后板子会自动重启，新的来源才会生效。</p>"
        "<div class=\"src\">"
        "<div>喇叭：" + spk_controls + "</div>"
        "<div>录音：" + mic_controls + "</div>"
        "<div>屏幕：" + disp_controls + "</div>"
        "</div>"
        "<button type=\"submit\">保存并重启</button></form>";
    if (!message.empty()) {
        html += "<p><b>" + message + "</b></p>";
    }
    html +=
        "<p>保存后板子会自动重启一次让新地址/来源生效；WiFi 配置不会丢失。"
        "以后手机 IP 变了只需再来这里改，不用重刷固件。</p>"
        "<p><a href=\"/api/host\">查看当前地址（纯文本）</a></p>"
        "<h2>设备屏幕（虚拟）</h2>"
        "<p>未接本地屏幕时，xiaozhi 的屏幕内容（含 6 位设备验证码）显示在这里，每秒刷新。"
        "即使已切到本地 LCD，这里仍会继续镜像一份，方便远程查看。</p>"
        "<p>主题色、表情图片与背景资源都取自真实的 assets 分区（与实体 LCD 同一份 index.json）。未刷 assets 时先显示内置 emoji 占位，刷入真实资源后自动切换为硬件表情图。</p>"
        "<div id=\"face\" style=\"text-align:center;min-height:96px;font-size:72px;line-height:96px\">"
        "<span id=\"faceEmoji\" style=\"display:none\"></span>"
        "<img id=\"faceImg\" alt=\"\" style=\"display:none;max-width:128px;max-height:128px;vertical-align:middle\">"
        "</div>"
        "<div id=\"screen\" style=\"white-space:pre-wrap;word-break:break-all;"
        "background:#111;color:#eee;padding:12px;border-radius:6px;font-family:monospace\">"
        "读取中...</div>"
        "<script>"
        "let assetIndex=null;"
        "const srcName=s=>s==='hw'?'本地硬件':'手机';"
        "fetch('/assets/index.json',{cache:'no-store'})"
        "  .then(r=>r.ok?r.json():null).then(j=>{assetIndex=j;applySkin();})"
        "  .catch(()=>{});"
        "function applySkin(){"
        "  if(!assetIndex||!assetIndex.skin)return;"
        "  const s=assetIndex.skin.light||assetIndex.skin.dark;"
        "  if(!s)return;"
        "  const box=document.getElementById('screen');"
        "  if(s.background_color)box.style.background=s.background_color;"
        "  if(s.text_color)box.style.color=s.text_color;"
        "  if(s.background_image){"
        "    const base=s.background_image.replace(/\\.[^.]+$/,'');"
        "    const probe=new Image();let alt=false;"
        "    probe.onload=function(){"
        "      box.style.backgroundImage='url('+probe.src+')';"
        "      box.style.backgroundSize='cover';"
        "    };"
        "    probe.onerror=function(){if(!alt){alt=true;probe.src='/assets/'+base+'.jpg';}};"
        "    probe.src='/assets/'+base+'.png';"
        "  }"
        "}"
        "function applyFace(emotion){"
        "  const img=document.getElementById('faceImg');"
        "  const span=document.getElementById('faceEmoji');"
        "  if(emotion&&assetIndex&&Array.isArray(assetIndex.emoji_collection)){"
        "    const list=assetIndex.emoji_collection;"
        "    const e=list.find(x=>x.name===emotion)||"
        "            list.find(x=>x.name&&x.name.indexOf(emotion)===0);"
        "    if(e&&e.file){"
        "      img.onload=function(){img.style.display='inline';span.style.display='none';};"
        "      img.onerror=function(){showFallback(emotion,span,img);};"
        "      img.src='/assets/'+e.file;"
        "      return;"
        "    }"
        "  }"
        "  showFallback(emotion,span,img);"
        "}"
        "function showFallback(emotion,span,img){"
        "  const MAP={'neutral':'🙂','happy':'😄','sad':'😢','angry':'😠',"
        "    'surprised':'😲','sleepy':'😴','thinking':'🤔','confused':'😕',"
        "    'excited':'🤩','fear':'😨','fearful':'😨','worried':'😟','love':'😍'};"
        "  const key=(emotion||'neutral').toLowerCase();"
        "  span.textContent=MAP[key]||MAP['neutral'];"
        "  span.style.display='inline';"
        "  img.style.display='none';"
        "}"
        "async function refreshScreen(){"
        "  try{"
        "    const r=await fetch('/status',{cache:'no-store'});"
        "    const j=await r.json();"
        "    let t='来源: 喇叭='+srcName(j.active_spk)+' 录音='+srcName(j.active_mic)+"
        "          ' 屏幕='+srcName(j.active_disp)+'\\n';"
        "    t+='状态: '+(j.screen_status||'')+'\\n';"
        "    t+='表情: '+(j.screen_emotion||'')+'  主题: '+(j.screen_theme||'')+'\\n';"
        "    t+='资源分区: '+(j.assets_valid?'有效':'未就绪')+'\\n';"
        "    if(j.screen_notification)t+='通知: '+j.screen_notification+'\\n';"
        "    t+='\\n--- 对话 ---\\n';"
        "    t+=(j.screen_messages||[]).join('\\n')||'（暂无）';"
        "    t+='\\n\\n--- 设备日志 ---\\n';"
        "    t+=(j.screen_history||[]).join('\\n');"
        "    document.getElementById('screen').textContent=t;"
        "    applyFace(j.screen_emotion);"
        "  }catch(e){document.getElementById('screen').textContent='读取失败: '+e;}"
        "}"
        "setInterval(refreshScreen,1000);refreshScreen();"
        "</script>"
        "</body></html>";
    return html;
}

esp_err_t PhoneAvConfigGetHandler(httpd_req_t* req) {
    const std::string page = PhoneAvConfigPage("");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, page.c_str(), HTTPD_RESP_USE_STRLEN);
}

esp_err_t PhoneAvConfigHostHandler(httpd_req_t* req) {
    const std::string host = PhoneAvHost();
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, host.c_str(), HTTPD_RESP_USE_STRLEN);
}

void PhoneAvRestartTask(void*) {
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    vTaskDelete(nullptr);
}

esp_err_t PhoneAvConfigSetHandler(httpd_req_t* req) {
    char body[512] = {0};
    int received = httpd_req_recv(req, body, static_cast<size_t>(sizeof(body) - 1));
    if (received < 0) {
        const std::string page = PhoneAvConfigPage("读取请求失败，请重试");
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_send(req, page.c_str(), HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }
    body[received] = '\0';
    const std::string form(body, static_cast<std::size_t>(received));
    const std::string host = PhoneAvFormValue(form, "host");
    if (!IsValidPhoneAvHost(host)) {
        const std::string page = PhoneAvConfigPage("地址无效，请填写 IP（如 192.168.0.108）或域名");
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_send(req, page.c_str(), HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    PhoneAvSource spk = PhoneAvParseSource(
        PhoneAvFormValue(form, kPhoneAvSrcSpkKey), PhoneAvGetSource(kPhoneAvSrcSpkKey));
    PhoneAvSource mic = PhoneAvParseSource(
        PhoneAvFormValue(form, kPhoneAvSrcMicKey), PhoneAvGetSource(kPhoneAvSrcMicKey));
    PhoneAvSource disp = PhoneAvParseSource(
        PhoneAvFormValue(form, kPhoneAvSrcDispKey), PhoneAvGetSource(kPhoneAvSrcDispKey));

    if (spk == PhoneAvSource::kHardware && !PhoneAvHwSpkConfigured()) {
        const std::string page = PhoneAvConfigPage(
            "本地喇叭未配置：请先在 config.h 填 PHONE_AV_HW_SPK_BCLK/LRCK/DIN 并重新编译");
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_send(req, page.c_str(), HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (mic == PhoneAvSource::kHardware && !PhoneAvHwMicConfigured()) {
        const std::string page = PhoneAvConfigPage(
            "本地麦克风未配置：请先在 config.h 填 PHONE_AV_HW_MIC_* 并重新编译");
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_send(req, page.c_str(), HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (disp == PhoneAvSource::kHardware && !PhoneAvHwDispConfigured()) {
        const std::string page = PhoneAvConfigPage(
            "本地屏幕未配置：请先在 config.h 填 PHONE_AV_HW_DISP_* 并重新编译");
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_send(req, page.c_str(), HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    {
        Settings settings(kPhoneAvConfigNs, true);
        settings.SetString(kPhoneAvConfigKey, host);
        settings.SetString(kPhoneAvSrcSpkKey, PhoneAvSourceToken(spk));
        settings.SetString(kPhoneAvSrcMicKey, PhoneAvSourceToken(mic));
        settings.SetString(kPhoneAvSrcDispKey, PhoneAvSourceToken(disp));
    }
    ESP_LOGI(TAG, "Phone AV config updated: host=%s spk=%s mic=%s disp=%s, restarting",
             host.c_str(), PhoneAvSourceToken(spk), PhoneAvSourceToken(mic),
             PhoneAvSourceToken(disp));
    const std::string page = PhoneAvConfigPage("已保存，板子即将自动重启…");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, page.c_str(), HTTPD_RESP_USE_STRLEN);
    xTaskCreate(PhoneAvRestartTask, "phone_av_restart", 2048, nullptr, 5, nullptr);
    return ESP_OK;
}

/* The virtual screen is a thin mirror of the real assets partition:
 * index.json, emoji PNG/GIF files and backgrounds are read through
 * Assets::GetAssetData() and sent verbatim. A physical LVGL display consumes
 * exactly the same bytes, so none of this has to be rewritten when the LCD
 * arrives -- only the Display implementation behind GetDisplay() changes. */
esp_err_t PhoneAvAssetsHandler(httpd_req_t* req) {
    constexpr const char* kPrefix = "/assets/";
    constexpr std::size_t kPrefixLen = sizeof("/assets/") - 1;
    std::string name;
    // req->uri is a fixed-size member array, never NULL; just match the prefix.
    if (std::strncmp(req->uri, kPrefix, kPrefixLen) == 0) {
        name = req->uri + kPrefixLen;
    }
    auto query = name.find('?');
    if (query != std::string::npos) {
        name.resize(query);
    }
    if (name.empty() || name.find("..") != std::string::npos) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    void* data = nullptr;
    std::size_t size = 0;
    if (!Assets::GetInstance().GetAssetData(name, data, size) || data == nullptr || size == 0) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    const char* type = "application/octet-stream";
    auto dot = name.rfind('.');
    if (dot != std::string::npos) {
        const std::string ext = name.substr(dot);
        if (ext == ".png") {
            type = "image/png";
        } else if (ext == ".gif") {
            type = "image/gif";
        } else if (ext == ".jpg" || ext == ".jpeg") {
            type = "image/jpeg";
        } else if (ext == ".json") {
            type = "application/json; charset=utf-8";
        }
    }
    httpd_resp_set_type(req, type);
    return httpd_resp_send(req, static_cast<const char*>(data), static_cast<ssize_t>(size));
}

void StartPhoneAvConfigServer() {
    static std::atomic<bool> started{false};
    if (started.exchange(true)) {
        return;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = kPhoneAvConfigPort;
    // The config-page handlers call into the display/assets path, which needs
    // more stack than the 4 KB httpd default; otherwise the httpd task overflows
    // and the board reboots in a loop right after connecting to WiFi.
    config.stack_size = 16 * 1024;
    config.max_uri_handlers = 8;
    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start phone-av config server");
        // The TCP/IP stack may not be ready yet when the board is constructed.
        // Do not latch "started": a later network event must be able to retry.
        started.store(false);
        return;
    }
    httpd_uri_t root_uri = {
        .uri = "/", .method = HTTP_GET,
        .handler = PhoneAvConfigGetHandler, .user_ctx = nullptr,
    };
    httpd_uri_t host_uri = {
        .uri = "/api/host", .method = HTTP_GET,
        .handler = PhoneAvConfigHostHandler, .user_ctx = nullptr,
    };
    httpd_uri_t set_uri = {
        .uri = "/set", .method = HTTP_POST,
        .handler = PhoneAvConfigSetHandler, .user_ctx = nullptr,
    };
    httpd_register_uri_handler(server, &root_uri);
    httpd_register_uri_handler(server, &host_uri);
    httpd_register_uri_handler(server, &set_uri);
    httpd_uri_t status_uri = {
        .uri = "/status", .method = HTTP_GET,
        .handler = PhoneAvStatusHandler, .user_ctx = nullptr,
    };
    httpd_register_uri_handler(server, &status_uri);
    httpd_uri_t assets_uri = {
        .uri = "/assets/*", .method = HTTP_GET,
        .handler = PhoneAvAssetsHandler, .user_ctx = nullptr,
    };
    httpd_register_uri_handler(server, &assets_uri);

    esp_netif_t* netif = esp_netif_get_default_netif();
    esp_netif_ip_info_t info{};
    if (netif != nullptr && esp_netif_get_ip_info(netif, &info) == ESP_OK) {
        ESP_LOGI(TAG, "Phone AV config page: http://" IPSTR ":%u/", IP2STR(&info.ip),
                 static_cast<unsigned>(kPhoneAvConfigPort));
    } else {
        ESP_LOGI(TAG, "Phone AV config page listening on port %u",
                 static_cast<unsigned>(kPhoneAvConfigPort));
    }
    ESP_LOGI(TAG, "Phone AV host: %s (change it on the config page)", PhoneAvHost().c_str());
}

class NetworkCamera : public Camera {
public:
    virtual void SetExplainUrl(const std::string& url, const std::string& token) override {
        std::lock_guard<std::mutex> lock(state_mutex_);
        explain_url_ = url;
        explain_token_ = token;
    }

    virtual bool Capture() override {
        if (!PhoneAvNetworkReady()) {
            return false;
        }
        auto network = Board::GetInstance().GetNetwork();
        auto http = network->CreateHttp(0);
        const std::string url = PhoneAvUrl(PHONE_AV_SNAPSHOT_PATH);
        if (auto opened = http->Open("GET", url); !opened) {
            ESP_LOGW(TAG, "Failed to open phone camera snapshot");
            http->Close();
            return false;
        }
        auto status = http->GetStatusCode();
        if (!status || *status != 200) {
            ESP_LOGW(TAG, "Phone camera snapshot rejected the request");
            http->Close();
            return false;
        }
        std::string jpeg = http->ReadAll();
        http->Close();
        if (jpeg.size() < 4 ||
            static_cast<uint8_t>(jpeg[0]) != 0xFF || static_cast<uint8_t>(jpeg[1]) != 0xD8 ||
            static_cast<uint8_t>(jpeg[jpeg.size() - 2]) != 0xFF || static_cast<uint8_t>(jpeg[jpeg.size() - 1]) != 0xD9) {
            ESP_LOGW(TAG, "Phone camera snapshot is not a complete JPEG");
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            jpeg_ = std::move(jpeg);
        }
        return true;
    }

    virtual bool SetHMirror(bool enabled) override {
        (void)enabled;
        return false;
    }

    virtual bool SetVFlip(bool enabled) override {
        (void)enabled;
        return false;
    }

    virtual std::expected<std::string, std::string> Explain(const std::string& question) override {
        if (!PhoneAvNetworkReady()) {
            return std::unexpected(std::string("Network not connected"));
        }
        std::string jpeg;
        std::string url;
        std::string token;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (jpeg_.empty() || explain_url_.empty()) {
                return std::unexpected(std::string("No phone camera frame available"));
            }
            jpeg = jpeg_;
            url = explain_url_;
            token = explain_token_;
        }

        const std::string boundary = "----PHONE_AV_CAMERA_BOUNDARY";
        std::string body;
        body.reserve(jpeg.size() + question.size() + 512);
        body += "--";
        body += boundary;
        body += "\r\nContent-Disposition: form-data; name=\"question\"\r\n\r\n";
        body += question;
        body += "\r\n--";
        body += boundary;
        body += "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"camera.jpg\"\r\n";
        body += "Content-Type: image/jpeg\r\n\r\n";
        body.append(jpeg.data(), jpeg.size());
        body += "\r\n--";
        body += boundary;
        body += "--\r\n";

        auto network = Board::GetInstance().GetNetwork();
        auto http = network->CreateHttp(0);
        http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
        http->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());
        if (!token.empty()) {
            http->SetHeader("Authorization", std::string("Bearer ") + token);
        }
        http->SetHeader("Content-Type", std::string("multipart/form-data; boundary=") + boundary);
        http->SetContent(std::move(body));
        if (auto opened = http->Open("POST", url); !opened) {
            return std::unexpected(std::string("Failed to connect to explain URL"));
        }
        auto status = http->GetStatusCode();
        if (!status || *status != 200) {
            http->Close();
            return std::unexpected(std::string("Failed to upload phone camera frame"));
        }
        std::string result = http->ReadAll();
        http->Close();
        return result;
    }

private:
    std::mutex state_mutex_;
    std::string jpeg_;
    std::string explain_url_;
    std::string explain_token_;
};

// The dev-board RGB (GPIO 48) is a standard GRB SK6812/WS2812. SingleLed drives
// it as GRB too, so SetColor(r,g,b) maps directly: (0,g,0) -> green, (0,0,b) ->
// blue, (r,0,0) -> red. Keep the state -> colour mapping identical to a real
// board; only the brightness is tuned here. Auto-listen keeps the device in
// Listening while the phone App is open, so a full green would stay on forever.
// When the wake-word model / real accessories arrive and auto-listen is removed,
// these two constants are the only values that need re-tuning.
constexpr uint8_t kListeningLedLevel = 1;
constexpr uint8_t kSpeakingLedLevel = 4;

class PhoneAvLed : public SingleLed {
public:
    explicit PhoneAvLed(gpio_num_t gpio) : SingleLed(gpio) {}

    void OnStateChanged() override {
        auto& app = Application::GetInstance();
        switch (app.GetDeviceState()) {
            case kDeviceStateIdle:
                TurnOff();
                break;
            case kDeviceStateListening:
            case kDeviceStateAudioTesting:
                SetColor(0, kListeningLedLevel, 0);
                TurnOn();
                break;
            case kDeviceStateSpeaking:
            case kDeviceStateNotifying:
                SetColor(0, 0, kSpeakingLedLevel);
                TurnOn();
                break;
            default:
                // starting / configuring / activating / upgrading / connecting:
                // keep the light off to avoid a distracting blink.
                TurnOff();
                break;
        }
    }
};

class PhoneAvBoard : public WifiBoard {
public:
    PhoneAvBoard() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeDisplay();

        // Standard xiaozhi behaviour: a tap on the BOOT button enters WiFi
        // config mode while the device is still starting (so you can re-bind /
        // factory-reset from the captive portal at http://192.168.4.1), and
        // toggles the chat during normal operation. Without this the board has
        // no way to reach the setup page (port 80 is not otherwise reachable).
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
        boot_button_.OnLongPress(
            [this]() { Application::GetInstance().Schedule([this]() { EnterWifiConfigMode(); }); });

        // Keep UART0 logging enabled. The RGB status LED is on GPIO 48 (WS2812)
        // and is unrelated to the USB-UART TX activity LED; serial logs are the
        // only way to debug the phone audio stream on this board.
        // This board ships without a wake-word model (the assets partition is
        // disabled), so the default wake-word activation can never fire and the
        // device would sit idle forever. Instead, once the device is online and
        // activated, automatically start a conversation — no button needed.
        // A slow periodic timer re-arms it whenever the device is back to Idle
        // (e.g. after a reply or a dropped connection), keeping the chat going.
        // It must only call ToggleChatState(): the stock state machine then runs
        // Idle -> Connecting -> OpenAudioChannel() (hello / session_id / UDP) ->
        // Listening. Setting the state to Listening directly here would skip that
        // handshake and the next toggle would immediately close the channel.
        constexpr uint64_t kAutoListenIntervalUs = 1000000;  // 1 s
        esp_timer_create_args_t auto_listen_timer_args = {
            .callback =
                [](void* arg) {
                    auto* board = static_cast<PhoneAvBoard*>(arg);
                    if (!board->IsNetworkReady()) {
                        return;
                    }
                    // Only start a conversation when a microphone is actually
                    // available: the phone mic stream is open, or a local I2S/PDM
                    // mic initialized successfully. Otherwise entering listening
                    // would immediately fail and the device would blink forever
                    // in an idle<->listening loop.
                    if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
                        auto* codec = static_cast<PhoneAvAudioCodec*>(board->GetAudioCodec());
                        if (codec != nullptr && codec->IsMicAvailable()) {
                            // Let HandleToggleChatEvent() open the audio channel through
                            // the standard Connecting state. Do NOT call
                            // SetListeningMode() here: it would jump straight to
                            // Listening without a hello/session_id/UDP handshake,
                            // and the toggle would then close the channel at once.
                            Application::GetInstance().ToggleChatState();
                        }
                    }
                },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "auto_listen",
            .skip_unhandled_events = true,
        };
        esp_timer_create(&auto_listen_timer_args, &auto_listen_timer_);
        esp_timer_start_periodic(auto_listen_timer_, kAutoListenIntervalUs);

        // Construct the codec here (on the main task) instead of letting the
        // first status/auto-listen callback do it on the small esp_timer or
        // httpd stack. Hardware I2S initialization happens in the constructor.
        AudioCodecPtr();
    }

    virtual void SetNetworkEventCallback(NetworkEventCallback callback) override {
        user_network_event_callback_ = std::move(callback);
        WifiBoard::SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
            network_ready_ = (event == NetworkEvent::Connected);
            // This page is the board's only "screen" (activation code, chat
            // subtitles, Phone AV host), so keep it reachable both on a good
            // STA link and in config AP mode (192.168.4.1), when WiFi is broken
            // and the page is needed most.
            // httpd_start must NOT run any earlier than a network event:
            // Application calls SetNetworkEventCallback before StartNetwork(),
            // while lwIP is not initialised yet, and httpd_start there asserts
            // "tcpip_send_msg_wait_sem ... (Invalid mbox)" and reboot-loops.
            // Idempotent; re-attempts only if an earlier httpd_start failed.
            if (event == NetworkEvent::Connected ||
                event == NetworkEvent::WifiConfigModeEnter) {
                StartPhoneAvConfigServer();
            }
            if (user_network_event_callback_) {
                user_network_event_callback_(event, data);
            }
        });
    }

    bool IsNetworkReady() const {
        return network_ready_.load();
    }

    bool IsPhoneMicConnected() {
        auto* codec = AudioCodecPtr();
        return codec != nullptr && codec->IsPhoneMicConnected();
    }
    bool IsMicAvailable() {
        auto* codec = AudioCodecPtr();
        return codec != nullptr && codec->IsMicAvailable();
    }
    uint32_t MicConnectCount() {
        auto* codec = AudioCodecPtr();
        return codec != nullptr ? codec->MicConnectCount() : 0;
    }
    uint32_t MicDisconnectCount() {
        auto* codec = AudioCodecPtr();
        return codec != nullptr ? codec->MicDisconnectCount() : 0;
    }
    uint32_t UptimeMs() const {
        return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    }

    PhoneAvSource SelectedMicSource() {
        auto* codec = AudioCodecPtr();
        return codec != nullptr ? codec->SelectedMicSource() : PhoneAvSource::kPhone;
    }
    PhoneAvSource SelectedSpkSource() {
        auto* codec = AudioCodecPtr();
        return codec != nullptr ? codec->SelectedSpkSource() : PhoneAvSource::kPhone;
    }
    PhoneAvSource ActiveMicSource() {
        auto* codec = AudioCodecPtr();
        return codec != nullptr ? codec->ActiveMicSource() : PhoneAvSource::kPhone;
    }
    PhoneAvSource ActiveSpkSource() {
        auto* codec = AudioCodecPtr();
        return codec != nullptr ? codec->ActiveSpkSource() : PhoneAvSource::kPhone;
    }
    bool HwMicActive() {
        auto* codec = AudioCodecPtr();
        return codec != nullptr && codec->HwMicActive();
    }
    bool HwSpkActive() {
        auto* codec = AudioCodecPtr();
        return codec != nullptr && codec->HwSpkActive();
    }
    bool HwDisplayActive() const { return hw_display_ != nullptr; }

    virtual AudioCodec* GetAudioCodec() override {
        static PhoneAvAudioCodec codec;
        return &codec;
    }

    virtual Camera* GetCamera() override {
        static NetworkCamera camera;
        return &camera;
    }

    virtual Display* GetDisplay() override {
        return display_router_.get();
    }

    virtual Backlight* GetBacklight() override { return backlight_; }

    virtual Led* GetLed() override {
        static PhoneAvLed led(BUILTIN_LED_GPIO);
        return &led;
    }

private:
    PhoneAvAudioCodec* AudioCodecPtr() {
        return static_cast<PhoneAvAudioCodec*>(GetAudioCodec());
    }

    void InitializeDisplay() {
        const PhoneAvSource source = PhoneAvGetSource(kPhoneAvSrcDispKey);
        if (source == PhoneAvSource::kHardware) {
            if (!PhoneAvHwDispConfigured()) {
                ESP_LOGE(TAG,
                         "Local display selected but PHONE_AV_HW_DISP_* is not configured; "
                         "using the 8080 screen");
            } else if (!InitializeHardwareDisplay()) {
                ESP_LOGE(TAG, "Local display init failed; falling back to the 8080 screen");
                Settings settings(kPhoneAvConfigNs, true);
                settings.SetString(kPhoneAvSrcDispKey,
                                   PhoneAvSourceToken(PhoneAvSource::kPhone));
            }
        }
        display_router_ = std::make_unique<PhoneAvDisplayRouter>(hw_display_);
    }

    bool InitializeHardwareDisplay() {
#if PHONE_AV_HW_DISP_TYPE == 0
        return false;
#else
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = PHONE_AV_HW_DISP_MOSI;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = PHONE_AV_HW_DISP_CLK;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz =
            PHONE_AV_HW_DISP_WIDTH * PHONE_AV_HW_DISP_HEIGHT * sizeof(uint16_t);
        esp_err_t err = spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize display SPI bus: %s", esp_err_to_name(err));
            return false;
        }

        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = PHONE_AV_HW_DISP_CS;
        io_config.dc_gpio_num = PHONE_AV_HW_DISP_DC;
        io_config.spi_mode = PHONE_AV_HW_DISP_SPI_MODE;
        io_config.pclk_hz = PHONE_AV_HW_DISP_PCLK_HZ;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        err = esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create display panel IO: %s", esp_err_to_name(err));
            return false;
        }

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = PHONE_AV_HW_DISP_RST;
        panel_config.rgb_ele_order = PHONE_AV_HW_DISP_RGB_ORDER
                                         ? LCD_RGB_ELEMENT_ORDER_BGR
                                         : LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
#if PHONE_AV_HW_DISP_TYPE == 2
        err = esp_lcd_new_panel_ili9341(panel_io, &panel_config, &panel);
#elif PHONE_AV_HW_DISP_TYPE == 3
        err = esp_lcd_new_panel_gc9a01(panel_io, &panel_config, &panel);
#else
        err = esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel);
#endif
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create display panel: %s", esp_err_to_name(err));
            esp_lcd_panel_io_del(panel_io);
            return false;
        }

        esp_lcd_panel_reset(panel);
        err = esp_lcd_panel_init(panel);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize display panel: %s", esp_err_to_name(err));
            esp_lcd_panel_del(panel);
            esp_lcd_panel_io_del(panel_io);
            return false;
        }
        esp_lcd_panel_invert_color(panel, PHONE_AV_HW_DISP_INVERT);
        esp_lcd_panel_swap_xy(panel, PHONE_AV_HW_DISP_SWAP_XY);
        esp_lcd_panel_mirror(panel, PHONE_AV_HW_DISP_MIRROR_X, PHONE_AV_HW_DISP_MIRROR_Y);

        hw_display_ = new SpiLcdDisplay(
            panel_io, panel, PHONE_AV_HW_DISP_WIDTH, PHONE_AV_HW_DISP_HEIGHT,
            PHONE_AV_HW_DISP_OFFSET_X, PHONE_AV_HW_DISP_OFFSET_Y, PHONE_AV_HW_DISP_MIRROR_X,
            PHONE_AV_HW_DISP_MIRROR_Y, PHONE_AV_HW_DISP_SWAP_XY);
        if (PHONE_AV_HW_DISP_BL != GPIO_NUM_NC) {
            backlight_ =
                new PwmBacklight(PHONE_AV_HW_DISP_BL, !PHONE_AV_HW_DISP_BL_ACTIVE_HIGH);
            backlight_->RestoreBrightness();
        }
        ESP_LOGI(TAG, "Local LCD ready (%dx%d, type=%d)", PHONE_AV_HW_DISP_WIDTH,
                 PHONE_AV_HW_DISP_HEIGHT, PHONE_AV_HW_DISP_TYPE);
        return true;
#endif
    }

    std::atomic<bool> network_ready_{false};
    NetworkEventCallback user_network_event_callback_;
    esp_timer_handle_t auto_listen_timer_{nullptr};
    Button boot_button_;
    Display* hw_display_ = nullptr;
    PwmBacklight* backlight_ = nullptr;
    std::unique_ptr<PhoneAvDisplayRouter> display_router_;
};

esp_err_t PhoneAvStatusHandler(httpd_req_t* req) {
    auto& app = Application::GetInstance();
    DeviceState state = app.GetDeviceState();

    const char* state_str = "unknown";
    switch (state) {
        case kDeviceStateIdle:         state_str = "idle"; break;
        case kDeviceStateListening:    state_str = "listening"; break;
        case kDeviceStateSpeaking:     state_str = "speaking"; break;
        case kDeviceStateConnecting:   state_str = "connecting"; break;
        case kDeviceStateActivating:   state_str = "activating"; break;
        case kDeviceStateUpgrading:    state_str = "upgrading"; break;
        case kDeviceStateNotifying:    state_str = "notifying"; break;
        case kDeviceStateAudioTesting: state_str = "audio_testing"; break;
        default:                      state_str = "unknown"; break;
    }

    auto* board = static_cast<PhoneAvBoard*>(&Board::GetInstance());
    const bool mic = board->IsPhoneMicConnected();
    const bool net = board->IsNetworkReady();
    const bool voice = app.IsVoiceDetected();
    const uint32_t mc = board->MicConnectCount();
    const uint32_t md = board->MicDisconnectCount();
    const uint32_t up = board->UptimeMs();
    auto* codec = static_cast<PhoneAvAudioCodec*>(board->GetAudioCodec());
    const uint32_t sp_ok = codec != nullptr ? codec->SpeakerPostOk() : 0;
    const uint32_t sp_fail = codec != nullptr ? codec->SpeakerPostFail() : 0;
    const uint32_t sp_samples = codec != nullptr ? codec->SpeakerSamples() : 0;
    const bool assets_valid = Assets::GetInstance().partition_valid();

    const PhoneAvSource src_spk = board->SelectedSpkSource();
    const PhoneAvSource src_mic = board->SelectedMicSource();
    const PhoneAvSource src_disp = PhoneAvGetSource(kPhoneAvSrcDispKey);
    const PhoneAvSource active_spk = board->ActiveSpkSource();
    const PhoneAvSource active_mic = board->ActiveMicSource();
    const bool active_disp_hw = board->HwDisplayActive();
    const bool hw_spk_configured = PhoneAvHwSpkConfigured();
    const bool hw_mic_configured = PhoneAvHwMicConfigured();
    const bool hw_disp_configured = PhoneAvHwDispConfigured();
    const bool hw_spk_active = board->HwSpkActive();
    const bool hw_mic_active = board->HwMicActive();

    const char* rr = "unknown";
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   rr = "poweron"; break;
        case ESP_RST_SW:        rr = "software"; break;
        case ESP_RST_PANIC:     rr = "panic"; break;
        case ESP_RST_INT_WDT:   rr = "int_wdt"; break;
        case ESP_RST_TASK_WDT:  rr = "task_wdt"; break;
        case ESP_RST_WDT:       rr = "wdt"; break;
        case ESP_RST_DEEPSLEEP: rr = "deepsleep"; break;
        case ESP_RST_BROWNOUT:  rr = "brownout"; break;
        case ESP_RST_SDIO:      rr = "sdio"; break;
        default:                rr = "unknown"; break;
    }

    // Report the address the page is actually reachable on (STA IP, or
    // 192.168.4.1 while in config AP mode) and the phone host we are trying
    // to reach. This makes "the IP did not change but the page is gone"
    // diagnosable without a serial cable.
    std::string local_ip = "0.0.0.0";
    esp_netif_t* netif = esp_netif_get_default_netif();
    esp_netif_ip_info_t info{};
    if (netif != nullptr && esp_netif_get_ip_info(netif, &info) == ESP_OK) {
        char ip_buf[16];
        snprintf(ip_buf, sizeof(ip_buf), IPSTR, IP2STR(&info.ip));
        local_ip = ip_buf;
    }

    auto& screen = PhoneAvScreen();
    std::string screen_status, screen_emotion, screen_theme, screen_chat, screen_notice;
    std::string screen_history, screen_messages;
    bool custom_emoji_collection = false;
    {
        std::lock_guard<std::mutex> lock(screen.mutex);
        screen_status = PhoneAvJsonEscape(screen.status);
        screen_emotion = PhoneAvJsonEscape(screen.emotion);
        screen_theme = PhoneAvJsonEscape(screen.theme);
        custom_emoji_collection = screen.custom_emoji_collection;
        screen_chat = PhoneAvJsonEscape(screen.chat_message);
        screen_notice = PhoneAvJsonEscape(screen.notification);
        screen_history = "[";
        for (size_t i = 0; i < screen.history.size(); ++i) {
            if (i != 0) {
                screen_history += ",";
            }
            screen_history += "\"" + PhoneAvJsonEscape(screen.history[i]) + "\"";
        }
        screen_history += "]";
        screen_messages = "[";
        for (size_t i = 0; i < screen.messages.size(); ++i) {
            if (i != 0) {
                screen_messages += ",";
            }
            screen_messages += "\"" + PhoneAvJsonEscape(screen.messages[i]) + "\"";
        }
        screen_messages += "]";
    }

    std::string body = std::string("{\n") +
        "  \"state\": \"" + state_str + "\",\n" +
        "  \"ip\": \"" + local_ip + "\",\n" +
        "  \"phone_av_host\": \"" + PhoneAvJsonEscape(std::string(PhoneAvHost())) + "\",\n" +
        "  \"source_spk\": \"" + PhoneAvSourceToken(src_spk) + "\",\n" +
        "  \"source_mic\": \"" + PhoneAvSourceToken(src_mic) + "\",\n" +
        "  \"source_disp\": \"" + PhoneAvSourceToken(src_disp) + "\",\n" +
        "  \"active_spk\": \"" + PhoneAvSourceToken(active_spk) + "\",\n" +
        "  \"active_mic\": \"" + PhoneAvSourceToken(active_mic) + "\",\n" +
        "  \"active_disp\": \"" + std::string(active_disp_hw ? "hw" : "phone") + "\",\n" +
        "  \"hw_spk_configured\": " + (hw_spk_configured ? "true" : "false") + ",\n" +
        "  \"hw_mic_configured\": " + (hw_mic_configured ? "true" : "false") + ",\n" +
        "  \"hw_disp_configured\": " + (hw_disp_configured ? "true" : "false") + ",\n" +
        "  \"hw_spk_active\": " + (hw_spk_active ? "true" : "false") + ",\n" +
        "  \"hw_mic_active\": " + (hw_mic_active ? "true" : "false") + ",\n" +
        "  \"hw_disp_active\": " + (active_disp_hw ? "true" : "false") + ",\n" +
        "  \"voice_detected\": " + (voice ? "true" : "false") + ",\n" +
        "  \"phone_mic_connected\": " + (mic ? "true" : "false") + ",\n" +
        "  \"network_ready\": " + (net ? "true" : "false") + ",\n" +
        "  \"mic_connect_count\": " + std::to_string(mc) + ",\n" +
        "  \"mic_disconnect_count\": " + std::to_string(md) + ",\n" +
        "  \"speaker_post_ok\": " + std::to_string(sp_ok) + ",\n" +
        "  \"speaker_post_fail\": " + std::to_string(sp_fail) + ",\n" +
        "  \"speaker_samples\": " + std::to_string(sp_samples) + ",\n" +
        "  \"uptime_ms\": " + std::to_string(up) + ",\n" +
        "  \"reset_reason\": \"" + rr + "\",\n" +
        "  \"assets_valid\": " + (assets_valid ? "true" : "false") + ",\n" +
        "  \"screen_status\": \"" + screen_status + "\",\n" +
        "  \"screen_emotion\": \"" + screen_emotion + "\",\n" +
        "  \"screen_theme\": \"" + screen_theme + "\",\n" +
        "  \"screen_emoji_collection\": " + (custom_emoji_collection ? "true" : "false") + ",\n" +
        "  \"screen_chat_message\": \"" + screen_chat + "\",\n" +
        "  \"screen_notification\": \"" + screen_notice + "\",\n" +
        "  \"screen_messages\": " + screen_messages + ",\n" +
        "  \"screen_history\": " + screen_history + "\n" +
        "}";
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_send(req, body.c_str(), HTTPD_RESP_USE_STRLEN);
}

}  // namespace

// Must stay outside the anonymous namespace: phone_av_audio.cc calls this
// function, so it needs external linkage.
bool PhoneAvNetworkReady() {
    return static_cast<PhoneAvBoard&>(Board::GetInstance()).IsNetworkReady();
}

DECLARE_BOARD(PhoneAvBoard);
