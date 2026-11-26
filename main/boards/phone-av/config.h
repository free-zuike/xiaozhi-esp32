#ifndef _PHONE_AV_BOARD_CONFIG_H_
#define _PHONE_AV_BOARD_CONFIG_H_

#include <driver/gpio.h>

/* Both xiaozhi and the Stage 2 phone bridge use 16 kHz mono 16-bit PCM,
 * so no resampling is needed in either direction. */
#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 16000

/* android-ip-camera HTTP server. This is only the factory default: the live
 * phone address is kept in NVS (namespace "phone_av", key "host") and can be
 * changed at runtime from the board's LAN config page:
 *     http://<board-ip>:8080/
 * No rebuild or reflash is needed when the phone's LAN IP changes.
 * 192.168.4.2 is the usual DHCP lease when the phone joins the ESP32 AP.
 * For the LAN profile, disable TLS and Basic Auth in the App settings. */
#define PHONE_AV_HOST "192.168.4.2"
#define PHONE_AV_PORT 4444

#define PHONE_AV_MIC_PATH      "/audio/pcm"
/* The phone app exposes /audio/upload/16000 for 16 kHz mono PCM and
 * /audio/upload for 44.1 kHz. AUDIO_OUTPUT_SAMPLE_RATE is 16000, so using the
 * plain /audio/upload would make the phone's AudioTrack consume the stream
 * 2.75x too fast (underrun / no audible sound). See
 * StreamingServerHelper.kt handleTalkUpload(). */
#define PHONE_AV_SPEAKER_PATH "/audio/upload/16000"
#define PHONE_AV_SNAPSHOT_PATH "/video/snapshot"

#define BUILTIN_LED_GPIO GPIO_NUM_48
#define BOOT_BUTTON_GPIO GPIO_NUM_0

/* ---------------------------------------------------------------------------
 * Optional local hardware
 *
 * Every capability defaults to "not wired": the firmware keeps using the phone
 * for it.  Fill in the pins below for the hardware you actually connected,
 * rebuild, then choose "本地硬件" for that capability on
 * http://<board-ip>:8080/ (the choice is stored in NVS and takes effect after
 * the automatic reboot).
 *
 * All pins are compile-time, like the other boards in this repository; the
 * 8080 page only selects the source, it does not remap GPIOs.
 * ------------------------------------------------------------------------- */

/* Speaker: I2S amplifier (MAX98357A, PCM5102, ...).
 * SPK_EN is the optional SD/EN/shutdown pin; it is driven high while playing.
 * Set SPK_SLOT_RIGHT to 1 if the amplifier takes the right I2S slot. */
#ifndef PHONE_AV_HW_SPK_BCLK
#define PHONE_AV_HW_SPK_BCLK GPIO_NUM_15
#endif
#ifndef PHONE_AV_HW_SPK_LRCK
#define PHONE_AV_HW_SPK_LRCK GPIO_NUM_16
#endif
#ifndef PHONE_AV_HW_SPK_DIN
#define PHONE_AV_HW_SPK_DIN GPIO_NUM_7
#endif
#ifndef PHONE_AV_HW_SPK_EN
#define PHONE_AV_HW_SPK_EN GPIO_NUM_NC
#endif
#ifndef PHONE_AV_HW_SPK_SLOT_RIGHT
#define PHONE_AV_HW_SPK_SLOT_RIGHT 0
#endif

/* Microphone: I2S digital mic (INMP441, ICS-43434, ...) or PDM mic.
 * Set PHONE_AV_HW_MIC_PDM to 1 for PDM (WS is then unused).
 * MIC_SLOT_RIGHT selects the I2S slot for a digital mic; tie the mic's L/R
 * pin accordingly (INMP441: L/R low = left slot). */
#ifndef PHONE_AV_HW_MIC_PDM
#define PHONE_AV_HW_MIC_PDM 0
#endif
#ifndef PHONE_AV_HW_MIC_SCK
#define PHONE_AV_HW_MIC_SCK GPIO_NUM_17
#endif
#ifndef PHONE_AV_HW_MIC_WS
#define PHONE_AV_HW_MIC_WS GPIO_NUM_18
#endif
#ifndef PHONE_AV_HW_MIC_DIN
#define PHONE_AV_HW_MIC_DIN GPIO_NUM_21
#endif
#ifndef PHONE_AV_HW_MIC_SLOT_RIGHT
#define PHONE_AV_HW_MIC_SLOT_RIGHT 0
#endif
/* I2S mic samples arrive as 24-bit data in a 32-bit slot. INMP441 is left
 * justified, so >>16 converts to 16-bit without clipping; a 32-bit mic that
 * really uses the low bits would need a smaller shift. */
#ifndef PHONE_AV_HW_MIC_SHIFT
#define PHONE_AV_HW_MIC_SHIFT 16
#endif
/* INMP441 raw speech measured around RMS 256 after the 24->16 bit shift.
 * Apply a DC blocker and gain so the AFE/VAD sees a normal level. */
#ifndef PHONE_AV_HW_MIC_GAIN
#define PHONE_AV_HW_MIC_GAIN 8.0f
#endif

/* Screen: 0 = none, 1 = ST7789, 2 = ILI9341, 3 = GC9A01.
 * Only the pins used by the selected controller need to be valid; RST and BL
 * may stay GPIO_NUM_NC if they are tied in hardware. */
#ifndef PHONE_AV_HW_DISP_TYPE
#define PHONE_AV_HW_DISP_TYPE 1
#endif
#ifndef PHONE_AV_HW_DISP_WIDTH
#define PHONE_AV_HW_DISP_WIDTH 240
#endif
#ifndef PHONE_AV_HW_DISP_HEIGHT
#define PHONE_AV_HW_DISP_HEIGHT 240
#endif
#ifndef PHONE_AV_HW_DISP_MOSI
#define PHONE_AV_HW_DISP_MOSI GPIO_NUM_12
#endif
#ifndef PHONE_AV_HW_DISP_CLK
#define PHONE_AV_HW_DISP_CLK GPIO_NUM_38
#endif
#ifndef PHONE_AV_HW_DISP_CS
#define PHONE_AV_HW_DISP_CS GPIO_NUM_47
#endif
#ifndef PHONE_AV_HW_DISP_DC
#define PHONE_AV_HW_DISP_DC GPIO_NUM_1
#endif
#ifndef PHONE_AV_HW_DISP_RST
#define PHONE_AV_HW_DISP_RST GPIO_NUM_39
#endif
#ifndef PHONE_AV_HW_DISP_BL
#define PHONE_AV_HW_DISP_BL GPIO_NUM_40
#endif
#ifndef PHONE_AV_HW_DISP_BL_ACTIVE_HIGH
#define PHONE_AV_HW_DISP_BL_ACTIVE_HIGH 1
#endif
#ifndef PHONE_AV_HW_DISP_OFFSET_X
#define PHONE_AV_HW_DISP_OFFSET_X 0
#endif
#ifndef PHONE_AV_HW_DISP_OFFSET_Y
#define PHONE_AV_HW_DISP_OFFSET_Y 0
#endif
#ifndef PHONE_AV_HW_DISP_MIRROR_X
#define PHONE_AV_HW_DISP_MIRROR_X 0
#endif
#ifndef PHONE_AV_HW_DISP_MIRROR_Y
#define PHONE_AV_HW_DISP_MIRROR_Y 0
#endif
#ifndef PHONE_AV_HW_DISP_SWAP_XY
#define PHONE_AV_HW_DISP_SWAP_XY 0
#endif
#ifndef PHONE_AV_HW_DISP_INVERT
/* 1.54" ST7789 modules vary: with invert enabled this panel showed cyan,
 * magenta and yellow where red/green/blue were expected. */
#define PHONE_AV_HW_DISP_INVERT 0
#endif
/* 0 = RGB element order, 1 = BGR. */
#ifndef PHONE_AV_HW_DISP_RGB_ORDER
#define PHONE_AV_HW_DISP_RGB_ORDER 0
#endif
#ifndef PHONE_AV_HW_DISP_SPI_MODE
/* This 1.54" module latches commands in mode 0; mode 3 produced only random
 * flicker and no image. */
#define PHONE_AV_HW_DISP_SPI_MODE 0
#endif
#ifndef PHONE_AV_HW_DISP_PCLK_HZ
/* 10 MHz is deliberately conservative: ST7789 modules work at 40 MHz on short
 * PCB traces, but the phone-av board is usually wired with dupont leads where
 * 40 MHz can corrupt the panel init/stream and leave the screen black. Raise
 * this only after the wiring is known-good. */
#define PHONE_AV_HW_DISP_PCLK_HZ (10 * 1000 * 1000)
#endif

/* Auto-listen voice gate. When "自动对话" is enabled on the 8080 page, the
 * board only starts a new turn after the microphone sees sustained speech-like
 * energy for one 60 ms frame. These thresholds are on the raw 16-bit PCM that
 * reaches the audio codec; raise them if ambient noise keeps starting turns. */
#ifndef PHONE_AV_VOICE_RMS_THRESHOLD
#define PHONE_AV_VOICE_RMS_THRESHOLD 500
#endif
#ifndef PHONE_AV_VOICE_PEAK_THRESHOLD
#define PHONE_AV_VOICE_PEAK_THRESHOLD 4000
#endif

/* ---------------------------------------------------------------------------
 * Tank car: TB6612FNG dual H-bridge
 *
 * PWMA/AIN1/AIN2 drive the left track, PWMB/BIN1/BIN2 the right track.
 * STBY must be high for the driver to move; set PHONE_AV_TANK_STBY to
 * GPIO_NUM_NC if you tied it to 3.3V. Set PHONE_AV_TANK_ENABLED to 0 to
 * compile the controller out and not register the MCP tools.
 * ------------------------------------------------------------------------- */
#ifndef PHONE_AV_TANK_ENABLED
#define PHONE_AV_TANK_ENABLED 1
#endif
#ifndef PHONE_AV_TANK_PWMA
#define PHONE_AV_TANK_PWMA GPIO_NUM_4
#endif
#ifndef PHONE_AV_TANK_AIN1
#define PHONE_AV_TANK_AIN1 GPIO_NUM_5
#endif
#ifndef PHONE_AV_TANK_AIN2
#define PHONE_AV_TANK_AIN2 GPIO_NUM_6
#endif
#ifndef PHONE_AV_TANK_PWMB
#define PHONE_AV_TANK_PWMB GPIO_NUM_8
#endif
#ifndef PHONE_AV_TANK_BIN1
#define PHONE_AV_TANK_BIN1 GPIO_NUM_9
#endif
#ifndef PHONE_AV_TANK_BIN2
#define PHONE_AV_TANK_BIN2 GPIO_NUM_10
#endif
#ifndef PHONE_AV_TANK_STBY
#define PHONE_AV_TANK_STBY GPIO_NUM_11
#endif
#ifndef PHONE_AV_TANK_PWM_FREQ_HZ
#define PHONE_AV_TANK_PWM_FREQ_HZ 20000
#endif

/* Optional I2C/sensor pins, exposed for future tools. The board does not
 * initialize the bus yet. */
#ifndef PHONE_AV_I2C_SDA
#define PHONE_AV_I2C_SDA GPIO_NUM_13
#endif
#ifndef PHONE_AV_I2C_SCL
#define PHONE_AV_I2C_SCL GPIO_NUM_14
#endif
/* Battery divider input (ADC1_CH1). Use e.g. 100k/100k for a 4.2 V cell. */
#ifndef PHONE_AV_BATTERY_ADC
#define PHONE_AV_BATTERY_ADC GPIO_NUM_2
#endif

/* Default capability sources when the NVS keys are missing. All three local
 * peripherals are wired on this tank, so default to hardware; switch any of
 * them back to the phone on the 8080 page. */
#ifndef PHONE_AV_DEFAULT_SRC_SPK
#define PHONE_AV_DEFAULT_SRC_SPK "hw"
#endif
#ifndef PHONE_AV_DEFAULT_SRC_MIC
#define PHONE_AV_DEFAULT_SRC_MIC "hw"
#endif
#ifndef PHONE_AV_DEFAULT_SRC_DISP
#define PHONE_AV_DEFAULT_SRC_DISP "hw"
#endif

#endif
