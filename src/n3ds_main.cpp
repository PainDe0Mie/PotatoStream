/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015-2019 Iwan Timmer
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

#include "audio/audio.h"
#include "config.hpp"
#include "input/n3ds_input.hpp"
#include "menu_ui.hpp"
#include "potato/potato_profile.h"
#include "system/dispatcher.hpp"
#include "system/host_book.hpp"
#include "system/host_discovery.hpp"
#include "system/n3ds_connection.hpp"
#include "system/pair_record.hpp"
#include "system/update_check.hpp"
#include "video/video.hpp"

#include <3ds.h>
#include <Limelight.h>
#include <curl/curl.h>

#include <client.h>
#include <discover.h>
#include <http.h>

#include <arpa/inet.h>
#include <atomic>
#include <errno.h>
#include <exception>
#include <malloc.h>
#include <netdb.h>
#include <netinet/in.h>
#include <openssl/rand.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define SOC_ALIGN 0x1000
#define SOC_BUFFERSIZE 0x100000

struct NumericSetting {
    const char *title;
    int min_value;
    int max_value;
    int step;
    int fast_step;
};

static const NumericSetting SETTING_WIDTH = {"Stream width", 160, 1920, 8, 80};
static const NumericSetting SETTING_HEIGHT = {"Stream height", 120, 1200, 8, 60};
static const NumericSetting SETTING_FPS = {"Stream FPS", 10, 120, 1, 10};
static const NumericSetting SETTING_BITRATE = {"Bitrate (kbps)", 100, 50000, 100,
                                               1000};
static const NumericSetting SETTING_PACKET = {"Packet size", 256, 2048, 4, 128};
static const NumericSetting SETTING_PORT = {"Host port", 1, 65535, 1, 100};

static u32 *SOC_buffer = NULL;
static bool g_ac_initialized = false;
static bool g_gfx_initialized = false;
static bool g_apt_initialized = false;
static bool g_soc_initialized = false;
static bool g_ndmu_initialized = false;
static bool g_ndmu_exclusive = false;
static bool g_ndmu_locked = false;
static bool g_curl_initialized = false;

static int init_server(CONFIGURATION *config, SERVER_DATA *server);
static std::string build_profile_status(PCONFIGURATION config = nullptr);
static void wait_for_all_buttons_release();
static bool prompt_for_boolean(std::string prompt, bool default_val);

struct AsyncTask {
    int (*fn)(void *);
    void *context;
    std::atomic<bool> done = false;
    int result = -1;
};

struct InitServerTaskContext {
    CONFIGURATION *config;
    SERVER_DATA *server;
};

struct AppListTaskContext {
    PSERVER_DATA server;
    PAPP_LIST list = NULL;
};

struct PairTaskContext {
    PSERVER_DATA server;
    char *pin;
};

struct ServerTaskContext {
    PSERVER_DATA server;
};

struct StartAppTaskContext {
    PSERVER_DATA server;
    PSTREAM_CONFIGURATION stream;
    int appId;
    bool sops;
    bool localaudio;
    int gamepad_mask;
};

struct HostScanTaskContext {
    unsigned short port;
    HostScanResult *result;
};

static void async_task_entry(void *arg) {
    AsyncTask *task = static_cast<AsyncTask *>(arg);
    task->result = task->fn(task->context);
    task->done.store(true);
}

static int init_server_task(void *ctx) {
    auto *task = static_cast<InitServerTaskContext *>(ctx);
    return init_server(task->config, task->server);
}

static int app_list_task(void *ctx) {
    auto *task = static_cast<AppListTaskContext *>(ctx);
    return gs_applist(task->server, &task->list);
}

static int pair_task(void *ctx) {
    auto *task = static_cast<PairTaskContext *>(ctx);
    return gs_pair(task->server, task->pin);
}

static int unpair_task(void *ctx) {
    auto *task = static_cast<ServerTaskContext *>(ctx);
    return gs_unpair(task->server);
}

static int quit_app_task(void *ctx) {
    auto *task = static_cast<ServerTaskContext *>(ctx);
    return gs_quit_app(task->server);
}

static int start_app_task(void *ctx) {
    auto *task = static_cast<StartAppTaskContext *>(ctx);
    return gs_start_app(task->server, task->stream, task->appId, task->sops,
                        task->localaudio, task->gamepad_mask);
}

static int update_check_task(void *context) {
    *static_cast<UpdateCheckResult *>(context) = update_check_run();
    return 0;
}

static volatile bool g_background_cancel = false;

static int host_scan_task(void *ctx) {
    auto *task = static_cast<HostScanTaskContext *>(ctx);
    *task->result = host_discovery_scan(task->port, &g_background_cancel);
    return task->result->failed ? -1 : 0;
}

struct BackgroundJob {
    AsyncTask task;
    Thread worker = nullptr;
    bool running = false;
    void (*on_done)(void) = nullptr;
};

static BackgroundJob g_background;
static UpdateCheckResult g_update_result;
static HostScanResult g_scan_result;
static HostScanTaskContext g_scan_context;

static bool background_idle() { return !g_background.running; }

static void background_start(int (*fn)(void *), void *context,
                             void (*on_done)(void)) {
    if (g_background.running) {
        return;
    }

    g_background.task.fn = fn;
    g_background.task.context = context;
    g_background.task.done.store(false);
    g_background.task.result = -1;
    g_background.on_done = on_done;
    g_background_cancel = false;

    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    g_background.worker = threadCreate(async_task_entry, &g_background.task,
                                       0x20000, priority, -1, false);
    if (g_background.worker == nullptr) {
        g_background.task.fn(g_background.task.context);
        if (on_done != nullptr) {
            on_done();
        }
        return;
    }
    g_background.running = true;
}

static bool background_poll() {
    if (!g_background.running || !g_background.task.done.load()) {
        return false;
    }

    threadJoin(g_background.worker, ~0ULL);
    threadFree(g_background.worker);
    g_background.worker = nullptr;
    g_background.running = false;

    if (g_background.on_done != nullptr) {
        g_background.on_done();
    }
    return true;
}

static void background_wait() {
    if (!g_background.running) {
        return;
    }

    g_background_cancel = true;
    threadJoin(g_background.worker, ~0ULL);
    threadFree(g_background.worker);
    g_background.worker = nullptr;
    g_background.running = false;
    g_background.on_done = nullptr;
    g_background_cancel = false;
}

static void on_update_check_done() {
    if (g_update_result.failed || !g_update_result.update_available) {
        return;
    }
    menu_ui_show_toast("PotatoStream " + g_update_result.latest_tag +
                           " is out on GitHub",
                       5.0f);
}

static void on_host_scan_done() {
    if (g_scan_result.failed) {
        menu_ui_show_toast(g_scan_result.error.empty()
                               ? std::string("Network scan failed")
                               : g_scan_result.error,
                           6.0f);
        return;
    }

    for (const auto &found : g_scan_result.hosts) {
        host_book_add(found.address, found.port, found.address);
    }

    const size_t count = g_scan_result.hosts.size();
    if (count == 0) {
        std::string report = "No host on port " +
                             std::to_string((unsigned)g_scan_context.port) +
                             ": " + std::to_string(g_scan_result.probed) +
                             " IPs scanned, " +
                             std::to_string(g_scan_result.open_ports) + " open";
        if (g_scan_result.socket_errors > 0) {
            report += ", " + std::to_string(g_scan_result.socket_errors) +
                      " socket errors";
        }
        menu_ui_show_toast(report, 6.0f);
    } else if (count == 1) {
        menu_ui_show_toast("1 host found on the network", 3.0f);
    } else {
        menu_ui_show_toast(std::to_string(count) +
                               " hosts found on the network",
                           3.0f);
    }
}

static int run_loading_task(const std::string &title, const std::string &body,
                            const std::string &status,
                            const std::string &footer_hint, int (*fn)(void *),
                            void *context,
                            const std::string &pin = std::string()) {
    if (!menu_ui_is_active()) {
        return fn(context);
    }

    AsyncTask task = {
        .fn = fn,
        .context = context,
    };

    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    Thread worker =
        threadCreate(async_task_entry, &task, 0x20000, priority, -1, false);
    if (worker == nullptr) {
        return fn(context);
    }

    int frame = 0;
    while (aptMainLoop() && !task.done.load()) {
        if (pin.empty()) {
            menu_ui_draw_loading(title, body, status, footer_hint, frame);
        } else {
            menu_ui_draw_pairing(title, pin, body, status, footer_hint, frame);
        }
        gspWaitForVBlank();
        frame++;
    }

    threadJoin(worker, ~0ULL);
    threadFree(worker);
    return task.result;
}

static void draw_stream_wait_screen(PCONFIGURATION config,
                                    const std::string &headline,
                                    const std::string &detail) {
    consoleSelect(&DebugTouchHandler::topScreen);
    consoleClear();
    printf("STREAM POTATO\n\n");
    printf("%s\n\n", headline.c_str());
    printf("%s\n\n", detail.c_str());
    printf("Requested stream:\n%dx%d @ %d fps\n",
           config->stream.width, config->stream.height, config->stream.fps);

    consoleSelect(&DebugTouchHandler::bottomScreen);
    consoleClear();
    printf("Status\n\n");
    printf("%s\n\n", build_profile_status(config).c_str());
    if (g_potato.is_potato) {
        printf("If the PC desktop still looks tiny,\n");
        printf("the host is likely staying in 1080p.\n");
        printf("Use a game/app session or raise PC UI scaling.\n");
    }

    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
}

static std::string build_profile_status(PCONFIGURATION config) {
    char buffer[256];
    if (g_potato.is_potato) {
        bool better_screen =
            config != nullptr ? config->experimental_better_screen
                              : g_potato.experimental_better_screen;
        bool stable_stream =
            config != nullptr ? config->experimental_stable_stream
                              : g_potato.experimental_stable_stream;
        bool ultra_potato =
            config != nullptr ? config->experimental_ultra_potato
                              : g_potato.experimental_ultra_potato;
        const int width =
            config != nullptr ? config->stream.width : g_potato.width;
        const int height =
            config != nullptr ? config->stream.height : g_potato.height;
        const int fps = config != nullptr ? config->stream.fps : g_potato.fps;
        const int bitrate =
            config != nullptr ? config->stream.bitrate : g_potato.bitrate_kbps;
        const int packet_size = config != nullptr ? config->stream.packetSize
                                                  : g_potato.packet_size;
        const bool host_audio =
            config != nullptr ? config->localaudio : g_potato.host_audio;
        char experimental[128];
        snprintf(experimental, sizeof(experimental), "%s%s%s%s%s",
                 ultra_potato ? "ultra potato auto" : "",
                 ultra_potato && (better_screen || stable_stream) ? " + " : "",
                 better_screen ? "better screen" : "",
                 (better_screen && stable_stream && !ultra_potato) ? " + " : "",
                 stable_stream ? "stable stream" : "");

        MenuStatus ui_status;
        ui_status.mode = "OLD 3DS";
        ui_status.alert = g_potato.dynamic_ultra_active;
        const int locked =
            config != nullptr
                ? (int)config->auto_width + (int)config->auto_height +
                      (int)config->auto_fps + (int)config->auto_bitrate +
                      (int)config->auto_packetsize
                : 5;
        ui_status.detail =
            locked == 5
                ? "Auto profile is available on this console."
                : "Auto profile is available. Some values are set manually.";
        ui_status.resolution =
            std::to_string(width) + "x" + std::to_string(height);
        ui_status.fps = std::to_string(fps);
        ui_status.bitrate = std::to_string(bitrate) + " kbps";
        ui_status.packet = std::to_string(packet_size);
        ui_status.audio = host_audio ? "Host" : "3DS";
        if (g_potato.dynamic_ultra_active) {
            ui_status.flags.push_back("emergency");
        }
        if (ultra_potato) {
            ui_status.flags.push_back("ultra potato auto");
        }
        if (better_screen) {
            ui_status.flags.push_back("better screen");
        }
        if (stable_stream) {
            ui_status.flags.push_back("stable stream");
        }
        menu_ui_set_status(ui_status);
        snprintf(buffer, sizeof(buffer), "%s: %dx%d | %dfps | %dkbps | packet %d | audio %s%s%s",
                 g_potato.dynamic_ultra_active ? "OLD 3DS (emergency)" : "OLD 3DS",
                 width, height, fps, bitrate, packet_size,
                 host_audio ? "host" : "3DS",
                 experimental[0] != '\0' ? " | " : "",
                 experimental);
        return buffer;
    }

    // determine the exact New 3DS model name
    const char *n3ds_label = "NEW 3DS";
    switch (g_potato.model) {
    case CFG_MODEL_N3DS:   n3ds_label = "NEW 3DS";     break;
    case CFG_MODEL_N3DSXL: n3ds_label = "NEW 3DS XL";  break;
    case CFG_MODEL_N2DSXL: n3ds_label = "NEW 2DS XL";  break;
    default:               n3ds_label = "NEW 3DS";     break;
    }
    MenuStatus ui_status;
    ui_status.mode = n3ds_label;

    if (config == nullptr) {
        ui_status.detail = "Hardware H264 decode & 2 thread FFmpeg.";
        menu_ui_set_status(ui_status);
        return std::string(n3ds_label) + ": hardware decode available.";
    }

    ui_status.resolution = std::to_string(config->stream.width) + "x" +
                           std::to_string(config->stream.height);
    ui_status.fps = std::to_string(config->stream.fps);
    ui_status.bitrate = std::to_string(config->stream.bitrate) + " kbps";
    ui_status.packet = std::to_string(config->stream.packetSize);
    ui_status.audio = config->localaudio ? "Host" : "3DS";
    const char *dec_label =
        (config->video_decoder == VIDEO_DECODER_TYPE::HARDWARE_VIDEO_DECODER)
            ? "MVD"
            : "SW";
    ui_status.flags.push_back(std::string("decoder:") + dec_label);
    menu_ui_set_status(ui_status);

    snprintf(buffer, sizeof(buffer),
             "%s: %dx%d | %dfps | %dkbps | %s decoder",
             n3ds_label, config->stream.width, config->stream.height,
             config->stream.fps, config->stream.bitrate, dec_label);
    return buffer;
}

static const char *stage_name(int stage) {
    switch (stage) {
    case STAGE_PLATFORM_INIT:
        return "Platform init";
    case STAGE_NAME_RESOLUTION:
        return "Resolution negotiation";
    case STAGE_AUDIO_STREAM_INIT:
        return "Audio stream init";
    case STAGE_RTSP_HANDSHAKE:
        return "RTSP handshake";
    case STAGE_CONTROL_STREAM_INIT:
        return "Control stream init";
    case STAGE_VIDEO_STREAM_INIT:
        return "Video stream init";
    case STAGE_INPUT_STREAM_INIT:
        return "Input stream init";
    case STAGE_CONTROL_STREAM_START:
        return "Control stream start";
    case STAGE_VIDEO_STREAM_START:
        return "Video stream start";
    case STAGE_AUDIO_STREAM_START:
        return "Audio stream start";
    case STAGE_INPUT_STREAM_START:
        return "Input stream start";
    default:
        return "Unknown stage";
    }
}

static std::string build_stream_end_message(N3dsConnectionListener *listener) {
    if (listener == nullptr) {
        return "The stream ended, but no diagnostic details were available.";
    }

    const int error_code = listener->get_last_error_code();
    const int stage = listener->get_last_stage();
    const int stage_error = listener->get_last_stage_error();
    const int connection_status = listener->get_last_connection_status();

    if (stage_error != 0 && !listener->has_connection_started()) {
        char buffer[256];
        snprintf(buffer, sizeof(buffer),
                 "Startup failed during %s. Error code: %d.",
                 stage_name(stage), stage_error);
        return buffer;
    }

    switch (error_code) {
    case ML_ERROR_GRACEFUL_TERMINATION:
        return "The host closed the stream cleanly.";
    case ML_ERROR_NO_VIDEO_TRAFFIC:
        return "No video packets reached the 3DS. Check firewall rules, Sunshine host availability, and local network routing.";
    case ML_ERROR_NO_VIDEO_FRAME:
        return "The 3DS received data but could not build frames fast enough. StreamPotato already forced safe mode, so the next suspects are Wi-Fi stability, Sunshine encoder load, or overlays/DRM on the host.";
    case ML_ERROR_UNEXPECTED_EARLY_TERMINATION:
        return "The host terminated the stream immediately. This usually points to capture failure, DRM content, or Sunshine/GameStream refusing the current desktop/app.";
    case ML_ERROR_PROTECTED_CONTENT:
        return "The host reported DRM-protected content. Close video players, streaming apps, or protected overlays on the PC.";
    case ML_ERROR_FRAME_CONVERSION:
        return "The host failed a frame conversion step. Disable HDR on the PC and try a simple desktop resolution before reconnecting.";
    default:
        if (error_code != 0) {
            char buffer[256];
            snprintf(buffer, sizeof(buffer),
                     "The stream ended with error %d after %s.",
                     error_code, stage_name(stage));
            return buffer;
        }
        if (connection_status == CONN_STATUS_POOR) {
            return "The stream stopped after a poor connection state. Stay close to Wi-Fi, prefer 5 GHz, and keep the PC on Ethernet if possible.";
        }
        return "The stream stopped.";
    }
}

static void wait_for_all_buttons_release() {
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysHeld() == 0) {
            return;
        }

        if (menu_ui_is_active()) {
            menu_ui_draw_message("StreamPotato", "Release buttons to continue.",
                                 build_profile_status(), "Release all inputs");
        } else {
            gfxSwapBuffers();
            gfxFlushBuffers();
        }
        gspWaitForVBlank();
    }
}

static inline void wait_for_button(std::string prompt = "") {
    wait_for_all_buttons_release();

    const std::string message =
        prompt.empty() ? "Press any button to continue." : prompt;
    if (prompt.empty()) {
        printf("\nPress any button to continue\n");
    } else {
        printf("\n%s\n", prompt.c_str());
    }
    while (aptMainLoop()) {
        if (menu_ui_is_active()) {
            menu_ui_draw_message("StreamPotato", message, build_profile_status(),
                                 "Press any button");
        } else {
            gfxSwapBuffers();
            gfxFlushBuffers();
        }
        gspWaitForVBlank();

        hidScanInput();
        u32 kDown = hidKeysDown();

        if (kDown)
            break;
    }
}

static void n3ds_exit_handler(void) {
    if (menu_ui_is_active()) {
        menu_ui_shutdown();
    }
    if (g_ndmu_initialized) {
        if (g_ndmu_locked) {
            NDMU_UnlockState();
            g_ndmu_locked = false;
        }
        if (g_ndmu_exclusive) {
            NDMU_LeaveExclusiveState();
            g_ndmu_exclusive = false;
        }
        ndmuExit();
        g_ndmu_initialized = false;
    }
    if (g_soc_initialized) {
        SOCU_ShutdownSockets();
        SOCU_CloseSockets();
        socExit();
        g_soc_initialized = false;
    }
    if (g_curl_initialized) {
        curl_global_cleanup();
        g_curl_initialized = false;
    }
    if (SOC_buffer != NULL) {
        free(SOC_buffer);
        SOC_buffer = NULL;
    }
    if (g_apt_initialized) {
        aptExit();
        g_apt_initialized = false;
    }
    if (g_gfx_initialized) {
        gfxExit();
        g_gfx_initialized = false;
    }
    if (g_ac_initialized) {
        acExit();
        g_ac_initialized = false;
    }
}

static int console_selection_prompt(std::string prompt,
                                    std::vector<std::string> options,
                                    int default_idx,
                                    std::string subtitle = "",
                                    std::string status = "",
                                    std::string footer_hint = "",
                                    const std::vector<MenuRowAction> *row_actions = nullptr,
                                    bool *action_chosen = nullptr,
                                    bool *refreshed = nullptr) {
    if (action_chosen != nullptr) {
        *action_chosen = false;
    }
    if (refreshed != nullptr) {
        *refreshed = false;
    }
    if (status.empty()) {
        status = build_profile_status();
    }
    if (footer_hint.empty()) {
        footer_hint = "D-Pad: move   A: confirm   B: back";
    }

    if (options.empty()) {
        return -1;
    }

    int option_idx = default_idx;
    if (option_idx < 0) {
        option_idx = 0;
    } else if ((size_t)option_idx >= options.size()) {
        option_idx = (int)options.size() - 1;
    }
    wait_for_all_buttons_release();

    auto row_has_action = [&](int idx) {
        return row_actions != nullptr && idx >= 0 &&
               idx < (int)row_actions->size() &&
               !(*row_actions)[idx].label.empty();
    };
    bool on_action = false;

    while (aptMainLoop()) {
        if (menu_ui_is_active()) {
            menu_ui_draw_menu(prompt, subtitle, options, option_idx, status,
                              footer_hint, row_actions, on_action);
        } else {
            consoleClear();
            if (!prompt.empty()) {
                printf("%s\n", prompt.c_str());
            }
            if (!subtitle.empty()) {
                printf("%s\n\n", subtitle.c_str());
            }
            printf("Press up/down to select\n");
            printf("Press A to confirm\n");
            printf("Press B to go back\n\n");

            for (size_t i = 0; i < options.size(); i++) {
                std::string suffix;
                if (row_has_action((int)i)) {
                    suffix = "  [" + (*row_actions)[i].label + "]";
                    if ((*row_actions)[i].active) {
                        suffix += "*";
                    }
                }
                if ((int)i == option_idx) {
                    printf(">%s%s%s\n", options[i].c_str(), suffix.c_str(),
                           on_action ? " <" : "");
                } else {
                    printf("%s%s\n", options[i].c_str(), suffix.c_str());
                }
            }
        }

        if (!menu_ui_is_active()) {
            gfxSwapBuffers();
            gfxFlushBuffers();
        }
        gspWaitForVBlank();

        // callers that can rebuild their list let a finished job interrupt them
        if (refreshed != nullptr && background_poll()) {
            *refreshed = true;
            return option_idx;
        }

        hidScanInput();
        u32 kDown = hidKeysDown();

        if (kDown & KEY_A) {
            if (!menu_ui_is_active()) {
                consoleClear();
            }
            if (action_chosen != nullptr) {
                *action_chosen = on_action;
            }
            return option_idx;
        }
        if (kDown & KEY_B) {
            if (on_action) {
                on_action = false;
                continue;
            }
            if (!menu_ui_is_active()) {
                consoleClear();
            }
            return -1;
        }
        if (kDown & (KEY_DOWN | KEY_CPAD_DOWN)) {
            if ((size_t)(option_idx + 1) < options.size()) {
                option_idx++;
            }
        } else if (kDown & (KEY_UP | KEY_CPAD_UP)) {
            if (option_idx > 0) {
                option_idx--;
            }
        } else if (kDown & (KEY_RIGHT | KEY_CPAD_RIGHT)) {
            on_action = true;
        } else if (kDown & (KEY_LEFT | KEY_CPAD_LEFT)) {
            on_action = false;
        }
        on_action = on_action && row_has_action(option_idx);
    }

    return -1;
}

static std::string prompt_for_action(PSERVER_DATA server) {
    if (server->paired) {
        std::vector<std::string> actions = {
            "Start stream",
            "Stream settings",
            "Quit running app",
            "Unpair host",
        };
        char subtitle[192];
        snprintf(subtitle, sizeof(subtitle), "Host ready. GPU: %s",
                 server->gpuType != NULL ? server->gpuType : "unknown");
        int idx = console_selection_prompt("Select an action", actions, 0,
                                           subtitle, build_profile_status());
        if (idx < 0) {
            return "";
        }
        static const std::vector<std::string> action_keys = {
            "stream",
            "stream settings",
            "quit stream",
            "unpair",
        };
        return action_keys[idx];
    }
    std::vector<std::string> actions = {"Pair with host"};
    int idx = console_selection_prompt("Select an action", actions, 0,
                                       "This host is not paired yet.",
                                       build_profile_status());
    if (idx < 0) {
        return "";
    }
    return "pair";
}

static int prompt_for_number(const NumericSetting &setting, int value) {
    wait_for_all_buttons_release();

    if (value < setting.min_value) {
        value = setting.min_value;
    } else if (value > setting.max_value) {
        value = setting.max_value;
    }

    const int initial_value = value;
    int hold_counter = 0;
    int last_dir = 0;  // 0=none, 1=up, 2=down, 3=right, 4=left

    char range_hint[96];
    snprintf(range_hint, sizeof(range_hint), "Allowed range: %d - %d",
             setting.min_value, setting.max_value);
    char footer_hint[128];
    snprintf(footer_hint, sizeof(footer_hint),
             "Up/Down: +-%d   Left/Right: +-%d   A: confirm   B: cancel",
             setting.step, setting.fast_step);

    while (aptMainLoop()) {
        if (menu_ui_is_active()) {
            menu_ui_draw_number_editor(setting.title,
                                       "Hold a direction to change faster.",
                                       std::to_string(value), range_hint,
                                       build_profile_status(), footer_hint);
        } else {
            gfxSwapBuffers();
            gfxFlushBuffers();
        }
        gspWaitForVBlank();

        hidScanInput();
        u32 kDown = hidKeysDown();
        u32 kHeld = hidKeysHeld();

        if (kDown & KEY_A) {
            return value;
        }
        if (kDown & KEY_B) {
            return initial_value;
        }

        int cur_dir = 0;
        if (kHeld & KEY_UP) cur_dir = 1;
        else if (kHeld & KEY_DOWN) cur_dir = 2;
        else if (kHeld & KEY_RIGHT) cur_dir = 3;
        else if (kHeld & KEY_LEFT) cur_dir = 4;

        if (cur_dir == 0) {
            hold_counter = 0;
            last_dir = 0;
            continue;
        }

        if (cur_dir != last_dir) {
            hold_counter = 0;
            last_dir = cur_dir;
        } else {
            hold_counter++;
        }

        const bool trigger =
            (kDown & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)) ||
            (hold_counter > 20 && (hold_counter % 4 == 0));
        if (!trigger) {
            continue;
        }

        switch (cur_dir) {
        case 1: value += setting.step; break;
        case 2: value -= setting.step; break;
        case 3: value += setting.fast_step; break;
        case 4: value -= setting.fast_step; break;
        }

        if (value < setting.min_value) {
            value = setting.min_value;
        } else if (value > setting.max_value) {
            value = setting.max_value;
        }
    }

    return initial_value;
}

static std::string prompt_for_ip_address() {
    wait_for_all_buttons_release();

    int octets[4] = {192, 168, 1, 1};
    int selected = 0;
    int hold_counter = 0;
    int last_dir = 0;  // 0=none, 1=up, 2=down, 3=left, 4=right

    while (aptMainLoop()) {
        if (menu_ui_is_active()) {
            menu_ui_draw_ip_picker(
                "Enter host IP",
                "Up/Down: change value   Left/Right: switch octet",
                octets, selected,
                build_profile_status(),
                "D-Pad: move   A: confirm   B: back");
        } else {
            gfxSwapBuffers();
            gfxFlushBuffers();
        }
        gspWaitForVBlank();

        hidScanInput();
        u32 kDown = hidKeysDown();
        u32 kHeld = hidKeysHeld();

        if (kDown & KEY_A) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d.%d.%d.%d",
                     octets[0], octets[1], octets[2], octets[3]);
            return buf;
        }
        if (kDown & KEY_B) {
            return "";
        }

        int cur_dir = 0;
        if (kHeld & KEY_UP) cur_dir = 1;
        else if (kHeld & KEY_DOWN) cur_dir = 2;
        else if (kHeld & KEY_LEFT) cur_dir = 3;
        else if (kHeld & KEY_RIGHT) cur_dir = 4;

        if (cur_dir == 0) {
            hold_counter = 0;
            last_dir = 0;
        } else {
            if (cur_dir != last_dir) {
                hold_counter = 0;
                last_dir = cur_dir;
            } else {
                hold_counter++;
            }

            bool trigger = (kDown & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)) ||
                           (hold_counter > 20 && (hold_counter % 4 == 0));

            if (trigger) {
                switch (cur_dir) {
                case 1: octets[selected] = (octets[selected] + 1) % 256; break;
                case 2: octets[selected] = (octets[selected] + 255) % 256; break;
                case 3: selected = (selected + 3) % 4; break;
                case 4: selected = (selected + 1) % 4; break;
                }
            }
        }
    }
    return "";
}

static std::string prompt_for_text(const std::string &title,
                                   const std::string &initial) {
    char buffer[64];
    SwkbdState swkbd;
    swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, sizeof(buffer) - 1);
    swkbdSetHintText(&swkbd, title.c_str());
    swkbdSetInitialText(&swkbd, initial.c_str());
    swkbdSetValidation(&swkbd, SWKBD_NOTBLANK_NOTEMPTY, 0, 0);

    const SwkbdButton button = swkbdInputText(&swkbd, buffer, sizeof(buffer));
    wait_for_all_buttons_release();
    if (button != SWKBD_BUTTON_CONFIRM) {
        return "";
    }
    return std::string(buffer);
}

static void prompt_for_host_options(const HostRecord &host) {
    char subtitle[192];
    snprintf(subtitle, sizeof(subtitle), "%s:%u", host.address.c_str(),
             (unsigned)host.port);

    std::vector<std::string> options;
    options.push_back("Rename");
    options.push_back("Remove");

    int idx = 0;
    while (aptMainLoop()) {
        idx = console_selection_prompt(host.name, options, idx, subtitle,
                                       build_profile_status());
        if (idx < 0) {
            return;
        }

        if (idx == 0) {
            std::string name = prompt_for_text("Host name", host.name);
            if (!name.empty()) {
                host_book_set_name(host.address, host.port, name);
            }
            return;
        }

        char remove_subtitle[192];
        snprintf(remove_subtitle, sizeof(remove_subtitle),
                 "Removing %s also forgets its pairing.", host.name.c_str());
        std::vector<std::string> confirm = {
            "Keep this host",
            "Remove it",
        };
        if (console_selection_prompt("Remove host", confirm, 0, remove_subtitle,
                                     build_profile_status()) == 1) {
            host_book_remove(host.address, host.port);
            return;
        }
    }
}

static void prompt_for_app_settings(PCONFIGURATION config) {
    int idx = 0;
    while (aptMainLoop()) {
        std::vector<std::string> options = {
            "Host port\t" + std::to_string(config->port),
            std::string("Auto-detect hosts: ") +
                (config->autodiscover ? "enabled" : "disabled"),
        };
        idx = console_selection_prompt("Settings", options, idx,
                                       "Applies to every host in the list.",
                                       build_profile_status(config));
        if (idx < 0) {
            break;
        }

        if (idx == 0) {
            config->port =
                (unsigned short)prompt_for_number(SETTING_PORT, config->port);
        } else {
            config->autodiscover = prompt_for_boolean(
                "Scan the local network for Sunshine PCs when the host list "
                "opens",
                config->autodiscover);
        }
    }

    config_save((char *)STREAMPOTATO_CONFIG_PATH, config);
}

static std::string prompt_for_address(PCONFIGURATION config,
                                      bool *should_exit) {
    *should_exit = false;
    bool scan_pending = config->autodiscover;
    int selected = 0;
    std::string selected_label;

    while (aptMainLoop()) {
        if (scan_pending && background_idle()) {
            scan_pending = false;
            g_scan_context.port = config->port;
            g_scan_context.result = &g_scan_result;
            background_start(host_scan_task, &g_scan_context,
                             on_host_scan_done);
            menu_ui_show_toast("Scanning the network for hosts", 3.0f);
        }

        auto hosts = host_book_list();
        std::vector<std::string> options;
        std::vector<MenuRowAction> row_actions;
        MenuRowAction none;
        MenuRowAction more;
        more.label = "...";

        options.push_back("Add a host");
        row_actions.push_back(none);
        for (const auto &host : hosts) {
            const std::string port = std::to_string((unsigned)host.port);
            options.push_back(host.name + "\n" +
                              (host.name == host.address
                                   ? "port " + port
                                   : host.address + " : " + port));
            row_actions.push_back(more);
        }
        options.push_back("Settings");
        row_actions.push_back(none);
        options.push_back("Exit");
        row_actions.push_back(none);

        if (!selected_label.empty()) {
            for (size_t i = 0; i < options.size(); i++) {
                if (options[i] == selected_label) {
                    selected = (int)i;
                    break;
                }
            }
            selected_label.clear();
        }
        if (selected < 0 || selected >= (int)options.size()) {
            selected = 0;
        }

        bool open_options = false;
        bool refreshed = false;
        selected = console_selection_prompt(
            "Select a host", options, selected,
            hosts.empty()
                ? "No host yet. Pick Add a host to enter an IP."
                : "PotatoStream v1.3.0",
            build_profile_status(config),
            "D-Pad: move   A: connect   Right: options", &row_actions,
            &open_options, &refreshed);
        if (refreshed) {
            if (selected >= 0 && selected < (int)options.size()) {
                selected_label = options[selected];
            }
            continue;
        }
        if (selected < 0) {
            selected = 0;
            continue;
        }

        const int host_idx = selected - 1;
        const bool is_host = host_idx >= 0 && host_idx < (int)hosts.size();

        if (is_host) {
            if (open_options) {
                prompt_for_host_options(hosts[host_idx]);
                continue;
            }
            background_wait();
            return host_book_format_key(hosts[host_idx].address,
                                        hosts[host_idx].port);
        }

        if (selected == 0) {
            std::string address = prompt_for_ip_address();
            if (address.empty()) {
                continue;
            }
            host_book_add(address, config->port, address);
            background_wait();
            return host_book_format_key(address, config->port);
        }

        if (selected == (int)options.size() - 2) {
            prompt_for_app_settings(config);
            continue;
        }

        background_wait();
        *should_exit = true;
        return "";
    }

    background_wait();
    *should_exit = true;
    return "";
}

static VIDEO_DECODER_TYPE
prompt_for_video_decoder(VIDEO_DECODER_TYPE default_val) {
    std::vector<std::string> decoders = {
        "Hardware decoder (New 3DS only)",
        "Software decoder",
        "Disable video",
    };
    int idx = console_selection_prompt(
        "Select a video option", decoders, (int)default_val,
        "Old 3DS / 2DS should stay on software. StreamPotato safe mode enforces it automatically.",
        build_profile_status());
    if (idx < 0) {
        return default_val;
    }
    return (VIDEO_DECODER_TYPE)idx;
}

static bool prompt_for_boolean(std::string prompt, bool default_val) {
    std::vector<std::string> options = {
        "Enabled",
        "Disabled",
    };
    int idx = console_selection_prompt(prompt, options, default_val ? 0 : 1,
                                       "", build_profile_status());
    if (idx < 0) {
        idx = default_val ? 0 : 1;
    }
    return idx == 0;
}


static bool *auto_flag_for(PCONFIGURATION config, const std::string &key) {
    if (!g_potato.is_potato) {
        return nullptr;
    }
    if (key == "width") {
        return &config->auto_width;
    }
    if (key == "height") {
        return &config->auto_height;
    }
    if (key == "fps") {
        return &config->auto_fps;
    }
    if (key == "bitrate") {
        return &config->auto_bitrate;
    }
    if (key == "packetsize") {
        return &config->auto_packetsize;
    }
    return nullptr;
}

static int stream_value_for(PCONFIGURATION config, const std::string &key) {
    if (key == "width") {
        return config->stream.width;
    }
    if (key == "height") {
        return config->stream.height;
    }
    if (key == "fps") {
        return config->stream.fps;
    }
    if (key == "bitrate") {
        return config->stream.bitrate;
    }
    if (key == "packetsize") {
        return config->stream.packetSize;
    }
    return 0;
}

static void prompt_for_stream_settings(PCONFIGURATION config) {
    std::vector<std::string> setting_keys = {
        "width",         "height",        "fps",
        "bitrate",       "packetsize",    "sops",
        "localaudio",    "quitappafter",  "viewonly",
        "video_decoder", "better_screen", "stable_stream",
        "ultra_potato",  "motion_controls",
        "swapfacebuttons",               "swaptriggersandshoulders",
        "usetriggersformouse",
    };
    int idx = 0;
    while (1) {
        std::vector<std::string> setting_names = {
            // A tab splits the label from its value, drawn as a pill
            "Width\t" + std::to_string(config->stream.width),
            "Height\t" + std::to_string(config->stream.height),
            "FPS\t" + std::to_string(config->stream.fps),
            "Bitrate\t" + std::to_string(config->stream.bitrate) + " kbps",
            "Packet size\t" + std::to_string(config->stream.packetSize),
            std::string("Optimize game settings: ") +
                (config->sops ? "enabled" : "disabled"),
            std::string("Audio on host only: ") +
                (config->localaudio ? "enabled" : "disabled"),
            std::string("Quit host app after stream: ") +
                (config->quitappafter ? "enabled" : "disabled"),
            std::string("View only mode: ") +
                (config->viewonly ? "enabled" : "disabled"),
            std::string("Video decoder\t") +
                (config->video_decoder ==
                         VIDEO_DECODER_TYPE::HARDWARE_VIDEO_DECODER
                     ? "Hardware"
                 : config->video_decoder ==
                         VIDEO_DECODER_TYPE::SOFTWARE_VIDEO_DECODER
                     ? "Software"
                     : "Off"),
            std::string("Experimental better screen: ") +
                (config->experimental_better_screen ? "enabled" : "disabled"),
            std::string("Experimental stable stream: ") +
                (config->experimental_stable_stream ? "enabled" : "disabled"),
            std::string("Experimental ultra potato: ") +
                (config->experimental_ultra_potato ? "enabled" : "disabled"),
            std::string("Motion controls: ") +
                (config->motion_controls ? "enabled" : "disabled"),
            std::string("Swap face buttons: ") +
                (config->swap_face_buttons ? "enabled" : "disabled"),
            std::string("Swap triggers and shoulders: ") +
                (config->swap_triggers_and_shoulders ? "enabled" : "disabled"),
            std::string("Triggers as mouse buttons: ") +
                (config->use_triggers_for_mouse ? "enabled" : "disabled"),
        };
        std::vector<MenuRowAction> row_actions;
        for (const auto &key : setting_keys) {
            MenuRowAction action;
            const bool *flag = auto_flag_for(config, key);
            if (flag != nullptr) {
                action.label = "AUTO";
                action.active = *flag;
            }
            row_actions.push_back(action);
        }

        std::string subtitle;
        std::string footer_hint;
        if (config->stream.width % GSP_SCREEN_HEIGHT_TOP &&
            config->stream.width % GSP_SCREEN_HEIGHT_BOTTOM) {
            subtitle = "Unsupported width: the 3DS expects a multiple of 400 "
                       "or 320.";
        } else if (config->stream.height % GSP_SCREEN_WIDTH &&
                   !(g_potato.is_potato &&
                     (config->stream.height == POTATO_HEIGHT ||
                      config->stream.height == POTATO_BETTER_HEIGHT))) {
            subtitle = "Unsupported height: the 3DS expects 240 lines, or "
                       "180 / 225 in fill modes.";
        } else if (g_potato.is_potato) {
            subtitle = "AUTO: follows the Old 3DS profile.";
        } else {
            subtitle = "Adjust the stream profile.";
        }
        if (g_potato.is_potato) {
            footer_hint = "D-Pad: move   A: edit   Right: AUTO";
        }

        bool toggle_auto = false;
        idx = console_selection_prompt("Stream settings", setting_names,
                                       idx, subtitle,
                                       build_profile_status(config),
                                       footer_hint, &row_actions, &toggle_auto);
        if (idx < 0) {
            break;
        }

        bool *auto_flag = auto_flag_for(config, setting_keys[idx]);
        if (auto_flag != nullptr && toggle_auto) {
            *auto_flag = !*auto_flag;
            if (*auto_flag) {
                potato_apply_config(config);
                menu_ui_show_toast("AUTO on: value follows the Old 3DS "
                                   "profile.");
            } else {
                menu_ui_show_toast("AUTO off: value is yours to set.");
            }
            continue;
        }
        const int value_before =
            auto_flag != nullptr ? stream_value_for(config, setting_keys[idx])
                                 : 0;

        if ("width" == setting_keys[idx]) {
            config->stream.width =
                prompt_for_number(SETTING_WIDTH, config->stream.width);
        } else if ("height" == setting_keys[idx]) {
            config->stream.height =
                prompt_for_number(SETTING_HEIGHT, config->stream.height);
        } else if ("motion_controls" == setting_keys[idx]) {
            config->motion_controls = prompt_for_boolean(
                "Enable Motion Controls", config->motion_controls);
        } else if ("fps" == setting_keys[idx]) {
            config->stream.fps =
                prompt_for_number(SETTING_FPS, config->stream.fps);
        } else if ("bitrate" == setting_keys[idx]) {
            config->stream.bitrate =
                prompt_for_number(SETTING_BITRATE, config->stream.bitrate);
        } else if ("packetsize" == setting_keys[idx]) {
            config->stream.packetSize =
                prompt_for_number(SETTING_PACKET, config->stream.packetSize);
        } else if ("sops" == setting_keys[idx]) {
            config->sops = prompt_for_boolean(
                "Optimize Game settings for streaming", config->sops);
        } else if ("localaudio" == setting_keys[idx]) {
            config->localaudio =
                prompt_for_boolean("Play audio on host only",
                                   config->localaudio);
        } else if ("quitappafter" == setting_keys[idx]) {
            config->quitappafter = prompt_for_boolean(
                "Quit app after streaming", config->quitappafter);
        } else if ("viewonly" == setting_keys[idx]) {
            config->viewonly = prompt_for_boolean("Disable controller input",
                                                  config->viewonly);
        } else if ("video_decoder" == setting_keys[idx]) {
            config->video_decoder =
                prompt_for_video_decoder(config->video_decoder);
        } else if ("better_screen" == setting_keys[idx]) {
            config->experimental_better_screen = prompt_for_boolean(
                "Experimental Better Screen\nSharper profile with zoom-to-fill "
                "crop on the top screen. Costs more decode time.",
                config->experimental_better_screen);
            if (g_potato.is_potato) {
                potato_apply_config(config);
            }
        } else if ("stable_stream" == setting_keys[idx]) {
            config->experimental_stable_stream = prompt_for_boolean(
                "Experimental Stable Stream\nLower FPS and more aggressive "
                "late-frame dropping to reduce freezes.",
                config->experimental_stable_stream);
            if (g_potato.is_potato) {
                potato_apply_config(config);
            }
        } else if ("ultra_potato" == setting_keys[idx]) {
            config->experimental_ultra_potato = prompt_for_boolean(
                "Experimental Ultra Potato\nStarts on an ultra-low profile "
                "and automatically hardens frame skipping when slow frames "
                "pile up live.",
                config->experimental_ultra_potato);
            if (g_potato.is_potato) {
                potato_apply_config(config);
            }
        } else if ("swapfacebuttons" == setting_keys[idx]) {
            config->swap_face_buttons = prompt_for_boolean(
                "Swaps A/B and X/Y to match Xbox controller layout",
                config->swap_face_buttons);
        } else if ("swaptriggersandshoulders" == setting_keys[idx]) {
            config->swap_triggers_and_shoulders = prompt_for_boolean(
                "Swaps L/ZL and R/ZR for a more natural feel",
                config->swap_triggers_and_shoulders);
        } else if ("usetriggersformouse" == setting_keys[idx]) {
            config->use_triggers_for_mouse =
                prompt_for_boolean("Use ZL/ZR as left/right mouse buttons",
                                   config->use_triggers_for_mouse);
        }

        if (auto_flag != nullptr && *auto_flag &&
            stream_value_for(config, setting_keys[idx]) != value_before) {
            *auto_flag = false;
            menu_ui_show_toast("AUTO off: value set manually.");
        }
    }

    // Update the config file
    char *config_file_path = (char *)STREAMPOTATO_CONFIG_PATH;
    config_save(config_file_path, config);
}

static void init_3ds() {
    Result status = 0;
    acInit();
    g_ac_initialized = true;
    gfxInit(GSP_RGB565_OES, GSP_RGB565_OES, false);
    g_gfx_initialized = true;
    gfxSetDoubleBuffering(GFX_TOP, false);
    gfxSetDoubleBuffering(GFX_BOTTOM, false);

    consoleInit(GFX_TOP, &DebugTouchHandler::topScreen);
    consoleInit(GFX_BOTTOM, &DebugTouchHandler::bottomScreen);
    consoleSelect(&DebugTouchHandler::topScreen);
    menu_ui_init();
    atexit(n3ds_exit_handler);

    osSetSpeedupEnable(true);
    aptSetSleepAllowed(false);
    aptInit();
    g_apt_initialized = true;

    // n3DS has 4 cores 
    // o3DS has 2 cores
    bool is_new_3ds = false;
    APT_CheckNew3DS(&is_new_3ds);
    u32 cpu_limit = is_new_3ds ? 80 : 30;
    Result cpu_limit_status = APT_SetAppCpuTimeLimit(cpu_limit);
    if (R_FAILED(cpu_limit_status)) {
        printf("Warning: APT_SetAppCpuTimeLimit(%lu) failed: %08lX\n",
               cpu_limit, cpu_limit_status);
    }

    SOC_buffer = (u32 *)memalign(SOC_ALIGN, SOC_BUFFERSIZE);
    status = socInit(SOC_buffer, SOC_BUFFERSIZE);
    if (R_FAILED(status)) {
        printf("socInit: %08lX\n", status);
        exit(1);
    }
    g_soc_initialized = true;

    // bg jobs share curl with the main thread
    curl_global_init(CURL_GLOBAL_DEFAULT);
    g_curl_initialized = true;

    status = ndmuInit();
    if (R_SUCCEEDED(status)) {
        g_ndmu_initialized = true;
        status = NDMU_EnterExclusiveState(NDM_EXCLUSIVE_STATE_INFRASTRUCTURE);
        if (R_SUCCEEDED(status)) {
            g_ndmu_exclusive = true;
            status = NDMU_LockState();
            if (R_SUCCEEDED(status)) {
                g_ndmu_locked = true;
            }
        }
    }
    if (R_FAILED(status)) {
        printf("Warning: failed to enter exclusive NDM state: %08lX\n", status);
        wait_for_button();
    }
}

static int prompt_for_app_id(PSERVER_DATA server) {
    AppListTaskContext task_context = {
        .server = server,
    };
    int status = run_loading_task(
        "Loading apps",
        "Fetching the host application list. If this never finishes, restart "
        "Sunshine on the host: it stops answering this port once a connection "
        "is left open.",
        build_profile_status(), "Querying the paired host", app_list_task,
        &task_context);
    if (status != GS_OK || task_context.list == NULL) {
        char buffer[450];
        snprintf(buffer, sizeof(buffer),
                 "Unable to load host app list from %s:%u (https): %s.\n\n"
                 "If Sunshine shows 'Client certificate identity is not enabled',\n"
                 "remove old PotatoStream clients in Sunshine, then choose\n"
                 "'Unpair host' here and pair again.",
                 server->serverInfo.address != NULL ? server->serverInfo.address
                                                    : "host",
                 server->httpsPort,
                 gs_error != NULL ? gs_error : "unknown error");
        wait_for_button(buffer);
        return -1;
    }

    PAPP_LIST list = task_context.list;
    std::vector<std::string> app_names;
    std::vector<int> app_ids;
    while (list != NULL) {
        printf("%d. %s\n", list->id, list->name);
        app_names.push_back(std::string(list->name));
        app_ids.push_back(list->id);
        list = list->next;
    }

    if (app_ids.empty()) {
        wait_for_button("No launchable apps were returned by the host.");
        return -1;
    }

    char subtitle[256];
    snprintf(subtitle, sizeof(subtitle), "GPU: %s | GFE: %s",
             server->gpuType != NULL ? server->gpuType : "unknown",
             server->serverInfo.serverInfoGfeVersion != NULL
                 ? server->serverInfo.serverInfoGfeVersion
                 : "unknown");
    int id_idx = console_selection_prompt("Select an app", app_names, 0,
                                          subtitle, build_profile_status());
    if (id_idx == -1) {
        return -1;
    }
    return app_ids[id_idx];
}

static inline void dispatch_loop(void *_unused_) {
    auto pDispatcher = MessageDispatcher::get_instance();
    auto connection_listener = N3dsConnectionListener::get_instance();
    while (!connection_listener->is_connection_closed()) {
        gspWaitForAnyEvent();
        pDispatcher->dispatch_all();
    }
}

static inline void input_loop(void *input_handler_in) {
    N3dsInput *input_handler = static_cast<N3dsInput *>(input_handler_in);
    auto connection_listener = N3dsConnectionListener::get_instance();
    input_handler->force_touchscreen_menu();
    while (!connection_listener->is_connection_closed()) {
        gspWaitForAnyEvent();
        input_handler->n3dsinput_handle_event();
    }
}

static inline void stream_loop(N3dsConnectionListener *connection_listener,
                               std::shared_ptr<N3dsInput> input_handler) {
    // Spin off worker threads
    size_t stack_size = 0x20000;
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    threadCreate(input_loop, input_handler.get(), stack_size, priority, -1,
                 true);
    threadCreate(dispatch_loop, nullptr, stack_size, priority, -1, true);

    // run main connection loop
    while (!connection_listener->is_connection_closed() && aptMainLoop()) {
        gspWaitForAnyEvent();
        if (aptShouldClose()) {
            connection_listener->connection_terminated(0);
        }
    }
}

static void stream(PSERVER_DATA server, PCONFIGURATION config, int appId,
                   std::shared_ptr<N3dsInput> input_handler) {
    potato_apply_config(config);

    int gamepad_mask = 1;
    StartAppTaskContext start_context = {
        .server = server,
        .stream = &config->stream,
        .appId = appId,
        .sops = config->sops,
        .localaudio = config->localaudio,
        .gamepad_mask = gamepad_mask,
    };
    int ret = run_loading_task("Preparing stream",
                               "Negotiating the stream session with the host.",
                               build_profile_status(config),
                               "Launching app and validating the stream mode",
                               start_app_task, &start_context);
    if (ret < 0) {
        if (ret == GS_NOT_SUPPORTED_4K)
            printf("Server doesn't support 4K\n");
        else if (ret == GS_NOT_SUPPORTED_MODE)
            printf("Server doesn't support %dx%d (%d fps) or remove "
                   "--nounsupported option\n",
                   config->stream.width, config->stream.height,
                   config->stream.fps);
        else if (ret == GS_NOT_SUPPORTED_SOPS_RESOLUTION)
            printf(
                "Optimal Playable Settings isn't supported for the resolution "
                "%dx%d, use supported resolution or disable 'sops' option\n",
                config->stream.width, config->stream.height);
        else if (ret == GS_ERROR)
            printf("Gamestream error: %s\n", gs_error);
        else
            printf("Errorcode starting app: %d\n", ret);
        menu_ui_init();
        wait_for_button();
        return;
    }

    menu_ui_shutdown();
    draw_stream_wait_screen(config, "Loading...",
                            "Waiting for the first video frame.");

    AUDIO_RENDERER_CALLBACKS *audio_callbacks =
        config->localaudio ? &audio_callbacks_mock : &audio_callbacks_n3ds;

    PDECODER_RENDERER_CALLBACKS video_callbacks = &decoder_callbacks_mock;
    switch (config->video_decoder) {
    case (VIDEO_DECODER_TYPE::HARDWARE_VIDEO_DECODER):
        if (g_potato.is_potato) {
            printf("[POTATO] Hardware decode is unavailable on Old 3DS/2DS, using StreamPotato software decoder\n");
            video_callbacks = &decoder_callbacks_potato;
        } else {
            video_callbacks = &decoder_callbacks_n3ds_mvd;
        }
        break;
    case (VIDEO_DECODER_TYPE::SOFTWARE_VIDEO_DECODER):
        if (!g_potato.is_potato) {
            decoder_callbacks_n3ds.capabilities =
                CAPABILITY_DIRECT_SUBMIT |
                CAPABILITY_REFERENCE_FRAME_INVALIDATION_AVC |
                CAPABILITY_SLICES_PER_FRAME(2);
        } else {
            decoder_callbacks_n3ds.capabilities =
                CAPABILITY_DIRECT_SUBMIT |
                CAPABILITY_REFERENCE_FRAME_INVALIDATION_AVC;
        }
        video_callbacks =
            g_potato.is_potato ? &decoder_callbacks_potato
                               : &decoder_callbacks_n3ds;
        break;
    default:
        break;
    }

    printf(
        "Loading...\nStream %dx%d, %dfps, %dkbps, sops=%d, localaudio=%d, quitappafter=%d,\
 viewonly=%d, encryption=%x, video_decoder=%d, swapfacebuttons=%d, swaptriggersandshoulders=%d,\
 usetriggersformouse=%d, motion_controls=%d\n",
        config->stream.width, config->stream.height, config->stream.fps,
        config->stream.bitrate, config->sops, config->localaudio,
        config->quitappafter, config->viewonly, config->stream.encryptionFlags,
        config->video_decoder, config->swap_face_buttons,
        config->swap_triggers_and_shoulders, config->use_triggers_for_mouse,
        config->motion_controls);

    auto connection_listener =
        N3dsConnectionListener::create_instance(config->motion_controls);
    int status = LiStartConnection(&server->serverInfo, &config->stream,
                                   &n3ds_connection_callbacks, video_callbacks,
                                   audio_callbacks, NULL, DISPLAY_FULLSCREEN,
                                   config->audio_device, 0);

    if (status != 0) {
        menu_ui_init();
        printf("Connection failed with error: %d\n", status);
        std::string failure_message = build_stream_end_message(connection_listener);
        if (failure_message == "The stream stopped.") {
            char buffer[192];
            snprintf(buffer, sizeof(buffer),
                     "Connection setup failed during %s. Error code: %d.",
                     stage_name(connection_listener->get_last_stage()), status);
            failure_message = buffer;
        }
        wait_for_button(failure_message);
        N3dsConnectionListener::destroy_instance();
        return;
    }

    printf("Connected!\n");
    stream_loop(connection_listener, input_handler);

    const std::string stream_end_message =
        build_stream_end_message(connection_listener);
    const int stream_end_error = connection_listener->get_last_error_code();

    LiStopConnection();
    N3dsConnectionListener::destroy_instance();
    menu_ui_init();

    if (config->quitappafter) {
        ServerTaskContext task_context = {
            .server = server,
        };
        run_loading_task("Closing app", "Sending the host app quit request.",
                         build_profile_status(config),
                         "Stopping the running session on the PC",
                         quit_app_task, &task_context);
        server->currentGame = 0;
    }

    if (stream_end_error != 0) {
        wait_for_button(stream_end_message);
    }
}

static int init_server(CONFIGURATION *config, SERVER_DATA *server) {
    printf("Connecting to %s:%d...\n", config->address, config->port);
    http_set_timeout_s(10);
    gs_cleanup();
    int status = gs_init(server, config->address, config->port, config->key_dir,
                         0, config->unsupported);
    http_set_timeout_s(60);

    if (gs_cert_was_regenerated()) {
        printf("[cert] Certificate was regenerated - clearing all confirmed pairs.\n");
        printf("[cert] You will need to re-pair with each Sunshine host.\n");
        clear_confirmed_pairs();

        server->paired = false;
    }

    if (status == GS_OUT_OF_MEMORY) {
        printf("Not enough memory\n");
        return 1;
    } else if (status == GS_ERROR) {
        printf("Gamestream error: %s\n", gs_error);
        return 1;
    } else if (status == GS_INVALID) {
        printf("Invalid data received from server: %s\n", gs_error);
        return 1;
    } else if (status == GS_UNSUPPORTED_VERSION) {
        printf("Unsupported version: %s\n", gs_error);
        return 1;
    } else if (status != GS_OK) {
        printf("Can't connect to server %s:%d\n", config->address,
               config->port);
        return 1;
    }

    printf("GPU: %s, GFE: %s (%s, %s)\n", server->gpuType,
           server->serverInfo.serverInfoGfeVersion, server->gsVersion,
           server->serverInfo.serverInfoAppVersion);
    printf("Server codec flags: 0x%x\n",
           server->serverInfo.serverCodecModeSupport);

    if (!server->paired && !server->isNvidiaSoftware &&
        is_confirmed_pair(config->address, config->port)) {
        server->paired = true;
    }

    if (server->paired) {
        add_pair_address(config->address, config->port);
    } else {
        remove_pair_address(config->address, config->port);
    }
    return 0;
}

static void action_stream(CONFIGURATION *config, SERVER_DATA *server) {
    potato_apply_config(config);

    int appId = prompt_for_app_id(server);
    if (appId == -1) {
        return;
    }

    config->stream.supportedVideoFormats = VIDEO_FORMAT_H264;

    if (config->viewonly) {
        printf("View-only mode enabled, no input will be sent "
               "to the host computer\n");
    }
    std::shared_ptr<N3dsInput> input_handler = std::make_shared<N3dsInput>(
        config->stream.width, config->stream.height, config->swap_face_buttons,
        config->swap_triggers_and_shoulders, config->use_triggers_for_mouse,
        config->viewonly);
    stream(server, config, appId, input_handler);
}

static void action_pair(CONFIGURATION *config, SERVER_DATA *server) {
    http_set_timeout_s(90);

    char pin[5];
    unsigned char pin_bytes[4];
    if (RAND_bytes(pin_bytes, sizeof(pin_bytes)) == 1) {
        snprintf(pin, sizeof(pin), "%u%u%u%u", pin_bytes[0] % 10u,
                 pin_bytes[1] % 10u, pin_bytes[2] % 10u, pin_bytes[3] % 10u);
    } else {
        snprintf(pin, sizeof(pin), "%u%u%u%u", (unsigned)random() % 10u,
                 (unsigned)random() % 10u, (unsigned)random() % 10u,
                 (unsigned)random() % 10u);
    }
    PairTaskContext task_context = {
        .server = server,
        .pin = &pin[0],
    };
    int status = run_loading_task(
        "Pair With Host", "Enter this PIN on the PC, then validate pairing.",
        build_profile_status(), "Waiting for host confirmation", pair_task,
        &task_context, pin);

    if (status != GS_OK) {
        char buffer[256];
        snprintf(buffer, sizeof(buffer), "Failed to pair with host: %s",
                 gs_error != NULL ? gs_error : "unknown error");
        wait_for_button(buffer);
    } else {
        server->paired = true;
        add_pair_address(config->address, config->port);
        add_confirmed_pair(config->address, config->port);
        wait_for_button("Host paired successfully.");
    }

    // default HTTP timeout
    http_set_timeout_s(60);
}

static void action_unpair(CONFIGURATION *config, SERVER_DATA *server) {
    ServerTaskContext task_context = {
        .server = server,
    };
    int status =
        run_loading_task("Unpair Host", "Removing the pairing with this PC.",
                         build_profile_status(),
                         "Revoking the current client trust", unpair_task,
                         &task_context);

    // reset local pairing state
    server->paired = false;
    remove_pair_address(config->address, config->port);
    remove_confirmed_pair(config->address, config->port);

    if (status != GS_OK) {
        char buffer[256];
        snprintf(buffer, sizeof(buffer),
                 "Host unpair error: %s.\nLocal pairing records have been cleared.\nYou can now pair again.",
                 gs_error != NULL ? gs_error : "unknown error");
        wait_for_button(buffer);
    } else {
        wait_for_button("Host unpaired successfully.");
    }
}

static void action_quit_stream(SERVER_DATA *server) {
    ServerTaskContext task_context = {
        .server = server,
    };
    int status =
        run_loading_task("Quit Running App",
                         "Sending the app stop request to the host.",
                         build_profile_status(),
                         "Stopping the active host session", quit_app_task,
                         &task_context);
    if (status != GS_OK) {
        wait_for_button("The host app did not stop cleanly.");
        return;
    }

    server->currentGame = 0;
    wait_for_button("Host app stopped.");
}

int main_loop(int argc, char *argv[]) {
    init_3ds();

    srandom((unsigned int)svcGetSystemTick());

    CONFIGURATION config;
    config_parse(argc, argv, &config);
    if (potato_init()) {
        potato_apply_config(&config);
    }

    background_start(update_check_task, &g_update_result,
                     on_update_check_done);

    while (aptMainLoop()) {
        bool should_exit = false;
        auto address_string = prompt_for_address(&config, &should_exit);
        if (should_exit) {
            break;
        }
        if (address_string.empty()) {
            continue;
        }

        unsigned short connection_port = config.port;
        size_t port_delim_pos = address_string.find(':');
        if (port_delim_pos != std::string::npos) {
            std::string port_string = address_string.substr(port_delim_pos + 1);
            address_string = address_string.substr(0, port_delim_pos);
            errno = 0;
            long parsed_port = strtol(port_string.c_str(), nullptr, 10);
            if (errno != 0 || parsed_port <= 0 || parsed_port > 65535) {
                printf("Invalid stored port '%s', using default %d\n",
                       port_string.c_str(), config.port);
            } else {
                connection_port = (unsigned short)parsed_port;
            }
        }

        const unsigned short saved_port = config.port;
        config.port = connection_port;
        config.address = (char *)address_string.c_str();

        SERVER_DATA server;
        memset(&server, 0, sizeof(server));
        InitServerTaskContext init_context = {
            .config = &config,
            .server = &server,
        };
        if (run_loading_task("Connecting to host",
                             "Checking pairing state and host information.",
                             build_profile_status(&config),
                             "Connecting to the selected IP/host",
                             init_server_task, &init_context)) {
            config.port = saved_port;
            wait_for_button();
            continue;
        }

        if (gs_cert_was_regenerated()) {
            menu_ui_show_toast(
                "New security cert generated - please re-pair with your host!",
                6.0f);
        }

        while (aptMainLoop()) {
            std::string action = prompt_for_action(&server);
            if (action.empty()) {
                break;
            }
            config.action = (char *)action.c_str();

            if (strcmp("stream", config.action) == 0) {
                action_stream(&config, &server);
            } else if (strcmp("pair", config.action) == 0) {
                action_pair(&config, &server);
            } else if (strcmp("stream settings", config.action) == 0) {
                prompt_for_stream_settings(&config);
            } else if (strcmp("unpair", config.action) == 0) {
                action_unpair(&config, &server);
            } else if (strcmp("quit stream", config.action) == 0) {
                action_quit_stream(&server);
            } else {
                printf("%s is not a valid action\n", config.action);
                wait_for_button();
            }
        }

        config.port = saved_port;
    }

    gs_cleanup();
    return 0;
}

int main(int argc, char *argv[]) {
    int status = 0;
    try {
        main_loop(argc, argv);
    } catch (const std::exception &ex) {
        printf("StreamPotato crashed with the following error: %s\n",
               ex.what());
        status = 1;
    } catch (const std::string &ex) {
        printf("StreamPotato crashed with the following error message: %s\n",
               ex.c_str());
        status = 1;
    } catch (...) {
        printf("StreamPotato crashed with an unknown error\n");
        status = 1;
    }

    gs_cleanup();
    http_shutdown();
    return status;
}
