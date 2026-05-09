#include "potato_flux_client.h"

#include "../input/touch/TouchHandler.hpp"
#include "../menu_ui.hpp"

#include <3ds.h>

#include <arpa/inet.h>
#include <algorithm>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr uint8_t POTATOFLUX_VERSION = 1;
constexpr size_t POTATOFLUX_MAX_PACKET = 1400;
constexpr uint32_t POTATOFLUX_MAX_FRAME_BYTES = 400 * 240 * 2;
constexpr uint16_t POTATOFLUX_MAX_PACKETS = 512;
constexpr uint16_t POTATOFLUX_DISCOVERY_PORT = 47999;
constexpr uint8_t POTATOFLUX_FLAG_KEYFRAME = 1 << 0;
constexpr uint8_t POTATOFLUX_FLAG_NATIVE_FRAMEBUFFER = 1 << 1;
constexpr uint8_t POTATOFLUX_FLAG_RUNLIST = 1 << 2;
constexpr uint16_t POTATOFLUX_RUNLIST_FILL_BIT = 0x8000;
constexpr uint16_t POTATOFLUX_RUNLIST_LEN_MASK = 0x7fff;
constexpr uint16_t POTATOFLUX_NATIVE_WIDTH = 400;
constexpr uint16_t POTATOFLUX_NATIVE_HEIGHT = 240;
constexpr uint32_t POTATOFLUX_NATIVE_FRAME_BYTES =
    POTATOFLUX_NATIVE_WIDTH * POTATOFLUX_NATIVE_HEIGHT * 2;
constexpr uint64_t TICKS_PER_SECOND = 268000000ULL;
constexpr int POTATOFLUX_PACKET_BUDGET = 160;

#pragma pack(push, 1)
struct PotatoFluxHeader {
    char magic[4];
    uint8_t version;
    uint8_t flags;
    uint16_t header_size;
    uint32_t frame_id;
    uint32_t timestamp_ms;
    uint16_t width;
    uint16_t height;
    uint32_t payload_offset;
    uint16_t payload_len;
    uint16_t packet_index;
    uint16_t packet_count;
    uint16_t reserved;
};
#pragma pack(pop)

static_assert(sizeof(PotatoFluxHeader) == 32,
              "PotatoFlux packet header must stay 32 bytes");

struct PotatoFluxFrame {
    uint32_t frame_id = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t packet_count = 0;
    uint16_t packets_received = 0;
    bool keyframe = false;
    bool native_framebuffer = false;
    uint8_t *staging_pixels = nullptr;
    uint8_t *completed_pixels = nullptr;
    uint32_t pixel_capacity = 0;
    std::vector<uint8_t> received;

    ~PotatoFluxFrame() {
        if (staging_pixels != nullptr) {
            linearFree(staging_pixels);
        }
        if (completed_pixels != nullptr) {
            linearFree(completed_pixels);
        }
    }
};

bool parse_header(const uint8_t *packet, ssize_t packet_len,
                  PotatoFluxHeader *header) {
    if (packet == nullptr || header == nullptr ||
        packet_len < (ssize_t)sizeof(PotatoFluxHeader)) {
        return false;
    }

    memcpy(header, packet, sizeof(PotatoFluxHeader));
    if (memcmp(header->magic, "PFLX", 4) != 0 ||
        header->version != POTATOFLUX_VERSION ||
        header->header_size < sizeof(PotatoFluxHeader)) {
        return false;
    }

    if (header->width == 0 || header->height == 0 ||
        header->packet_count == 0 ||
        header->packet_count > POTATOFLUX_MAX_PACKETS ||
        header->packet_index >= header->packet_count ||
        header->payload_len == 0) {
        return false;
    }

    const uint32_t frame_size = header->width * header->height * 2;
    if (frame_size > POTATOFLUX_MAX_FRAME_BYTES ||
        header->payload_offset > frame_size ||
        header->payload_len > frame_size - header->payload_offset) {
        return false;
    }

    if ((header->flags & POTATOFLUX_FLAG_NATIVE_FRAMEBUFFER) != 0 &&
        (header->width != POTATOFLUX_NATIVE_WIDTH ||
         header->height != POTATOFLUX_NATIVE_HEIGHT)) {
        return false;
    }

    if (packet_len < (ssize_t)header->header_size + header->payload_len) {
        return false;
    }

    return true;
}

void maybe_send_discovery_beacon(int fd, uint64_t *last_beacon_tick) {
    if (fd < 0 || last_beacon_tick == nullptr) {
        return;
    }

    const uint64_t now = svcGetSystemTick();
    if (*last_beacon_tick != 0 &&
        now - *last_beacon_tick < TICKS_PER_SECOND) {
        return;
    }
    *last_beacon_tick = now;

    const char beacon[] = "PFLXDS:1:47998:StreamPotato";
    sockaddr_in broadcast = {};
    broadcast.sin_family = AF_INET;
    broadcast.sin_port = htons(POTATOFLUX_DISCOVERY_PORT);
    broadcast.sin_addr.s_addr = INADDR_BROADCAST;
    sendto(fd, beacon, sizeof(beacon) - 1, 0, (sockaddr *)&broadcast,
           sizeof(broadcast));
}

std::string local_ip_hint() {
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        return "unknown";
    }

    sockaddr_in remote = {};
    remote.sin_family = AF_INET;
    remote.sin_port = htons(9);
    remote.sin_addr.s_addr = inet_addr("8.8.8.8");
    connect(fd, (sockaddr *)&remote, sizeof(remote));

    sockaddr_in local = {};
    socklen_t local_len = sizeof(local);
    std::string result = "unknown";
    if (getsockname(fd, (sockaddr *)&local, &local_len) == 0) {
        result = inet_ntoa(local.sin_addr);
    }

    close(fd);
    return result;
}

void prepare_direct_display() {
    gfxSetScreenFormat(GFX_TOP, GSP_RGB565_OES);
    gfxSetScreenFormat(GFX_BOTTOM, GSP_RGB565_OES);
    gfxSetDoubleBuffering(GFX_TOP, false);
    gfxSetDoubleBuffering(GFX_BOTTOM, false);
    gfxSet3D(false);
    gfxSetWide(false);
    consoleInit(GFX_TOP, &DebugTouchHandler::topScreen);
    consoleInit(GFX_BOTTOM, &DebugTouchHandler::bottomScreen);
}

void draw_bottom_status(uint32_t frames, uint32_t dropped,
                        uint32_t incomplete, uint32_t packets, uint16_t width,
                        uint16_t height, const std::string &ip_hint,
                        const std::string &last_sender) {
    consoleSelect(&DebugTouchHandler::bottomScreen);
    consoleClear();
    printf("PotatoFlux direct\n\n");
    printf("3DS target: %s:%d\n", ip_hint.c_str(), POTATOFLUX_PORT);
    printf("Frames: %lu\nDropped: %lu\nSkipped: %lu\n",
           (unsigned long)frames, (unsigned long)dropped,
           (unsigned long)incomplete);
    printf("Packets: %lu\nFrom: %s\n", (unsigned long)packets,
           last_sender.c_str());
    if (width != 0 && height != 0) {
        printf("Input: %ux%u RGB565\n", width, height);
    }
    printf("\nB or START: stop\n");

    gfxFlushBuffers();
    gfxScreenSwapBuffers(GFX_BOTTOM, false);
}

void draw_waiting_screen(uint32_t frames, uint32_t dropped,
                         uint32_t incomplete, uint32_t packets,
                         const std::string &ip_hint,
                         const std::string &last_sender) {
    consoleSelect(&DebugTouchHandler::topScreen);
    consoleClear();
    printf("POTATOFLUX DIRECT\n\n");
    printf("Listening for raw low-latency frames.\n\n");
    printf("Start PotatoFlux on the PC:\n");
    printf("potatoflux.exe --target %s:%d\n", ip_hint.c_str(),
           POTATOFLUX_PORT);

    gfxFlushBuffers();
    gfxScreenSwapBuffers(GFX_TOP, false);
    draw_bottom_status(frames, dropped, incomplete, packets, 0, 0, ip_hint,
                       last_sender);
}

void draw_rgb565_to_top(const uint8_t *pixels, uint16_t width, uint16_t height,
                        bool native_framebuffer) {
    if (pixels == nullptr || width == 0 || height == 0) {
        return;
    }

    u16 *fb = reinterpret_cast<u16 *>(gfxGetFramebuffer(GFX_TOP, GFX_LEFT,
                                                        nullptr, nullptr));
    if (fb == nullptr) {
        return;
    }

    if (native_framebuffer && width == POTATOFLUX_NATIVE_WIDTH &&
        height == POTATOFLUX_NATIVE_HEIGHT) {
        memcpy(fb, pixels, POTATOFLUX_NATIVE_FRAME_BYTES);
        gfxFlushBuffers();
        gfxScreenSwapBuffers(GFX_TOP, false);
        return;
    }

    const int screen_w = 400;
    const int screen_h = 240;
    const int out_w =
        std::max(1, std::min(screen_w, (int)width * screen_h / height));
    const int out_h =
        std::max(1, std::min(screen_h, (int)height * screen_w / width));
    const int off_x = (screen_w - out_w) / 2;
    const int off_y = (screen_h - out_h) / 2;
    memset(fb, 0, screen_w * screen_h * sizeof(u16));

    for (int y = 0; y < out_h; y++) {
        const int src_y = y * height / out_h;
        const uint16_t *src_row =
            reinterpret_cast<const uint16_t *>(pixels + src_y * width * 2);
        const int dst_y = off_y + y;
        for (int x = 0; x < out_w; x++) {
            const int src_x = x * width / out_w;
            const int dst_x = off_x + x;
            fb[(screen_h - 1 - dst_y) + dst_x * screen_h] = src_row[src_x];
        }
    }

    gfxFlushBuffers();
    gfxScreenSwapBuffers(GFX_TOP, false);
}

bool apply_runlist_payload(uint8_t *pixels, uint32_t capacity,
                           const uint8_t *payload, uint16_t payload_len) {
    uint32_t pos = 0;
    while (pos < payload_len) {
        if (payload_len - pos < 6) {
            return false;
        }

        const uint32_t offset = (uint32_t)payload[pos] |
                                ((uint32_t)payload[pos + 1] << 8) |
                                ((uint32_t)payload[pos + 2] << 16) |
                                ((uint32_t)payload[pos + 3] << 24);
        const uint16_t len_flags =
            (uint16_t)payload[pos + 4] | ((uint16_t)payload[pos + 5] << 8);
        const bool fill_run = (len_flags & POTATOFLUX_RUNLIST_FILL_BIT) != 0;
        const uint16_t len = len_flags & POTATOFLUX_RUNLIST_LEN_MASK;
        pos += 6;

        if (len == 0 || offset > capacity || len > capacity - offset ||
            (fill_run && ((len & 1) != 0 || payload_len - pos < 2)) ||
            (!fill_run && payload_len - pos < len)) {
            return false;
        }

        if (fill_run) {
            const uint8_t lo = payload[pos];
            const uint8_t hi = payload[pos + 1];
            for (uint32_t i = 0; i < len; i += 2) {
                pixels[offset + i] = lo;
                pixels[offset + i + 1] = hi;
            }
            pos += 2;
        } else {
            memcpy(pixels + offset, payload + pos, len);
            pos += len;
        }
    }

    return true;
}

bool begin_frame(PotatoFluxFrame *frame, const PotatoFluxHeader &header,
                 bool has_completed_frame) {
    const uint32_t frame_size = header.width * header.height * 2;
    const bool keyframe = (header.flags & POTATOFLUX_FLAG_KEYFRAME) != 0;
    const bool native_framebuffer =
        (header.flags & POTATOFLUX_FLAG_NATIVE_FRAMEBUFFER) != 0;
    if (!keyframe && !has_completed_frame) {
        return false;
    }

    try {
        const bool size_changed =
            frame->width != header.width || frame->height != header.height ||
            frame->pixel_capacity != frame_size;
        if (size_changed && !keyframe) {
            return false;
        }
        if (size_changed) {
            if (frame->staging_pixels != nullptr) {
                linearFree(frame->staging_pixels);
                frame->staging_pixels = nullptr;
            }
            if (frame->completed_pixels != nullptr) {
                linearFree(frame->completed_pixels);
                frame->completed_pixels = nullptr;
            }
            frame->staging_pixels =
                static_cast<uint8_t *>(linearAlloc(frame_size));
            frame->completed_pixels =
                static_cast<uint8_t *>(linearAlloc(frame_size));
            frame->pixel_capacity =
                frame->staging_pixels != nullptr &&
                        frame->completed_pixels != nullptr
                    ? frame_size
                    : 0;
            if (frame->staging_pixels == nullptr ||
                frame->completed_pixels == nullptr) {
                return false;
            }
        }

        if (keyframe || size_changed) {
            memset(frame->staging_pixels, 0, frame_size);
        } else {
            memcpy(frame->staging_pixels, frame->completed_pixels, frame_size);
        }
        frame->frame_id = header.frame_id;
        frame->width = header.width;
        frame->height = header.height;
        frame->packet_count = header.packet_count;
        frame->packets_received = 0;
        frame->keyframe = keyframe;
        frame->native_framebuffer = native_framebuffer;
        frame->received.assign(header.packet_count, 0);
    } catch (...) {
        return false;
    }
    return true;
}

bool frame_id_is_newer(uint32_t candidate, uint32_t reference) {
    return candidate != reference &&
           (int32_t)(candidate - reference) > 0;
}

} // namespace

void potato_flux_receive_loop(void) {
    const bool had_menu_ui = menu_ui_is_active();
    if (had_menu_ui) {
        menu_ui_shutdown();
    }
    prepare_direct_display();

    const std::string ip_hint = local_ip_hint();
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        printf("PotatoFlux socket failed: %d\n", errno);
        if (had_menu_ui) {
            menu_ui_init();
        }
        return;
    }

    sockaddr_in bind_addr = {};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons(POTATOFLUX_PORT);
    if (bind(fd, (sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
        printf("PotatoFlux bind failed: %d\n", errno);
        close(fd);
        if (had_menu_ui) {
            menu_ui_init();
        }
        return;
    }

    int broadcast_enabled = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &broadcast_enabled,
               sizeof(broadcast_enabled));
    int receive_buffer_size = 256 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer_size,
               sizeof(receive_buffer_size));
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

    PotatoFluxFrame frame;
    uint8_t packet[POTATOFLUX_MAX_PACKET];
    uint32_t frames_rendered = 0;
    uint32_t dropped_packets = 0;
    uint32_t incomplete_frames = 0;
    uint32_t packets_seen = 0;
    uint32_t idle_ticks = 0;
    uint32_t bottom_status_ticks = 0;
    uint16_t stream_width = 0;
    uint16_t stream_height = 0;
    std::string last_sender = "none";
    uint64_t last_beacon_tick = 0;
    uint32_t last_completed_frame_id = 0;
    bool has_completed_frame = false;

    auto reset_frame = [&]() {
        frame.packet_count = 0;
        frame.packets_received = 0;
        frame.keyframe = false;
        frame.native_framebuffer = false;
        frame.received.clear();
    };

    auto drop_incomplete_frame = [&]() {
        if (frame.packet_count != 0 &&
            frame.packets_received < frame.packet_count) {
            incomplete_frames++;
        }
        reset_frame();
    };

    auto publish_frame = [&]() {
        if (frame.packets_received != frame.packet_count ||
            frame.staging_pixels == nullptr ||
            frame.completed_pixels == nullptr) {
            drop_incomplete_frame();
            return;
        }
        std::swap(frame.completed_pixels, frame.staging_pixels);
        draw_rgb565_to_top(frame.completed_pixels, frame.width, frame.height,
                           frame.native_framebuffer);
        frames_rendered++;
        last_completed_frame_id = frame.frame_id;
        has_completed_frame = true;
        stream_width = frame.width;
        stream_height = frame.height;
        reset_frame();
        if ((frames_rendered % 30) == 1) {
            draw_bottom_status(frames_rendered, dropped_packets,
                               incomplete_frames, packets_seen, stream_width,
                               stream_height, ip_hint, last_sender);
        }
    };

    auto discard_current_frame = [&]() {
        if (frame.packet_count == 0) {
            reset_frame();
            return;
        }
        drop_incomplete_frame();
    };

    draw_waiting_screen(frames_rendered, dropped_packets, incomplete_frames,
                        packets_seen, ip_hint, last_sender);

    while (aptMainLoop()) {
        maybe_send_discovery_beacon(fd, &last_beacon_tick);

        hidScanInput();
        const u32 keys = hidKeysDown();
        if ((keys & KEY_B) || (keys & KEY_START)) {
            break;
        }

        bool received_any = false;
        for (int budget = 0; budget < POTATOFLUX_PACKET_BUDGET; budget++) {
            sockaddr_in from = {};
            socklen_t from_len = sizeof(from);
            ssize_t received_len =
                recvfrom(fd, packet, sizeof(packet), 0, (sockaddr *)&from,
                         &from_len);

            if (received_len < 0) {
                break;
            }

            received_any = true;
            packets_seen++;
            last_sender = inet_ntoa(from.sin_addr);

            PotatoFluxHeader header = {};
            if (!parse_header(packet, received_len, &header)) {
                dropped_packets++;
                continue;
            }

            if (has_completed_frame &&
                (header.frame_id == last_completed_frame_id ||
                 !frame_id_is_newer(header.frame_id,
                                    last_completed_frame_id))) {
                continue;
            }

            if (frame.packet_count != 0 && header.frame_id != frame.frame_id &&
                !frame_id_is_newer(header.frame_id, frame.frame_id)) {
                continue;
            }

            const bool packet_is_keyframe =
                (header.flags & POTATOFLUX_FLAG_KEYFRAME) != 0;

            if (frame.packet_count != 0 && header.frame_id != frame.frame_id &&
                frame_id_is_newer(header.frame_id, frame.frame_id) &&
                frame.packets_received < frame.packet_count) {
                discard_current_frame();
            }

            if (!has_completed_frame && !packet_is_keyframe) {
                if (frame.packet_count != 0 &&
                    header.frame_id != frame.frame_id &&
                    frame.packets_received < frame.packet_count) {
                    discard_current_frame();
                } else {
                    reset_frame();
                }
                continue;
            }

            if (frame.frame_id != header.frame_id ||
                frame.width != header.width || frame.height != header.height ||
                frame.packet_count != header.packet_count) {
                if (!begin_frame(&frame, header, has_completed_frame)) {
                    dropped_packets++;
                    continue;
                }
            }

            if (frame.received[header.packet_index] == 0) {
                const uint8_t *payload = packet + header.header_size;
                const bool packet_is_runlist =
                    (header.flags & POTATOFLUX_FLAG_RUNLIST) != 0;
                const bool applied =
                    packet_is_runlist
                        ? apply_runlist_payload(frame.staging_pixels,
                                                frame.pixel_capacity, payload,
                                                header.payload_len)
                        : (memcpy(frame.staging_pixels + header.payload_offset,
                                  payload, header.payload_len),
                           true);
                if (!applied) {
                    dropped_packets++;
                    continue;
                }
                frame.received[header.packet_index] = 1;
                frame.packets_received++;
            }

            if (frame.packets_received == frame.packet_count) {
                publish_frame();
            }
        }

        if (received_any) {
            idle_ticks = 0;
        } else {
            idle_ticks++;
        }
        bottom_status_ticks++;
        if (!received_any && frames_rendered == 0 && (idle_ticks % 120) == 0) {
            draw_waiting_screen(frames_rendered, dropped_packets,
                                incomplete_frames, packets_seen, ip_hint,
                                last_sender);
        } else if (bottom_status_ticks % 60 == 0) {
            draw_bottom_status(frames_rendered, dropped_packets,
                               incomplete_frames, packets_seen, stream_width,
                               stream_height, ip_hint, last_sender);
        }
        gspWaitForVBlank();
    }

    close(fd);
    if (had_menu_ui) {
        menu_ui_init();
    }
}
