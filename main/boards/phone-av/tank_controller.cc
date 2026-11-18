#include "tank_controller.h"

#include "mcp_server.h"

#include <algorithm>

#include <esp_err.h>
#include <esp_log.h>

#define TAG "PhoneAvTank"

namespace {

bool ValidPin(gpio_num_t pin) {
    return pin != GPIO_NUM_NC;
}

}  // namespace

TankController::TankController() {
#if PHONE_AV_TANK_ENABLED
    if (!ValidPin(PHONE_AV_TANK_PWMA) || !ValidPin(PHONE_AV_TANK_AIN1) ||
        !ValidPin(PHONE_AV_TANK_AIN2) || !ValidPin(PHONE_AV_TANK_PWMB) ||
        !ValidPin(PHONE_AV_TANK_BIN1) || !ValidPin(PHONE_AV_TANK_BIN2)) {
        ESP_LOGW(TAG, "Tank pins are not configured, controller disabled");
        return;
    }

    const gpio_num_t outputs[] = {
        PHONE_AV_TANK_AIN1, PHONE_AV_TANK_AIN2, PHONE_AV_TANK_BIN1,
        PHONE_AV_TANK_BIN2, PHONE_AV_TANK_STBY,
    };
    for (gpio_num_t pin : outputs) {
        if (!ValidPin(pin)) {
            continue;
        }
        if (gpio_set_direction(pin, GPIO_MODE_OUTPUT) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to configure tank GPIO %d", static_cast<int>(pin));
            return;
        }
        gpio_set_level(pin, 0);
    }

    // Timer 1 / channels 1-2: the LCD backlight owns timer 0 / channel 0.
    ledc_timer_config_t timer_config = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_1,
        .freq_hz = PHONE_AV_TANK_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false,
    };
    esp_err_t err = ledc_timer_config(&timer_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure tank PWM timer: %s", esp_err_to_name(err));
        return;
    }

    ledc_channel_config_t channel_config = {};
    channel_config.speed_mode = LEDC_LOW_SPEED_MODE;
    channel_config.intr_type = LEDC_INTR_DISABLE;
    channel_config.timer_sel = LEDC_TIMER_1;
    channel_config.duty = 0;
    channel_config.hpoint = 0;

    channel_config.gpio_num = PHONE_AV_TANK_PWMA;
    channel_config.channel = LEDC_CHANNEL_1;
    err = ledc_channel_config(&channel_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure left PWM: %s", esp_err_to_name(err));
        return;
    }

    channel_config.gpio_num = PHONE_AV_TANK_PWMB;
    channel_config.channel = LEDC_CHANNEL_2;
    err = ledc_channel_config(&channel_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure right PWM: %s", esp_err_to_name(err));
        return;
    }

    if (ValidPin(PHONE_AV_TANK_STBY)) {
        gpio_set_level(PHONE_AV_TANK_STBY, 1);
    }

    esp_timer_create_args_t timer_args = {
        .callback = TimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "tank_stop",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&timer_args, &stop_timer_) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create tank stop timer");
        return;
    }

    enabled_ = true;

    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddTool(
        "self.tank.move",
        "控制坦克小车移动。direction 取 forward/backward/left/right/stop；"
        "speed 为 0-100；duration_ms 为 100-10000，到期自动停止。"
        "left/right 是原地转向。",
        PropertyList({
            Property("direction", kPropertyTypeString, "forward"),
            Property("speed", kPropertyTypeInteger, 60, 0, 100),
            Property("duration_ms", kPropertyTypeInteger, 1000, 100, 10000),
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            return Move(properties["direction"].value<std::string>(),
                        properties["speed"].value<int>(),
                        properties["duration_ms"].value<int>());
        });

    mcp_server.AddTool("self.tank.stop", "立即停止坦克小车", PropertyList(),
                       [this](const PropertyList&) -> ReturnValue {
                           Stop();
                           return true;
                       });

    mcp_server.AddTool("self.tank.status", "获取坦克小车当前方向和速度", PropertyList(),
                       [this](const PropertyList&) -> ReturnValue {
                           return StatusJson();
                       });

    ESP_LOGI(TAG, "Tank ready (PWMA=%d AIN=%d/%d PWMB=%d BIN=%d/%d STBY=%d)",
             PHONE_AV_TANK_PWMA, PHONE_AV_TANK_AIN1, PHONE_AV_TANK_AIN2,
             PHONE_AV_TANK_PWMB, PHONE_AV_TANK_BIN1, PHONE_AV_TANK_BIN2,
             PHONE_AV_TANK_STBY);
#else
    ESP_LOGI(TAG, "Tank controller disabled by PHONE_AV_TANK_ENABLED=0");
#endif
}

TankController::~TankController() {
#if PHONE_AV_TANK_ENABLED
    if (!enabled_) {
        return;
    }
    Stop();
    if (stop_timer_ != nullptr) {
        esp_timer_delete(stop_timer_);
        stop_timer_ = nullptr;
    }
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 0);
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2, 0);
    if (ValidPin(PHONE_AV_TANK_STBY)) {
        gpio_set_level(PHONE_AV_TANK_STBY, 0);
    }
#endif
}

bool TankController::Move(const std::string& direction, int speed, int duration_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_) {
        return false;
    }

    const int clamped_speed = std::clamp(speed, 0, 100);
    if (direction == "stop" || clamped_speed == 0) {
        StopLocked();
        return true;
    }

    bool left_forward = false;
    bool right_forward = false;
    if (direction == "forward") {
        left_forward = true;
        right_forward = true;
    } else if (direction == "backward") {
        left_forward = false;
        right_forward = false;
    } else if (direction == "left") {
        left_forward = false;
        right_forward = true;
    } else if (direction == "right") {
        left_forward = true;
        right_forward = false;
    } else {
        ESP_LOGW(TAG, "Unknown tank direction: %s", direction.c_str());
        return false;
    }

    SetLeft(left_forward, clamped_speed);
    SetRight(right_forward, clamped_speed);
    direction_ = direction;
    speed_ = clamped_speed;

    if (stop_timer_ != nullptr) {
        esp_timer_stop(stop_timer_);
        const int clamped_duration = std::clamp(duration_ms, 100, 10000);
        esp_timer_start_once(stop_timer_,
                             static_cast<uint64_t>(clamped_duration) * 1000ULL);
    }
    return true;
}

void TankController::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    StopLocked();
}

void TankController::StopLocked() {
    if (stop_timer_ != nullptr) {
        esp_timer_stop(stop_timer_);
    }
    SetPwm(LEDC_CHANNEL_1, 0);
    SetPwm(LEDC_CHANNEL_2, 0);
    if (ValidPin(PHONE_AV_TANK_AIN1)) {
        gpio_set_level(PHONE_AV_TANK_AIN1, 0);
    }
    if (ValidPin(PHONE_AV_TANK_AIN2)) {
        gpio_set_level(PHONE_AV_TANK_AIN2, 0);
    }
    if (ValidPin(PHONE_AV_TANK_BIN1)) {
        gpio_set_level(PHONE_AV_TANK_BIN1, 0);
    }
    if (ValidPin(PHONE_AV_TANK_BIN2)) {
        gpio_set_level(PHONE_AV_TANK_BIN2, 0);
    }
    direction_ = "stop";
    speed_ = 0;
}

void TankController::SetLeft(bool forward, int speed) {
    if (ValidPin(PHONE_AV_TANK_AIN1)) {
        gpio_set_level(PHONE_AV_TANK_AIN1, forward ? 1 : 0);
    }
    if (ValidPin(PHONE_AV_TANK_AIN2)) {
        gpio_set_level(PHONE_AV_TANK_AIN2, forward ? 0 : 1);
    }
    SetPwm(LEDC_CHANNEL_1, speed);
}

void TankController::SetRight(bool forward, int speed) {
    if (ValidPin(PHONE_AV_TANK_BIN1)) {
        gpio_set_level(PHONE_AV_TANK_BIN1, forward ? 1 : 0);
    }
    if (ValidPin(PHONE_AV_TANK_BIN2)) {
        gpio_set_level(PHONE_AV_TANK_BIN2, forward ? 0 : 1);
    }
    SetPwm(LEDC_CHANNEL_2, speed);
}

void TankController::SetPwm(ledc_channel_t channel, int speed) {
    const int clamped = std::clamp(speed, 0, 100);
    const uint32_t duty = (1023U * static_cast<uint32_t>(clamped)) / 100U;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, channel, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, channel);
}

std::string TankController::StatusJson() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string json = "{\"enabled\":";
    json += enabled_ ? "true" : "false";
    json += ",\"direction\":\"";
    json += direction_;
    json += "\",\"speed\":";
    json += std::to_string(speed_);
    json += "}";
    return json;
}

void TankController::TimerCallback(void* arg) {
    static_cast<TankController*>(arg)->Stop();
}
