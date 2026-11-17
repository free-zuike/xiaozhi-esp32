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
#define PHONE_AV_HW_SPK_BCLK GPIO_NUM_NC
#endif
#ifndef PHONE_AV_HW_SPK_LRCK
#define PHONE_AV_HW_SPK_LRCK GPIO_NUM_NC
#endif
#ifndef PHONE_AV_HW_SPK_DIN
#define PHONE_AV_HW_SPK_DIN GPIO_NUM_NC
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
#define PHONE_AV_HW_MIC_SCK GPIO_NUM_NC
#endif
#ifndef PHONE_AV_HW_MIC_WS
#define PHONE_AV_HW_MIC_WS GPIO_NUM_NC
#endif
#ifndef PHONE_AV_HW_MIC_DIN
#define PHONE_AV_HW_MIC_DIN GPIO_NUM_NC
#endif
#ifndef PHONE_AV_HW_MIC_SLOT_RIGHT
#define PHONE_AV_HW_MIC_SLOT_RIGHT 0
#endif

/* Screen: 0 = none, 1 = ST7789, 2 = ILI9341, 3 = GC9A01.
 * Only the pins used by the selected controller need to be valid; RST and BL
 * may stay GPIO_NUM_NC if they are tied in hardware. */
#ifndef PHONE_AV_HW_DISP_TYPE
#define PHONE_AV_HW_DISP_TYPE 0
#endif
#ifndef PHONE_AV_HW_DISP_WIDTH
#define PHONE_AV_HW_DISP_WIDTH 240
#endif
#ifndef PHONE_AV_HW_DISP_HEIGHT
#define PHONE_AV_HW_DISP_HEIGHT 240
#endif
#ifndef PHONE_AV_HW_DISP_MOSI
#define PHONE_AV_HW_DISP_MOSI GPIO_NUM_NC
#endif
#ifndef PHONE_AV_HW_DISP_CLK
#define PHONE_AV_HW_DISP_CLK GPIO_NUM_NC
#endif
#ifndef PHONE_AV_HW_DISP_CS
#define PHONE_AV_HW_DISP_CS GPIO_NUM_NC
#endif
#ifndef PHONE_AV_HW_DISP_DC
#define PHONE_AV_HW_DISP_DC GPIO_NUM_NC
#endif
#ifndef PHONE_AV_HW_DISP_RST
#define PHONE_AV_HW_DISP_RST GPIO_NUM_NC
#endif
#ifndef PHONE_AV_HW_DISP_BL
#define PHONE_AV_HW_DISP_BL GPIO_NUM_NC
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
#define PHONE_AV_HW_DISP_INVERT 0
#endif
/* 0 = RGB element order, 1 = BGR. */
#ifndef PHONE_AV_HW_DISP_RGB_ORDER
#define PHONE_AV_HW_DISP_RGB_ORDER 0
#endif
#ifndef PHONE_AV_HW_DISP_SPI_MODE
#define PHONE_AV_HW_DISP_SPI_MODE 0
#endif
#ifndef PHONE_AV_HW_DISP_PCLK_HZ
#define PHONE_AV_HW_DISP_PCLK_HZ (40 * 1000 * 1000)
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

#endif
