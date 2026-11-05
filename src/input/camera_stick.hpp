#pragma once

#include <stdint.h>

enum CameraStickMode {
    CAMERA_TOUCH_STICK = 0,
    CAMERA_TOUCH_SWIPE = 1,   
    CAMERA_TOUCH_STICK_GYRO = 2,
    CAMERA_TOUCH_SWIPE_GYRO = 3,  
    CAMERA_MODE_COUNT
};

#define CAMERA_SENS_MIN 1
#define CAMERA_SENS_MAX 10
#define CAMERA_SENS_DEFAULT 5

static inline bool camera_mode_is_swipe(int mode) {
    return mode == CAMERA_TOUCH_SWIPE || mode == CAMERA_TOUCH_SWIPE_GYRO;
}

static inline bool camera_mode_has_gyro(int mode) {
    return mode == CAMERA_TOUCH_STICK_GYRO || mode == CAMERA_TOUCH_SWIPE_GYRO;
}

const char *camera_mode_name(int mode);

// Games apply their own dead zone
#define CAMERA_ANTI_DEADZONE 0.22f

class TouchCameraStick {
  public:
    void configure(bool swipe, int sensitivity);
    void begin(int x, int y, uint64_t time_us);
    void move(int x, int y, uint64_t time_us);
    void end();

    bool active() const { return touching; }
    float x() const { return out_x; }
    float y() const { return out_y; }

  private:
    void _stick_move(int x, int y);
    void _swipe_move(int x, int y, uint64_t time_us);

    bool swipe = false;
    bool touching = false;
    float radius = 40.0f;     // stick: finger travel for full tilt (px)
    float full_speed = 600.f; // swipe: finger speed for full tilt (px/s)
    float anchor_x = 0, anchor_y = 0;
    int last_x = 0, last_y = 0;
    uint64_t last_us = 0;
    float vel_x = 0, vel_y = 0;
    float out_x = 0, out_y = 0;
};

class GyroCameraStick {
  public:
    void configure(int sensitivity);
    void reset();

    void update(float yaw_dps, float pitch_dps, float dt_s);

    float x() const { return out_x; }
    float y() const { return out_y; }

  private:
    float full_rate = 72.0f; // dps for full tilt
    float bias_yaw = 0, bias_pitch = 0;
    float calib_time = 0;
    float out_x = 0, out_y = 0;
};


class DoubleTapDetector {
  public:
    bool down(int x, int y, uint64_t time_us);
    void move(int x, int y);
    void up(uint64_t time_us);

  private:
    bool has_tap = false;
    int start_x = 0, start_y = 0;
    int max_travel = 0;
    uint64_t start_us = 0;
    uint64_t tap_end_us = 0;
    int tap_x = 0, tap_y = 0;
};
