#pragma once

#include <3ds.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define POTATO_WIDTH 320
#define POTATO_HEIGHT 180
#define POTATO_FPS 20
#define POTATO_BITRATE_KBPS 1500
#define POTATO_BETTER_WIDTH 400
#define POTATO_BETTER_HEIGHT 225
#define POTATO_BETTER_FPS 18
#define POTATO_BETTER_BITRATE_KBPS 2200
#define POTATO_STABLE_FPS 15
#define POTATO_STABLE_BITRATE_KBPS 1100
#define POTATO_STABLE_BETTER_BITRATE_KBPS 1600
#define POTATO_ULTRA_WIDTH 320
#define POTATO_ULTRA_HEIGHT 180
#define POTATO_ULTRA_FPS 12
#define POTATO_ULTRA_BITRATE_KBPS 750
#define POTATO_STEREO_WIDTH 640
#define POTATO_STEREO_HEIGHT 180
#define POTATO_STEREO_FPS 12
#define POTATO_STEREO_BITRATE_KBPS 1200
#define POTATO_AUDIO_BUF_MS 60
#define POTATO_FRAME_SKIP 2
#define POTATO_STABLE_FRAME_SKIP 6
#define POTATO_PACKET_SIZE 1024
#define POTATO_STABLE_PACKET_SIZE 960
#define POTATO_ULTRA_FRAME_SKIP 10
#define POTATO_STEREO_FRAME_SKIP 8
#define POTATO_ULTRA_PACKET_SIZE 896

typedef struct {
    bool is_potato;
    u8 model;
    int width;
    int height;
    int fps;
    int bitrate_kbps;
    bool hw_decode;
    bool frame_skip_enabled;
    bool experimental_better_screen;
    bool experimental_stable_stream;
    bool experimental_ultra_potato;
    bool experimental_stereoscopic_3d;
    bool dynamic_ultra_active;
    bool render_crop_to_fit;
    bool render_linear_filter;
    bool host_audio;
    int max_consecutive_skips;
    int audio_buf_ms;
    int packet_size;
    uint64_t last_decode_ticks;
    uint64_t decode_avg_ticks;
    uint32_t ultra_pressure;
    uint32_t frames_decoded;
    uint32_t frames_skipped;
    uint32_t consecutive_skips;
    uint64_t last_frame_tick;
    uint64_t frame_budget_ticks;
} PotatoProfile;

extern PotatoProfile g_potato;

bool potato_init(void);
bool potato_should_skip_frame(void);
void potato_frame_decoded(void);
void potato_frame_skipped(void);
void potato_print_stats(void);
void potato_apply_config(void *moonlight_config);
void potato_record_decode_ticks(uint64_t decode_ticks);
void potato_set_stereoscopic_3d(bool enabled);

#ifdef __cplusplus
}
#endif
