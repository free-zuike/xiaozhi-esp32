#ifndef _PHONE_AV_PIN_MAP_CHECK_H_
#define _PHONE_AV_PIN_MAP_CHECK_H_

#include "config.h"

#include <cstddef>

/* Compile-time guard for the phone-av/tank wiring.
 *
 * Every peripheral pin lives in config.h. This header cross-checks the whole
 * map in one place:
 *   - no two peripherals may use the same GPIO
 *   - no peripheral may use a pin reserved by the ESP32-S3 module/board
 *     (flash/PSRAM 26-37, USB 19/20, UART0 43/44, strapping 0/3/45/46,
 *      on-board RGB LED 48)
 *
 * GPIO39-42 are the JTAG pads. They are allowed because this board does not
 * use JTAG; the ST7789 RST/BL currently use 39/40. If you enable JTAG, move
 * those two to 3.3V and set the macros to GPIO_NUM_NC.
 */
namespace phone_av_pins {

struct PinUse {
    const char* name;
    int pin;
};

constexpr PinUse kPins[] = {
    {"spk_bclk", PHONE_AV_HW_SPK_BCLK},
    {"spk_lrck", PHONE_AV_HW_SPK_LRCK},
    {"spk_din", PHONE_AV_HW_SPK_DIN},
    {"spk_en", PHONE_AV_HW_SPK_EN},
    {"mic_sck", PHONE_AV_HW_MIC_SCK},
    {"mic_din", PHONE_AV_HW_MIC_DIN},
#if !PHONE_AV_HW_MIC_PDM
    {"mic_ws", PHONE_AV_HW_MIC_WS},
#endif
#if PHONE_AV_HW_DISP_TYPE != 0
    {"disp_mosi", PHONE_AV_HW_DISP_MOSI},
    {"disp_clk", PHONE_AV_HW_DISP_CLK},
    {"disp_cs", PHONE_AV_HW_DISP_CS},
    {"disp_dc", PHONE_AV_HW_DISP_DC},
    {"disp_rst", PHONE_AV_HW_DISP_RST},
    {"disp_bl", PHONE_AV_HW_DISP_BL},
#endif
#if PHONE_AV_TANK_ENABLED
    {"tank_pwma", PHONE_AV_TANK_PWMA},
    {"tank_ain1", PHONE_AV_TANK_AIN1},
    {"tank_ain2", PHONE_AV_TANK_AIN2},
    {"tank_pwmb", PHONE_AV_TANK_PWMB},
    {"tank_bin1", PHONE_AV_TANK_BIN1},
    {"tank_bin2", PHONE_AV_TANK_BIN2},
    {"tank_stby", PHONE_AV_TANK_STBY},
#endif
    {"i2c_sda", PHONE_AV_I2C_SDA},
    {"i2c_scl", PHONE_AV_I2C_SCL},
    {"battery_adc", PHONE_AV_BATTERY_ADC},
};

constexpr std::size_t kPinCount = sizeof(kPins) / sizeof(kPins[0]);

constexpr bool UniquePins() {
    for (std::size_t i = 0; i < kPinCount; ++i) {
        if (kPins[i].pin < 0) {
            continue;
        }
        for (std::size_t j = i + 1; j < kPinCount; ++j) {
            if (kPins[j].pin < 0) {
                continue;
            }
            if (kPins[i].pin == kPins[j].pin) {
                return false;
            }
        }
    }
    return true;
}

constexpr bool IsReservedPin(int pin) {
    return pin == 0 || pin == 3 || pin == 19 || pin == 20 ||
           (pin >= 26 && pin <= 37) || pin == 43 || pin == 44 ||
           pin == 45 || pin == 46 || pin == 48;
}

constexpr bool AllPinsSafe() {
    for (std::size_t i = 0; i < kPinCount; ++i) {
        if (kPins[i].pin >= 0 && IsReservedPin(kPins[i].pin)) {
            return false;
        }
    }
    return true;
}

static_assert(UniquePins(),
              "phone-av config.h: duplicate GPIO - two peripherals share a pin");
static_assert(AllPinsSafe(),
              "phone-av config.h: a peripheral uses a reserved flash/PSRAM/USB/UART/"
              "strapping/LED GPIO");

}  // namespace phone_av_pins

#endif  // _PHONE_AV_PIN_MAP_CHECK_H_
