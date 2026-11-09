#ifndef _PHONE_AV_BOARD_CONFIG_H_
#define _PHONE_AV_BOARD_CONFIG_H_

#include <driver/gpio.h>

/* Both xiaozhi and the Stage 2 phone bridge use 16 kHz mono 16-bit PCM,
 * so no resampling is needed in either direction. */
#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 16000

/* android-ip-camera HTTP server. 192.168.4.2 is the usual DHCP lease when
 * the phone joins the ESP32 AP; change to the phone's IP on a home router.
 * For the LAN profile, disable TLS and Basic Auth in the App settings. */
#define PHONE_AV_HOST "192.168.4.2"
#define PHONE_AV_PORT 4444

#define PHONE_AV_MIC_PATH      "/audio/pcm"
#define PHONE_AV_SPEAKER_PATH "/audio/upload/16000"
#define PHONE_AV_SNAPSHOT_PATH "/video/snapshot"

#define BUILTIN_LED_GPIO GPIO_NUM_48
#define BOOT_BUTTON_GPIO GPIO_NUM_0

#endif
