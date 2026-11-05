#include "camera_stick.hpp"

#include <math.h>
#include <stdlib.h>

#define STICK_DEADZONE_PX 2.5f
#define SWIPE_NOISE_PX_S 20.0f

#define SWIPE_SMOOTH_S 0.045f
#define STICK_CURVE 1.3f

#define GYRO_DEADZONE_DPS 1.5f
#define GYRO_STILL_DPS 2.5f
#define GYRO_CALIB_S 0.6f
#define GYRO_BIAS_TRACK_S 2.0f

#define TAP_MAX_US 200000
#define TAP_MAX_TRAVEL_PX 10
#define DOUBLE_TAP_GAP_US 300000
#define DOUBLE_TAP_DIST_PX 40

const char *camera_mode_name(int mode) {
    switch (mode) {
    case CAMERA_TOUCH_SWIPE:
        return "Touch swipe";
    case CAMERA_TOUCH_STICK_GYRO:
        return "Touch stick + gyro";
    case CAMERA_TOUCH_SWIPE_GYRO:
        return "Touch swipe + gyro";
    default:
        return "Touch stick";
    }
}

static int clamp_sens(int s) {
    if (s < CAMERA_SENS_MIN)
        return CAMERA_SENS_MIN;
    if (s > CAMERA_SENS_MAX)
        return CAMERA_SENS_MAX;
    return s;
}

static void radial_output(float dx, float dy, float len, float m, float *ox,
                          float *oy) {
    if (m <= 0.0f || len <= 0.0f) {
        *ox = 0.0f;
        *oy = 0.0f;
        return;
    }
    if (m > 1.0f)
        m = 1.0f;
    const float out = CAMERA_ANTI_DEADZONE + (1.0f - CAMERA_ANTI_DEADZONE) * m;
    *ox = dx / len * out;
    *oy = dy / len * out;
}

void TouchCameraStick::configure(bool swipe_in, int sensitivity) {
    const int s = clamp_sens(sensitivity);
    swipe = swipe_in;
    radius = 60.0f - 4.0f * s;      // 56 px, 20 px, 40 px by default
    full_speed = 3000.0f / s;       // 3000 px/s, 300 px/s, 600 by default
}

void TouchCameraStick::begin(int x, int y, uint64_t time_us) {
    touching = true;
    anchor_x = (float)x;
    anchor_y = (float)y;
    last_x = x;
    last_y = y;
    last_us = time_us;
    vel_x = vel_y = 0.0f;
    out_x = out_y = 0.0f;
}

void TouchCameraStick::move(int x, int y, uint64_t time_us) {
    if (!touching) {
        return;
    }
    if (swipe) {
        _swipe_move(x, y, time_us);
    } else {
        _stick_move(x, y);
    }
}

void TouchCameraStick::end() {
    touching = false;
    vel_x = vel_y = 0.0f;
    out_x = out_y = 0.0f;
}

void TouchCameraStick::_stick_move(int x, int y) {
    float dx = (float)x - anchor_x;
    float dy = (float)y - anchor_y;
    float len = sqrtf(dx * dx + dy * dy);

    if (len > radius) {
        const float pull = (len - radius) / len;
        anchor_x += dx * pull;
        anchor_y += dy * pull;
        dx -= dx * pull;
        dy -= dy * pull;
        len = radius;
    }

    float m = 0.0f;
    if (len > STICK_DEADZONE_PX) {
        m = (len - STICK_DEADZONE_PX) / (radius - STICK_DEADZONE_PX);
        m = powf(m, STICK_CURVE);
    }

    // screen Y goes down, stick Y goes up
    radial_output(dx, -dy, len, m, &out_x, &out_y);
}

void TouchCameraStick::_swipe_move(int x, int y, uint64_t time_us) {
    if (time_us <= last_us) {
        return;
    }
    float dt = (float)(time_us - last_us) * 1e-6f;
    if (dt > 0.1f) {
        dt = 0.1f;
    }
    const float vx = (float)(x - last_x) / dt;
    const float vy = (float)(y - last_y) / dt;
    last_x = x;
    last_y = y;
    last_us = time_us;

    const float a = dt / (SWIPE_SMOOTH_S + dt);
    vel_x += (vx - vel_x) * a;
    vel_y += (vy - vel_y) * a;

    const float speed = sqrtf(vel_x * vel_x + vel_y * vel_y);
    float m = 0.0f;
    if (speed > SWIPE_NOISE_PX_S) {
        m = (speed - SWIPE_NOISE_PX_S) / (full_speed - SWIPE_NOISE_PX_S);
    }
    radial_output(vel_x, -vel_y, speed, m, &out_x, &out_y);
}

void GyroCameraStick::configure(int sensitivity) {
    full_rate = 360.0f / clamp_sens(sensitivity); // 72 dps by default
}

void GyroCameraStick::reset() {
    bias_yaw = bias_pitch = 0.0f;
    calib_time = 0.0f;
    out_x = out_y = 0.0f;
}

void GyroCameraStick::update(float yaw_dps, float pitch_dps, float dt_s) {
    if (dt_s <= 0.0f) {
        return;
    }

    if (dt_s > 0.1f) {
        dt_s = 0.1f;
    }

    if (calib_time < GYRO_CALIB_S) {
        const float w = dt_s / (calib_time + dt_s);
        bias_yaw += (yaw_dps - bias_yaw) * w;
        bias_pitch += (pitch_dps - bias_pitch) * w;
        calib_time += dt_s;
        out_x = out_y = 0.0f;
        return;
    }

    float yaw = yaw_dps - bias_yaw;
    float pitch = pitch_dps - bias_pitch;

    if (fabsf(yaw) < GYRO_STILL_DPS && fabsf(pitch) < GYRO_STILL_DPS) {
        const float a = dt_s / GYRO_BIAS_TRACK_S;
        bias_yaw += yaw * a;
        bias_pitch += pitch * a;
    }

    const float rate = sqrtf(yaw * yaw + pitch * pitch);
    float m = 0.0f;

    if (rate > GYRO_DEADZONE_DPS) {
        m = (rate - GYRO_DEADZONE_DPS) / (full_rate - GYRO_DEADZONE_DPS);
    }

    radial_output(-yaw, pitch, rate, m, &out_x, &out_y);
}

bool DoubleTapDetector::down(int x, int y, uint64_t time_us) {
    const bool second = has_tap && time_us - tap_end_us < DOUBLE_TAP_GAP_US && abs(x - tap_x) < DOUBLE_TAP_DIST_PX && abs(y - tap_y) < DOUBLE_TAP_DIST_PX;
    has_tap = false;
    start_x = x;
    start_y = y;
    max_travel = second ? TAP_MAX_TRAVEL_PX + 1 : 0; // a 2nd tap never chains
    start_us = time_us;
    return second;
}

void DoubleTapDetector::move(int x, int y) {
    const int travel = abs(x - start_x) + abs(y - start_y);

    if (travel > max_travel) {
        max_travel = travel;
    }
}

void DoubleTapDetector::up(uint64_t time_us) {
    has_tap = time_us - start_us < TAP_MAX_US && max_travel <= TAP_MAX_TRAVEL_PX;

    if (has_tap) {
        tap_end_us = time_us;
        tap_x = start_x;
        tap_y = start_y;
    }
}
