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

#include "N3dsTouchscreenInput.hpp"
#include "../../system/dispatcher.hpp"

#define BACKLIGHT_IDLE_SECONDS 10

static volatile bool g_backlight_resync = false;
static aptHookCookie g_backlight_hook;

static void backlight_apt_hook(APT_HookType hook, void *param) {
    (void)param;
    switch (hook) {
    case APTHOOK_ONSUSPEND:
    case APTHOOK_ONSLEEP:
    case APTHOOK_ONEXIT:
        GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_BOTTOM);
        g_backlight_resync = true;
        break;
    case APTHOOK_ONRESTORE:
    case APTHOOK_ONWAKEUP:
        g_backlight_resync = true;
        break;
    default:
        break;
    }
}

N3dsTouchscreenInput::N3dsTouchscreenInput(GAMEPAD_STATE *gamepad_in,
                                           int image_width_in,
                                           int image_height_in,
                                           bool view_only_in,
                                           int camera_mode_in,
                                           int camera_sens_in,
                                           bool bottom_auto_off_in)
    : gamepad_state(gamepad_in), image_width(image_width_in),
      image_height(image_height_in), view_only(view_only_in),
      camera_mode(camera_mode_in), camera_sens(camera_sens_in) {
    if (bottom_auto_off_in && R_SUCCEEDED(gspLcdInit())) {
        bottom_auto_off = true;
        g_backlight_resync = false;
        aptHook(&g_backlight_hook, backlight_apt_hook, nullptr);
    }
    MessageDispatcher::get_instance()->subscribe(
        MessageType::TOUCH_STATE_CHANGED, this);
};

N3dsTouchscreenInput::~N3dsTouchscreenInput() {
    MessageDispatcher::get_instance()->unsubscribe(
        MessageType::TOUCH_STATE_CHANGED, this);
    if (bottom_auto_off) {
        aptUnhook(&g_backlight_hook);
        GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_BOTTOM);
        gspLcdExit();
    }
}

void N3dsTouchscreenInput::_update_backlight(bool touching) {
    if (!bottom_auto_off) {
        return;
    }
    const u64 now = svcGetSystemTick();
    if (g_backlight_resync) {
        g_backlight_resync = false;
        backlight_on = false; 
        last_activity_tick = now;
    }

    const bool may_dim = touch_type == N3dsTouchType::GAMEPAD ||
                         touch_type == N3dsTouchType::MOUSEPAD;
    if (!may_dim || touching || last_activity_tick == 0) {
        last_activity_tick = now;
    }
    const bool want_on =
        now - last_activity_tick < (u64)BACKLIGHT_IDLE_SECONDS * SYSCLOCK_ARM11;
    if (want_on != backlight_on) {
        if (want_on) {
            GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_BOTTOM);
        } else {
            GSPLCD_PowerOffBacklight(GSPLCD_SCREEN_BOTTOM);
        }
        backlight_on = want_on;
    }
}

void N3dsTouchscreenInput::accept(IMessage *msg) {
    if (msg->getMessageType() != MessageType::TOUCH_STATE_CHANGED) {
        return;
    }

    auto touch_msg = static_cast<TouchStateChangedMsg *>(msg);
    next_touch_type.store(touch_msg->ttype);
}

void N3dsTouchscreenInput::_n3dsinput_set_touch(N3dsTouchType touch_type_in) {
    switch (touch_type_in) {
    case N3dsTouchType::GAMEPAD:
        handler = std::make_unique<GamepadTouchHandler>(gamepad_state, camera_mode,
                                                        camera_sens);
        break;
    case N3dsTouchType::MOUSEPAD:
        handler = std::make_unique<MouseTouchHandler>();
        break;
    case N3dsTouchType::KEYBOARD:
        handler = std::make_unique<KeyboardTouchHandler>();
        break;
    case N3dsTouchType::ABSOLUTE_TOUCH:
        handler = std::make_unique<MirrorTouchHandler>();
        break;
    case N3dsTouchType::DS_TOUCH:
        handler = std::make_unique<StretchTouchHandler>();
        break;
    case N3dsTouchType::MAGNIFY_TOUCH:
        handler = std::make_unique<MagnifyTouchHandler>(
            gamepad_state, image_width, image_height);
        break;
    case N3dsTouchType::MENU_TOUCH:
        handler = std::make_unique<MenuTouchHandler>();
        break;
    case N3dsTouchType::DEBUG_TOUCH:
        handler = std::make_unique<DebugTouchHandler>();
        break;
    default:
        handler = nullptr;
        break;
    }
    touch_type = touch_type_in;
}

void N3dsTouchscreenInput::n3dsinput_handle_touch(u32 kDown, u32 kUp) {
    if (next_touch_type.load() != touch_type) {
        _n3dsinput_set_touch(next_touch_type.load());
    }
    _update_backlight((hidKeysHeld() & KEY_TOUCH) != 0);

    if (handler == nullptr) {
        return;
    }

    if (view_only && touch_type != N3dsTouchType::MENU_TOUCH) {
        return;
    }

    touchPosition touch;
    hidTouchRead(&touch);
    if (kDown & KEY_TOUCH) {
        handler->handle_touch_down(touch);
    } else if (kUp & KEY_TOUCH) {
        handler->handle_touch_up(touch);
    } else {
        handler->handle_touch_hold(touch);
    }
}
