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
#include "potato/potato_flux_client.h"
#include "potato/potato_profile.h"
#include "system/dispatcher.hpp"
#include "system/n3ds_connection.hpp"
#include "system/pair_record.hpp"
#include "video/video.hpp"

#include <3ds.h>
#include <Limelight.h>

#include <client.h>
#include <discover.h>
#include <http.h>

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cctype>
#include <exception>
#include <limits>
#include <malloc.h>
#include <netdb.h>
#include <netinet/in.h>
#include <openssl/rand.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define SOC_ALIGN 0x1000
// 0x40000 for each enet host (2 hosts total)
// 0x40000 for each platform socket (2 sockets total)
#define SOC_BUFFERSIZE 0x100000

#define MAX_INPUT_CHAR 60
static constexpr size_t ASYNC_TASK_STACK_SIZE = 0x80000;
static constexpr int STREAM_THREAD_INIT_ERROR = -10000;
static constexpr int ASYNC_TASK_UNEXPECTED_EXIT_ERROR = -10001;
static constexpr u32 ASYNC_TASK_MAGIC = 0x53505454;
static constexpr bool POTATOFLUX_MENU_ENABLED = false;
static constexpr const char *POTATOFLUX_DIRECT_ACTION =
    "potatoflux://listen";

static u32 *SOC_buffer = NULL;
static bool g_ac_initialized = false;
static bool g_gfx_initialized = false;
static bool g_apt_initialized = false;
static bool g_soc_initialized = false;
static bool g_ndmu_initialized = false;
static bool g_ndmu_exclusive = false;
static bool g_ndmu_locked = false;

static int init_server(CONFIGURATION *config, SERVER_DATA *server);
static std::string build_profile_status(PCONFIGURATION config = nullptr);
static std::string describe_video_decoder(VIDEO_DECODER_TYPE decoder,
                                          bool safe_mode_note = false);
static std::string sanitize_stream_settings(PCONFIGURATION config);
static bool try_parse_port(const std::string &port_text,
                           unsigned short *port_out);
static void wait_for_all_buttons_release();
static std::string build_connect_failure_message(PCONFIGURATION config,
                                                 int status);

static const char *safe_cstr(const char *value,
                             const char *fallback = "unknown") {
    return (value != nullptr && value[0] != '\0') ? value : fallback;
}

struct AsyncTask {
    u32 magic = ASYNC_TASK_MAGIC;
    int (*fn)(void *) = nullptr;
    void *context = nullptr;
    std::atomic<bool> done = false;
    std::atomic<bool> active = false;
    std::atomic<int> result = -1;
};

static AsyncTask g_async_task;

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

static void async_task_entry(void *arg) {
    (void)arg;
    AsyncTask *task = &g_async_task;
    if (task == nullptr || task->magic != ASYNC_TASK_MAGIC ||
        task->fn == nullptr) {
        gs_error =
            "The background connection worker was initialized with an invalid callback.";
        if (task != nullptr) {
            task->result.store(ASYNC_TASK_UNEXPECTED_EXIT_ERROR,
                               std::memory_order_release);
            task->done.store(true, std::memory_order_release);
        }
        return;
    }
    task->result.store(task->fn(task->context), std::memory_order_release);
    task->done.store(true, std::memory_order_release);
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

static int run_loading_task(const std::string &title, const std::string &body,
                            const std::string &status,
                            const std::string &footer_hint, int (*fn)(void *),
                            void *context,
                            const std::string &pin = std::string()) {
    struct HttpCancelScope {
        HttpCancelScope() { http_set_cancelled(false); }
        ~HttpCancelScope() { http_set_cancelled(false); }
    } http_cancel_scope;

    if (!menu_ui_is_active()) {
        return fn(context);
    }

    if (g_async_task.active.load()) {
        return fn(context);
    }

    AsyncTask *task = &g_async_task;
    task->magic = ASYNC_TASK_MAGIC;
    task->fn = fn;
    task->context = context;
    task->result.store(-1, std::memory_order_release);
    task->done.store(false, std::memory_order_release);
    task->active.store(true);

    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    Thread worker = threadCreate(async_task_entry, nullptr, ASYNC_TASK_STACK_SIZE,
                                 priority, -1, false);
    if (worker == nullptr) {
        task->active.store(false);
        return fn(context);
    }

    int frame = 0;
    bool cancel_requested = false;
    auto task_finished = [&]() {
        return task->done.load(std::memory_order_acquire);
    };
    while (aptMainLoop()) {
        if (task_finished()) {
            break;
        }

        if (aptShouldClose()) {
            cancel_requested = true;
            http_set_cancelled(true);
        }

        hidScanInput();
        const u32 keys = hidKeysDown();
        if ((keys & KEY_B) || (keys & KEY_START)) {
            cancel_requested = true;
            gs_error = "Connection cancelled.";
            http_set_cancelled(true);
        }

        const std::string active_footer =
            cancel_requested ? "Cancelling network request..."
                             : footer_hint + " | B/START: cancel";
        if (pin.empty()) {
            menu_ui_draw_loading(title, body, status, active_footer, frame);
        } else {
            menu_ui_draw_pairing(title, pin, body, status, active_footer,
                                 frame);
        }
        gspWaitForVBlank();
        frame++;
    }

    if (!task_finished()) {
        cancel_requested = true;
        http_set_cancelled(true);
        while (!task_finished()) {
            svcSleepThread(1000000ULL);
            gspWaitForVBlank();
        }
    }

    threadJoin(worker, ~0ULL);
    threadFree(worker);

    if (!task_finished()) {
        gs_error = cancel_requested
                       ? "Background network task was cancelled while closing the app."
                       : "Background network task terminated unexpectedly.";
        task->active.store(false);
        return ASYNC_TASK_UNEXPECTED_EXIT_ERROR;
    }
    const int result = task->result.load(std::memory_order_acquire);
    task->fn = nullptr;
    task->context = nullptr;
    task->active.store(false);
    return result;
}

static std::string prompt_for_text_input(const std::string &hint_text,
                                         const std::string &initial_text,
                                         SwkbdType keyboard_type,
                                         int max_length, int input_length) {
    wait_for_all_buttons_release();

    const bool had_menu_ui = menu_ui_is_active();
    if (had_menu_ui) {
        menu_ui_shutdown();
    }

    std::vector<char> buffer(max_length + 1, '\0');
    SwkbdState swkbd;
    int max_text_length = max_length;
    if (input_length > 0) {
        max_text_length = std::min(max_length, input_length);
    }
    swkbdInit(&swkbd, keyboard_type,
              keyboard_type == SWKBD_TYPE_NUMPAD ? 1 : 3, max_text_length);
    if (!hint_text.empty()) {
        swkbdSetHintText(&swkbd, hint_text.c_str());
    }
    if (!initial_text.empty()) {
        swkbdSetInitialText(&swkbd, initial_text.c_str());
    }
    SwkbdButton button =
        swkbdInputText(&swkbd, buffer.data(), static_cast<int>(buffer.size()));

    if (had_menu_ui) {
        menu_ui_init();
    }

    if (button != SWKBD_BUTTON_RIGHT) {
        return "";
    }

    std::string text = buffer.data();
    trim(text);
    return text;
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
        bool stereo_3d =
            config != nullptr ? config->experimental_stereoscopic_3d
                              : g_potato.experimental_stereoscopic_3d;
        int width = stereo_3d ? POTATO_STEREO_WIDTH
                    : ultra_potato ? POTATO_ULTRA_WIDTH
                                 : (better_screen ? POTATO_BETTER_WIDTH
                                                  : POTATO_WIDTH);
        int height = stereo_3d ? POTATO_STEREO_HEIGHT
                     : ultra_potato ? POTATO_ULTRA_HEIGHT
                                  : (better_screen ? POTATO_BETTER_HEIGHT
                                                   : POTATO_HEIGHT);
        int fps = stereo_3d ? POTATO_STEREO_FPS
                  : ultra_potato ? POTATO_ULTRA_FPS
                                : stable_stream ? POTATO_STABLE_FPS
                                : (better_screen ? POTATO_BETTER_FPS
                                                 : POTATO_FPS);
        int bitrate = stereo_3d
                          ? POTATO_STEREO_BITRATE_KBPS
                          : ultra_potato
                          ? POTATO_ULTRA_BITRATE_KBPS
                          : stable_stream
                          ? (better_screen ? POTATO_STABLE_BETTER_BITRATE_KBPS
                                           : POTATO_STABLE_BITRATE_KBPS)
                          : (better_screen ? POTATO_BETTER_BITRATE_KBPS
                                           : POTATO_BITRATE_KBPS);
        int packet_size = stereo_3d ? POTATO_STABLE_PACKET_SIZE
                          : ultra_potato ? POTATO_ULTRA_PACKET_SIZE
                           : stable_stream ? POTATO_STABLE_PACKET_SIZE
                                           : POTATO_PACKET_SIZE;
        const bool host_audio =
            config != nullptr ? config->localaudio : g_potato.host_audio;
        char experimental[128];
        snprintf(experimental, sizeof(experimental), "%s%s%s%s%s",
                 stereo_3d ? "stereoscopic 3D"
                           : (ultra_potato ? "ultra potato auto" : ""),
                 ultra_potato && (better_screen || stable_stream) ? " + " : "",
                 better_screen ? "better screen" : "",
                 (better_screen && stable_stream && !ultra_potato) ? " + " : "",
                 stable_stream ? "stable stream" : "");
        snprintf(buffer, sizeof(buffer), "%s: %dx%d | %dfps | %dkbps | packet %d | audio %s%s%s",
                 g_potato.dynamic_ultra_active ? "EMERGENCY MODE" : "SAFE MODE active",
                 width, height, fps, bitrate, packet_size,
                 host_audio ? "host" : "3DS",
                 experimental[0] != '\0' ? " | " : "",
                 experimental);
        return buffer;
    }

    if (config == nullptr) {
        return "Normal mode active. Hardware decode is available on New 3DS.";
    }

    snprintf(buffer, sizeof(buffer),
             "Profile: %dx%d | %dfps | %dkbps | packet %d",
             config->stream.width, config->stream.height, config->stream.fps,
             config->stream.bitrate, config->stream.packetSize);
    return buffer;
}

static std::string describe_video_decoder(VIDEO_DECODER_TYPE decoder,
                                          bool safe_mode_note) {
    switch (decoder) {
    case VIDEO_DECODER_TYPE::HARDWARE_VIDEO_DECODER:
        return safe_mode_note ? "hardware (safe mode overrides to software)"
                              : "hardware";
    case VIDEO_DECODER_TYPE::SOFTWARE_VIDEO_DECODER:
        return "software";
    case VIDEO_DECODER_TYPE::DISABLE_VIDEO_DECODER:
        return "disabled";
    default:
        return "unknown";
    }
}

static std::string sanitize_stream_settings(PCONFIGURATION config) {
    if (config == nullptr) {
        return "";
    }

    std::vector<std::string> changes;
    auto clamp_setting = [&changes](const char *label, int *value, int minimum,
                                    int maximum, int fallback) {
        int original = *value;
        if (original < minimum || original > maximum) {
            *value = (original <= 0) ? fallback : std::max(minimum, std::min(original, maximum));

            char buffer[96];
            snprintf(buffer, sizeof(buffer), "%s set to %d", label, *value);
            changes.push_back(buffer);
        }
    };

    clamp_setting("Width", &config->stream.width, 64, 3840, 800);
    clamp_setting("Height", &config->stream.height, 64, 2160, 480);
    clamp_setting("FPS", &config->stream.fps, 5, 120, 60);
    clamp_setting("Bitrate", &config->stream.bitrate, 250, 50000, 1500);
    clamp_setting("Packet size", &config->stream.packetSize, 256, 1392, 1024);

    if (config->port == 0) {
        config->port = 47989;
        changes.push_back("Port reset to 47989");
    }

    if (config->video_decoder < VIDEO_DECODER_TYPE::HARDWARE_VIDEO_DECODER ||
        config->video_decoder > VIDEO_DECODER_TYPE::DISABLE_VIDEO_DECODER) {
        config->video_decoder = VIDEO_DECODER_TYPE::HARDWARE_VIDEO_DECODER;
        changes.push_back("Video decoder reset to hardware");
    }

    if (changes.empty()) {
        return "";
    }

    std::string summary =
        "Some settings were outside the stable range and were corrected:";
    for (const std::string &change : changes) {
        summary += "\n";
        summary += change;
    }
    return summary;
}

static bool try_parse_port(const std::string &port_text,
                           unsigned short *port_out) {
    if (port_out == nullptr || port_text.empty()) {
        return false;
    }

    if (!std::all_of(port_text.begin(), port_text.end(), [](unsigned char ch) {
            return std::isdigit(ch) != 0;
        })) {
        return false;
    }

    unsigned long parsed_port = strtoul(port_text.c_str(), NULL, 10);
    if (parsed_port == 0 ||
        parsed_port > std::numeric_limits<unsigned short>::max()) {
        return false;
    }

    *port_out = static_cast<unsigned short>(parsed_port);
    return true;
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
    case STREAM_THREAD_INIT_ERROR:
        return "Stream worker threads could not start on the 3DS. Close the app, free some memory, and retry.";
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

static std::string build_connect_failure_message(PCONFIGURATION config,
                                                 int status) {
    const char *address = config != nullptr ? safe_cstr(config->address, "?")
                                            : "?";
    const unsigned short port = config != nullptr ? config->port : 0;
    const char *error_text =
        (gs_error != nullptr && gs_error[0] != '\0') ? gs_error : nullptr;

    char buffer[320];
    if (status == ASYNC_TASK_UNEXPECTED_EXIT_ERROR) {
        if (error_text != nullptr) {
            snprintf(buffer, sizeof(buffer),
                     "Connection task failed while contacting %s:%u: %s",
                     address, port, error_text);
        } else {
            snprintf(buffer, sizeof(buffer),
                     "Connection task failed while contacting %s:%u. Retry "
                     "this host and keep the video decoder on software.",
                     address, port);
        }
        return buffer;
    }

    if (error_text != nullptr) {
        snprintf(buffer, sizeof(buffer),
                 "Could not query the host at %s:%u: %s", address, port,
                 error_text);
        return buffer;
    }

    snprintf(buffer, sizeof(buffer),
             "Could not query the host at %s:%u. Status %d. Verify the IP, "
             "the Sunshine web service, and local firewall rules.",
             address, port, status);
    return buffer;
}

static void wait_for_all_buttons_release() {
    for (int frames = 0; aptMainLoop() && frames < 18; frames++) {
        if (aptShouldClose()) {
            return;
        }

        hidScanInput();
        if (hidKeysHeld() == 0) {
            return;
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
        if (aptShouldClose()) {
            break;
        }

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

static std::string format_ipv4_octets(const int octets[4],
                                      int selected = -1) {
    char parts[4][16];
    for (int i = 0; i < 4; i++) {
        if (i == selected) {
            snprintf(parts[i], sizeof(parts[i]), "[%03d]", octets[i]);
        } else if (selected >= 0) {
            snprintf(parts[i], sizeof(parts[i]), "%03d", octets[i]);
        } else {
            snprintf(parts[i], sizeof(parts[i]), "%d", octets[i]);
        }
    }

    char buffer[80];
    snprintf(buffer, sizeof(buffer), "%s.%s.%s.%s", parts[0], parts[1],
             parts[2], parts[3]);
    return buffer;
}

static bool default_ipv4_octets(int octets[4]) {
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        return false;
    }

    sockaddr_in remote = {};
    remote.sin_family = AF_INET;
    remote.sin_port = htons(9);
    remote.sin_addr.s_addr = inet_addr("8.8.8.8");
    connect(fd, (sockaddr *)&remote, sizeof(remote));

    sockaddr_in local = {};
    socklen_t local_len = sizeof(local);
    bool ok = false;
    if (getsockname(fd, (sockaddr *)&local, &local_len) == 0) {
        uint32_t ip = ntohl(local.sin_addr.s_addr);
        octets[0] = (ip >> 24) & 0xff;
        octets[1] = (ip >> 16) & 0xff;
        octets[2] = (ip >> 8) & 0xff;
        octets[3] = 1;
        ok = true;
    }

    close(fd);
    return ok;
}

static std::string prompt_for_ipv4_address() {
    int octets[4] = {192, 168, 1, 1};
    default_ipv4_octets(octets);
    int selected = 3;
    int frame = 0;

    wait_for_all_buttons_release();
    while (aptMainLoop()) {
        char body[256];
        std::string ip = format_ipv4_octets(octets, selected);
        snprintf(body, sizeof(body),
                 "PC host IP:\n\n%s\n\nD-Pad: edit  L/R: octet\nX/Y: +/-10  A: save  B: cancel",
                 ip.c_str());
        menu_ui_draw_loading("Add New Host", body, build_profile_status(),
                             "Saved hosts are selectable from the host list",
                             frame++);
        gspWaitForVBlank();

        hidScanInput();
        u32 keys = hidKeysDown();
        if (keys & KEY_B) {
            return "";
        }
        if (keys & KEY_A) {
            return format_ipv4_octets(octets);
        }
        if (keys & (KEY_DLEFT | KEY_L)) {
            selected = std::max(0, selected - 1);
        }
        if (keys & (KEY_DRIGHT | KEY_R)) {
            selected = std::min(3, selected + 1);
        }

        int delta = 0;
        if (keys & KEY_DUP) {
            delta += 1;
        }
        if (keys & KEY_DDOWN) {
            delta -= 1;
        }
        if (keys & KEY_X) {
            delta += 10;
        }
        if (keys & KEY_Y) {
            delta -= 10;
        }
        if (delta != 0) {
            octets[selected] = std::max(0, std::min(255, octets[selected] + delta));
        }
    }

    return "";
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
                                    std::string status = "") {
    if (status.empty()) {
        status = build_profile_status();
    }

    int option_idx = default_idx;
    wait_for_all_buttons_release();

    while (aptMainLoop()) {
        if (aptShouldClose()) {
            return -1;
        }

        if (menu_ui_is_active()) {
            menu_ui_draw_menu(prompt, subtitle, options, option_idx, status,
                              "D-Pad: move   A: confirm   B: back");
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
                if ((int)i == option_idx) {
                    printf(">%s\n", options[i].c_str());
                } else {
                    printf("%s\n", options[i].c_str());
                }
            }
        }

        if (!menu_ui_is_active()) {
            gfxSwapBuffers();
            gfxFlushBuffers();
        }
        gspWaitForVBlank();

        hidScanInput();
        u32 kDown = hidKeysDown();

        if (kDown & KEY_A) {
            if (!menu_ui_is_active()) {
                consoleClear();
            }
            return option_idx;
        }
        if (kDown & KEY_B) {
            if (!menu_ui_is_active()) {
                consoleClear();
            }
            return -1;
        }
        if (kDown & KEY_DOWN) {
            if ((size_t)option_idx < options.size() - 1) {
                option_idx++;
            }
        } else if (kDown & KEY_UP) {
            if (option_idx > 0) {
                option_idx--;
            }
        }
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

static std::string prompt_for_address() {
    auto address_list = list_paired_addresses();
    const bool has_saved_hosts = !address_list.empty();
    if (has_saved_hosts) {
        address_list.push_back("Remove saved host");
    }
    if (POTATOFLUX_MENU_ENABLED) {
        address_list.push_back("PotatoFlux direct receiver");
    }
    address_list.push_back("Add new host");
    int idx =
        console_selection_prompt("Select a host", address_list, 0,
                                 "Choose a paired PC or add a direct IP.",
                                 build_profile_status());
    if (idx < 0) {
        return "";
    } else if (address_list[idx] == "PotatoFlux direct receiver") {
        return POTATOFLUX_DIRECT_ACTION;
    } else if (address_list[idx] == "Remove saved host") {
        auto saved_hosts = list_paired_addresses();
        int remove_idx = console_selection_prompt(
            "Remove saved host", saved_hosts, 0,
            "Select one saved host to delete.", build_profile_status());
        if (remove_idx >= 0) {
            remove_pair_address_entry(saved_hosts[remove_idx]);
        }
        return "";
    } else if (address_list[idx] != "Add new host") {
        return address_list[idx];
    }

    std::string host = prompt_for_ipv4_address();
    if (!host.empty()) {
        return host + ":47989";
    }
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

static int prompt_for_int(std::string initial_text) {
    std::string setting_str =
        prompt_for_text_input("", initial_text, SWKBD_TYPE_NUMPAD,
                              MAX_INPUT_CHAR - 1, 8);
    if (setting_str.empty()) {
        return std::stoi(initial_text);
    }

    try {
        return std::stoi(setting_str);
    } catch (...) {
        return std::stoi(initial_text);
    }
}

static void prompt_for_stream_settings(PCONFIGURATION config) {
    std::vector<std::string> setting_keys = {
        "width",    "height",      "fps",           "motion_controls",
        "bitrate",  "packetsize",  "sops",          "localaudio",
        "quitappafter",           "viewonly",       "video_decoder",
        "better_screen",          "stable_stream",
        "ultra_potato",             "stereoscopic_3d",
        "swapfacebuttons",        "swaptriggersandshoulders",
        "usetriggersformouse",
    };
    int idx = 0;
    while (1) {
        std::vector<std::string> setting_names = {
            "Width: " + std::to_string(config->stream.width),
            "Height: " + std::to_string(config->stream.height),
            "FPS: " + std::to_string(config->stream.fps),
            std::string("Motion controls: ") +
                (config->motion_controls ? "enabled" : "disabled"),
            "Bitrate: " + std::to_string(config->stream.bitrate) + " kbps",
            "Packet size: " + std::to_string(config->stream.packetSize),
            std::string("Optimize game settings: ") +
                (config->sops ? "enabled" : "disabled"),
            std::string("Audio on host only: ") +
                (config->localaudio ? "enabled" : "disabled"),
            std::string("Quit host app after stream: ") +
                (config->quitappafter ? "enabled" : "disabled"),
            std::string("View only mode: ") +
                (config->viewonly ? "enabled" : "disabled"),
            std::string("Video decoder: ") +
                describe_video_decoder(config->video_decoder, g_potato.is_potato),
            std::string("Experimental better screen: ") +
                (config->experimental_better_screen ? "enabled" : "disabled"),
            std::string("Experimental stable stream: ") +
                (config->experimental_stable_stream ? "enabled" : "disabled"),
            std::string("Experimental ultra potato: ") +
                (config->experimental_ultra_potato ? "enabled" : "disabled"),
            std::string("Experimental Stereoscopic 3D: ") +
                (config->experimental_stereoscopic_3d ? "enabled" : "disabled"),
            std::string("Swap face buttons: ") +
                (config->swap_face_buttons ? "enabled" : "disabled"),
            std::string("Swap triggers and shoulders: ") +
                (config->swap_triggers_and_shoulders ? "enabled" : "disabled"),
            std::string("Triggers as mouse buttons: ") +
                (config->use_triggers_for_mouse ? "enabled" : "disabled"),
        };

        std::string prompt = "Tune stream settings";
        if (config->stream.width % GSP_SCREEN_HEIGHT_TOP &&
            config->stream.width % GSP_SCREEN_HEIGHT_BOTTOM) {
            prompt += "\n\nWARNING: Using an unsupported width may "
                      "cause issues (3DS supports multiples of 400 or 320)\n";
        }
        if (config->stream.height % GSP_SCREEN_WIDTH) {
            if (!(g_potato.is_potato &&
                  (config->stream.height == POTATO_HEIGHT ||
                   config->stream.height == POTATO_BETTER_HEIGHT ||
                   config->stream.height == POTATO_STEREO_HEIGHT))) {
                prompt += "\n\nWARNING: Using an unsupported height may "
                          "cause issues (3DS usually expects 240 lines, "
                          "except StreamPotato fill modes at 180 / 225)\n";
            }
        }
        idx = console_selection_prompt(prompt, setting_names, idx,
                                       "Adjust the stream profile.",
                                       build_profile_status(config));
        if (idx < 0) {
            break;
        }

        if ("width" == setting_keys[idx]) {
            config->stream.width =
                prompt_for_int(std::to_string(config->stream.width));
        } else if ("height" == setting_keys[idx]) {
            config->stream.height =
                prompt_for_int(std::to_string(config->stream.height));
        } else if ("motion_controls" == setting_keys[idx]) {
            config->motion_controls = prompt_for_boolean(
                "Enable Motion Controls", config->motion_controls);
        } else if ("fps" == setting_keys[idx]) {
            config->stream.fps =
                prompt_for_int(std::to_string(config->stream.fps));
        } else if ("bitrate" == setting_keys[idx]) {
            config->stream.bitrate =
                prompt_for_int(std::to_string(config->stream.bitrate));
        } else if ("packetsize" == setting_keys[idx]) {
            config->stream.packetSize =
                prompt_for_int(std::to_string(config->stream.packetSize));
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
            if (config->experimental_better_screen) {
                config->experimental_stereoscopic_3d = false;
            }
            if (g_potato.is_potato) {
                potato_apply_config(config);
            }
        } else if ("stable_stream" == setting_keys[idx]) {
            config->experimental_stable_stream = prompt_for_boolean(
                "Experimental Stable Stream\nLower FPS and more aggressive "
                "late-frame dropping to reduce freezes.",
                config->experimental_stable_stream);
            if (config->experimental_stable_stream) {
                config->experimental_stereoscopic_3d = false;
            }
            if (g_potato.is_potato) {
                potato_apply_config(config);
            }
        } else if ("ultra_potato" == setting_keys[idx]) {
            config->experimental_ultra_potato = prompt_for_boolean(
                "Experimental Ultra Potato\nStarts on an ultra-low profile "
                "and automatically hardens frame skipping when slow frames "
                "pile up live.",
                config->experimental_ultra_potato);
            if (config->experimental_ultra_potato) {
                config->experimental_stereoscopic_3d = false;
            }
            if (g_potato.is_potato) {
                potato_apply_config(config);
            }
        } else if ("stereoscopic_3d" == setting_keys[idx]) {
            config->experimental_stereoscopic_3d = prompt_for_boolean(
                "Experimental Stereoscopic 3D\nUse Dolphin Side-by-Side 3D. "
                "Sunshine streams the SBS image, StreamPotato splits it on "
                "the top screen when the 3D slider is open.",
                config->experimental_stereoscopic_3d);
            if (config->experimental_stereoscopic_3d) {
                config->experimental_better_screen = false;
                config->experimental_stable_stream = false;
                config->experimental_ultra_potato = false;
            }
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
    }

    // Update the config file
    std::string sanitize_message = sanitize_stream_settings(config);
    if (!config_save(STREAMPOTATO_CONFIG_PATH, config)) {
        wait_for_button(
            "Stream settings were updated for this session, but the SD config file could not be written.");
    } else if (!sanitize_message.empty()) {
        wait_for_button(sanitize_message);
    }
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

    SOC_buffer = (u32 *)memalign(SOC_ALIGN, SOC_BUFFERSIZE);
    if (SOC_buffer == NULL) {
        printf("Failed to allocate SOC buffer\n");
        exit(1);
    }
    status = socInit(SOC_buffer, SOC_BUFFERSIZE);
    if (R_FAILED(status)) {
        printf("socInit: %08lX\n", status);
        exit(1);
    }
    g_soc_initialized = true;

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
    int status = run_loading_task("Loading apps",
                                  "Fetching the host application list.",
                                  build_profile_status(),
                                  "Querying the paired host", app_list_task,
                                  &task_context);
    if (status != GS_OK || task_context.list == NULL) {
        wait_for_button("Unable to load the host app list.");
        return -1;
    }

    PAPP_LIST list = task_context.list;
    std::vector<std::string> app_names;
    std::vector<int> app_ids;
    while (list != NULL) {
        printf("%d. %s\n", list->id, safe_cstr(list->name, "Unnamed app"));
        app_names.push_back(safe_cstr(list->name, "Unnamed app"));
        app_ids.push_back(list->id);
        list = list->next;
    }

    if (app_ids.empty()) {
        xml_free_applist(task_context.list);
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
        xml_free_applist(task_context.list);
        return -1;
    }
    int app_id = app_ids[id_idx];
    xml_free_applist(task_context.list);
    return app_id;
}

static inline void dispatch_loop(void *_unused_) {
    auto pDispatcher = MessageDispatcher::get_instance();
    while (aptMainLoop()) {
        auto connection_listener = N3dsConnectionListener::get_instance();
        if (connection_listener == nullptr ||
            connection_listener->is_connection_closed()) {
            break;
        }
        gspWaitForAnyEvent();
        pDispatcher->dispatch_all();
    }
}

static inline void input_loop(void *input_handler_in) {
    N3dsInput *input_handler = static_cast<N3dsInput *>(input_handler_in);
    input_handler->force_touchscreen_menu();
    while (aptMainLoop()) {
        auto connection_listener = N3dsConnectionListener::get_instance();
        if (connection_listener == nullptr ||
            connection_listener->is_connection_closed()) {
            break;
        }
        gspWaitForAnyEvent();
        input_handler->n3dsinput_handle_event();
    }
}

static void join_stream_thread(Thread *thread) {
    if (thread == nullptr || *thread == nullptr) {
        return;
    }

    threadJoin(*thread, ~0ULL);
    threadFree(*thread);
    *thread = nullptr;
}

static inline void stream_loop(PCONFIGURATION config,
                               N3dsConnectionListener *connection_listener,
                               std::shared_ptr<N3dsInput> input_handler) {
    // Spin off worker threads
    size_t stack_size = 0x20000;
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    Thread input_thread = nullptr;
    Thread dispatch_thread = nullptr;

    if (!config->viewonly) {
        input_thread = threadCreate(input_loop, input_handler.get(), stack_size,
                                    priority, -1, false);
    }
    dispatch_thread =
        threadCreate(dispatch_loop, nullptr, stack_size, priority, -1, false);

    if (dispatch_thread == nullptr ||
        (!config->viewonly && input_thread == nullptr)) {
        printf("Failed to create one or more stream worker threads\n");
        connection_listener->stage_failed(STAGE_PLATFORM_INIT,
                                          STREAM_THREAD_INIT_ERROR);
        connection_listener->connection_terminated(STREAM_THREAD_INIT_ERROR);
    }

    // Run the main connection loop
    while (!connection_listener->is_connection_closed() && aptMainLoop()) {
        gspWaitForAnyEvent();
        if (aptShouldClose()) {
            connection_listener->connection_terminated(0);
        }
    }

    join_stream_thread(&input_thread);
    join_stream_thread(&dispatch_thread);
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
                            "Waiting the first frame.");

    AUDIO_RENDERER_CALLBACKS *audio_callbacks =
        config->localaudio ? &audio_callbacks_mock : &audio_callbacks_n3ds;

    PDECODER_RENDERER_CALLBACKS video_callbacks = &decoder_callbacks_mock;
    switch (config->video_decoder) {
    case (VIDEO_DECODER_TYPE::HARDWARE_VIDEO_DECODER):
        if (g_potato.is_potato) {
            printf("[P.S.] Hardware decode is unavailable on Old 3DS/2DS, using StreamPotato software decoder\n");
            video_callbacks = &decoder_callbacks_potato;
        } else {
            video_callbacks = &decoder_callbacks_n3ds_mvd;
        }
        break;
    case (VIDEO_DECODER_TYPE::SOFTWARE_VIDEO_DECODER):
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
    stream_loop(config, connection_listener, input_handler);

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
    const bool quiet = menu_ui_is_active();
    if (!quiet) {
        printf("Connecting to %s:%d...\n", config->address, config->port);
    }
    http_set_timeout_s(4);
    gs_cleanup();
    int status = gs_init(server, config->address, config->port, config->key_dir,
                         0, config->unsupported);
    http_set_timeout_s(60);
    if (status == GS_OUT_OF_MEMORY) {
        if (!quiet) {
            printf("Not enough memory\n");
        }
        return status;
    } else if (status == GS_ERROR) {
        if (!quiet) {
            printf("Gamestream error: %s\n", gs_error);
        }
        return status;
    } else if (status == GS_INVALID) {
        if (!quiet) {
            printf("Invalid data received from server: %s\n", gs_error);
        }
        return status;
    } else if (status == GS_UNSUPPORTED_VERSION) {
        if (!quiet) {
            printf("Unsupported version: %s\n", gs_error);
        }
        return status;
    } else if (status != GS_OK) {
        if (!quiet) {
            printf("Can't connect to server %s:%d\n", config->address,
                   config->port);
        }
        return status;
    }

    if (!quiet) {
        printf("GPU: %s, GFE: %s (%s, %s)\n", safe_cstr(server->gpuType),
               safe_cstr(server->serverInfo.serverInfoGfeVersion),
               safe_cstr(server->gsVersion),
               safe_cstr(server->serverInfo.serverInfoAppVersion));
        printf("Server codec flags: 0x%x\n",
               server->serverInfo.serverCodecModeSupport);
    }

    if (!server->paired && !server->isNvidiaSoftware &&
        (is_confirmed_pair(config->address, config->port) ||
         is_saved_pair_address(config->address, config->port))) {
        server->paired = true;
    }

    if (server->paired) {
        add_pair_address(config->address, config->port);
    } else {
        remove_confirmed_pair(config->address, config->port);
    }
    return 0;
}

static void action_stream(CONFIGURATION *config, SERVER_DATA *server) {
    potato_apply_config(config);
    potato_set_stereoscopic_3d(config->experimental_stereoscopic_3d);
    if (!g_potato.is_potato && config->experimental_stereoscopic_3d) {
        config->stream.width = 800;
        config->stream.height = 240;
        config->stream.fps = std::min(config->stream.fps, 30);
    }

    int appId = prompt_for_app_id(server);
    if (appId == -1) {
        return;
    }

    config->stream.supportedVideoFormats = VIDEO_FORMAT_H264;

    std::shared_ptr<N3dsInput> input_handler = nullptr;
    if (config->viewonly) {
        printf("View-only mode enabled, no input will be sent "
               "to the host computer\n");
    } else {
        input_handler = std::make_shared<N3dsInput>(
            config->stream.width, config->stream.height,
            config->swap_face_buttons, config->swap_triggers_and_shoulders,
            config->use_triggers_for_mouse);
    }
    stream(server, config, appId, input_handler);
}

static void action_pair(CONFIGURATION *config, SERVER_DATA *server) {
    // Keep pairing responsive while still giving enough time to enter the PIN
    http_set_timeout_s(90);

    char pin[5];
    sprintf(pin, "%d%d%d%d", (unsigned)random() % 10, (unsigned)random() % 10,
            (unsigned)random() % 10, (unsigned)random() % 10);
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
        snprintf(buffer, sizeof(buffer),
                 "Failed to pair with host (status %d): %s", status,
                 gs_error != NULL ? gs_error
                                  : "pairing task returned without details");
        wait_for_button(buffer);
    } else {
        server->paired = true;
        add_pair_address(config->address, config->port);
        add_confirmed_pair(config->address, config->port);
        wait_for_button("Host paired successfully.");
    }

    // Revert to default HTTP timeout
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
    if (status != GS_OK) {
        char buffer[256];
        snprintf(buffer, sizeof(buffer), "Failed to unpair from host: %s",
                 gs_error != NULL ? gs_error : "unknown error");
        wait_for_button(buffer);
    } else {
        server->paired = false;
        remove_pair_address(config->address, config->port);
        remove_confirmed_pair(config->address, config->port);
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

    CONFIGURATION config;
    config_parse(argc, argv, &config);
    sanitize_stream_settings(&config);
    if (potato_init()) {
        potato_apply_config(&config);
    }

    while (aptMainLoop()) {
        auto address_string = prompt_for_address();
        if (address_string.empty()) {
            continue;
        }
        if (address_string == POTATOFLUX_DIRECT_ACTION) {
            potato_flux_receive_loop();
            continue;
        }
        // Split address and port (if specified)
        size_t port_delim_pos = address_string.rfind(':');
        if (port_delim_pos != std::string::npos &&
            address_string.find(':') == port_delim_pos) {
            std::string port_string = address_string.substr(port_delim_pos + 1);
            address_string = address_string.substr(0, port_delim_pos);
            if (!try_parse_port(port_string, &config.port)) {
                wait_for_button("Invalid port. Use host or host:1-65535.");
                continue;
            }
        }
        if (address_string.empty()) {
            wait_for_button("Host address cannot be empty.");
            continue;
        }
        config.address = (char *)address_string.c_str();

        SERVER_DATA server = {};
        InitServerTaskContext init_context = {
            .config = &config,
            .server = &server,
        };
        if (menu_ui_is_active()) {
            for (int frame = 0; frame < 10 && aptMainLoop(); frame++) {
                menu_ui_draw_loading("Connecting to host",
                                     "Checking pairing state and host information.",
                                     build_profile_status(&config),
                                     "Connecting to the selected IP/host",
                                     frame);
                gspWaitForVBlank();
            }
        }
        int init_status = init_server_task(&init_context);
        if (init_status != 0) {
            gs_cleanup();
            wait_for_button(build_connect_failure_message(&config, init_status));
            continue;
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
    }
    return 0;
}

int main(int argc, char *argv[]) {
    try {
        main_loop(argc, argv);
    } catch (const std::exception &ex) {
        printf("StreamPotato crashed with the following error: %s\n",
               ex.what());
        return 1;
    } catch (const std::string &ex) {
        printf("StreamPotato crashed with the following error message: %s\n",
               ex.c_str());
        return 1;
    } catch (...) {
        printf("StreamPotato crashed with an unknown error\n");
        return 1;
    }
    return 0;
}
