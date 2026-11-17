# phone-av 板

这块板默认把 **麦克风、喇叭、摄像头** 都交给手机 App（android-ip-camera），
8080 页面上的“虚拟屏”显示 xiaozhi 的状态、验证码和聊天记录。

现在 8080 页面还可以为 **喇叭、录音、屏幕** 分别选择来源：

- **手机**：保持原来的网络音频/虚拟屏路径。
- **本地硬件**：使用 ESP32-S3 上实际连接的 I2S 功放、I2S/PDM 麦克风、SPI LCD。

选择保存在 NVS 里，**保存后板子会自动重启**，重启后生效。这样 AFE 的通道格式、
LVGL 和 I2S 只在启动时初始化一次，避免运行时热切换把已经稳定的音频链路弄坏。

## 8080 页面

启动后访问：

```
http://<板子IP>:8080/
```

页面里的“能力来源”：

```
喇叭:  (o) 手机   ( ) 本地功放
录音:  (o) 手机   ( ) 本地麦克风
屏幕:  (o) 8080 虚拟屏   ( ) 本地 LCD
```

未在 `config.h` 里配置引脚的能力会显示“未配置”，硬件选项被禁用。保存时如果
选了未配置的硬件，服务端也会拒绝并给出提示。

NVS 命名空间 `phone_av`：

| key | 含义 | 默认 |
| --- | --- | --- |
| `host` | 手机 IP/域名 | `192.168.4.2` |
| `src_spk` | 喇叭来源，`phone` / `hw` | `phone` |
| `src_mic` | 录音来源，`phone` / `hw` | `phone` |
| `src_disp` | 屏幕来源，`phone` / `hw` | `phone` |

`/status` 会返回 `source_*`（已选）、`active_*`（实际生效，初始化失败回退后不同）、
`hw_*_configured`、`hw_*_active` 等字段，方便判断是“选了但没初始化成功”还是
“根本没配置”。

## 配置硬件

所有引脚都在 `main/boards/phone-av/config.h` 里，默认是 `GPIO_NUM_NC`（未接）。
填好引脚后重新编译，再到 8080 选择本地硬件。

### 喇叭（I2S 功放）

以 MAX98357A 为例：

```c
#define PHONE_AV_HW_SPK_BCLK GPIO_NUM_4
#define PHONE_AV_HW_SPK_LRCK GPIO_NUM_5
#define PHONE_AV_HW_SPK_DIN  GPIO_NUM_6
#define PHONE_AV_HW_SPK_EN   GPIO_NUM_7   // SD/EN，可选；不接则保持 GPIO_NUM_NC
#define PHONE_AV_HW_SPK_SLOT_RIGHT 0      // 左 slot；右 slot 改成 1
```

接线要点：

- MAX98357A 的 `VIN` 接 5V（或模块允许的电压），`GND` 共地。
- `SD` 不能悬空；如果不用 GPIO 控制，直接接 3.3V/5V 使能。
- 喇叭接 `+` / `-`，不要接地。
- PCM5102 等 I2S DAC 同样适用，只是可能还需要 `SCK/MCLK`，按模块说明接。

### 麦克风（I2S 数字麦或 PDM）

INMP441（I2S）示例：

```c
#define PHONE_AV_HW_MIC_PDM 0
#define PHONE_AV_HW_MIC_SCK GPIO_NUM_15
#define PHONE_AV_HW_MIC_WS  GPIO_NUM_16
#define PHONE_AV_HW_MIC_DIN GPIO_NUM_17
#define PHONE_AV_HW_MIC_SLOT_RIGHT 0
```

- INMP441 的 `L/R` 接 GND 对应左 slot（`MIC_SLOT_RIGHT=0`）；接 VDD 对应右 slot。
- `VDD` 3.3V，`GND` 共地。

PDM 麦克风：

```c
#define PHONE_AV_HW_MIC_PDM 1
#define PHONE_AV_HW_MIC_SCK GPIO_NUM_15
#define PHONE_AV_HW_MIC_DIN GPIO_NUM_16
// 此时 PHONE_AV_HW_MIC_WS 不用填
```

### 屏幕（SPI LCD）

`PHONE_AV_HW_DISP_TYPE`：`1` = ST7789，`2` = ILI9341，`3` = GC9A01，`0` = 不接。

```c
#define PHONE_AV_HW_DISP_TYPE   1
#define PHONE_AV_HW_DISP_WIDTH  240
#define PHONE_AV_HW_DISP_HEIGHT 240
#define PHONE_AV_HW_DISP_MOSI   GPIO_NUM_11
#define PHONE_AV_HW_DISP_CLK    GPIO_NUM_12
#define PHONE_AV_HW_DISP_CS     GPIO_NUM_10
#define PHONE_AV_HW_DISP_DC     GPIO_NUM_13
#define PHONE_AV_HW_DISP_RST    GPIO_NUM_14   // 可 GPIO_NUM_NC
#define PHONE_AV_HW_DISP_BL     GPIO_NUM_21   // 可 GPIO_NUM_NC
```

如果屏幕颜色不对，调整：

- `PHONE_AV_HW_DISP_INVERT`：1 = 反色。
- `PHONE_AV_HW_DISP_MIRROR_X` / `MIRROR_Y`：镜像。
- `PHONE_AV_HW_DISP_SWAP_XY`：横竖屏交换。
- `PHONE_AV_HW_DISP_RGB_ORDER`：0 = RGB，1 = BGR。
- `PHONE_AV_HW_DISP_OFFSET_X/Y`：面板偏移。

SPI 使用 `SPI3_HOST`，默认 40 MHz；屏幕花屏时先把
`PHONE_AV_HW_DISP_PCLK_HZ` 降到 20 MHz。

> 注意：`config.json` 默认设置了 `CONFIG_FLASH_NONE_ASSETS=y`。如果要让本地 LCD
> 显示完整的表情/主题资源，需要把 assets 选项改回启用（或保留内置字体和占位
> emoji）。8080 页面的虚拟屏不受影响，它直接读 assets 分区，没有资源时显示占位。

## 混搭示例

- **只接了功放**：喇叭选“本地功放”，录音/屏幕保持“手机”。手机 App 继续提供麦克风，
  TTS 从本地功放输出。
- **只接了麦克风**：录音选“本地麦克风”，喇叭/屏幕走手机。
- **全部本地**：喇叭、录音、屏幕都选硬件；手机只保留摄像头（当前仍走手机
  `/video/snapshot`）。此时 8080 页面仍然可以查看状态和修改配置。

## 状态与排障

- 8080 页面顶部会显示 `active_*`，例如 `喇叭=手机 录音=本地硬件 屏幕=手机`。
- `/status` 里：
  - `source_*` 是 NVS 里的选择；
  - `active_*` 是启动时实际生效的来源；
  - `hw_*_configured` 是编译期是否填了引脚；
  - `hw_*_active` 是本地硬件是否真的初始化成功。
- 选了硬件但 `active_*` 还是 `phone`：看串口日志里的 `PhoneAvAudio` / `PhoneAvHwAudio`
  错误；常见原因是引脚没填、I2S 通道创建失败、屏幕 panel 初始化失败。
- 本地喇叭有声但很小：调 `output_volume`（MCP 音量工具/设置），并检查功放增益、
  喇叭阻抗和供电。
- 本地麦克风没有声音：确认 `L/R`/slot 设置、`SCK/WS/DIN` 没接反、麦克风供电，
  以及 `PHONE_AV_HW_MIC_PDM` 与实际模块一致。
- 屏幕白屏/花屏：优先降 SPI 频率，再核对 `OFFSET`、`MIRROR`、`SWAP_XY`、
  `INVERT`、`RGB_ORDER`。

## 注意

- 引脚是编译期配置；8080 只切换来源，不做 GPIO 重映射。
- 切换来源会重启板子；WiFi 凭据和 NVS 其它配置不会丢。
- 目前摄像头仍固定走手机；本地摄像头不在这一版范围内。
