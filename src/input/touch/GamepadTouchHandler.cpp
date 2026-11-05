/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015-2017 Iwan Timmer
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */

#include "TouchHandler.hpp"
#include <Limelight.h>
#include <cstring>

#define BUTTON_ROW_HEIGHT 56
#define BUTTON_WIDTH 64

static uint64_t now_us() {
    return (uint64_t)(svcGetSystemTick() / (SYSCLOCK_ARM11 / 1000000.0));
}

GamepadTouchHandler::GamepadTouchHandler(GAMEPAD_STATE *gamepad_in,
                                         int camera_mode, int camera_sens)
    : gamepad_state(gamepad_in) {
    camera.configure(camera_mode_is_swipe(camera_mode), camera_sens);
    _publish();
}

GamepadTouchHandler::~GamepadTouchHandler() {
    held_key = 0;
    held_button = 0;
    r3_hold = false;
    camera.end();
    _publish();
}

void GamepadTouchHandler::_publish() {
    gamepad_state->touch_keys = held_key;
    gamepad_state->touch_buttons = held_button | (r3_hold ? RS_CLK_FLAG : 0);
    gamepad_state->touch_stick = camera.active() && !r3_hold;
    gamepad_state->touch_rx = gamepad_state->touch_stick ? camera.x() : 0.0f;
    gamepad_state->touch_ry = gamepad_state->touch_stick ? camera.y() : 0.0f;
}

void GamepadTouchHandler::_handle_touch_down(touchPosition touch) {
    held_key = 0;
    held_button = 0;
    on_camera = false;
    r3_hold = false;

    if (touch.py < BUTTON_ROW_HEIGHT) {
        switch (touch.px / BUTTON_WIDTH) {
        case 0:
            held_key = KEY_ZL;
            break;
        case 1:
            held_button = LS_CLK_FLAG;
            break;
        case 2:
            held_button = SPECIAL_FLAG;
            break;
        case 3:
            held_button = RS_CLK_FLAG;
            break;
        default:
            held_key = KEY_ZR;
            break;
        }
    } else {
        const uint64_t t = now_us();
        on_camera = true;
        r3_hold = double_tap.down(touch.px, touch.py, t);
        camera.begin(touch.px, touch.py, t);
    }
    _publish();
}

void GamepadTouchHandler::_handle_touch_up(touchPosition touch) {
    if (on_camera) {
        double_tap.up(now_us());
    }
    held_key = 0;
    held_button = 0;
    on_camera = false;
    r3_hold = false;
    camera.end();
    _publish();
}

void GamepadTouchHandler::_handle_touch_hold(touchPosition touch) {
    if (!on_camera) {
        return;
    }
    double_tap.move(touch.px, touch.py);
    camera.move(touch.px, touch.py, now_us());
    _publish();
}
