#ifndef _PHONE_AV_TANK_CONTROLLER_H_
#define _PHONE_AV_TANK_CONTROLLER_H_

#include "config.h"

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_timer.h>

#include <cstdint>
#include <mutex>
#include <string>

/* TB6612FNG dual H-bridge: left track on PWMA/AIN1/AIN2, right track on
 * PWMB/BIN1/BIN2. STBY is driven high when the driver is enabled. */
class TankController {
public:
    TankController();
    ~TankController();

    bool enabled() const { return enabled_; }

    /* direction: forward / backward / left / right / stop
     * left/right are pivot turns (one track forward, the other backward).
     * duration_ms is clamped to 100..10000 and stops the motors automatically. */
    bool Move(const std::string& direction, int speed, int duration_ms);
    void Stop();
    std::string StatusJson();

private:
    void SetLeft(bool forward, int speed);
    void SetRight(bool forward, int speed);
    void SetPwm(ledc_channel_t channel, int speed);
    void StopLocked();
    static void TimerCallback(void* arg);

    bool enabled_ = false;
    std::mutex mutex_;
    esp_timer_handle_t stop_timer_ = nullptr;
    std::string direction_ = "stop";
    int speed_ = 0;
};

#endif  // _PHONE_AV_TANK_CONTROLLER_H_
