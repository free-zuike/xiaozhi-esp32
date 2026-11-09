#ifndef _PHONE_AV_BOARD_CONFIG_H_
#define _PHONE_AV_BOARD_CONFIG_H_

#include <driver/gpio.h>

/* Audio format expected by the xiaozhi audio engine. The phone streams
 * 44.1 kHz PCM, so the network codec resamples in both directions. */
#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

/* android-ip-camera HTTP server. 192.168.4.2 is the usual DHCP lease when
 * the phone joins the ESP32 AP; change to the phone's IP on a home router. */
#define PHONE_AV_HOST "192.168.4.2"
#define PHONE_AV_PORT 4444

#define BUILTIN_LED_GPIO GPIO_NUM_48
#define BOOT_BUTTON_GPIO GPIO_NUM_0

#endif
