#include "potato_profile.h"

#include "../config.hpp"

#include <stdio.h>
#include <string.h>

PotatoProfile g_potato = {0};

#define TICKS_PER_SEC_O3DS 268000000ULL
#define FRAME_BUDGET(fps) (TICKS_PER_SEC_O3DS / (fps))

static void potato_resolve_profile(CONFIGURATION *cfg) {
    if (cfg == NULL) {
        return;
    }

    g_potato.host_audio = cfg->localaudio;
    g_potato.experimental_better_screen = cfg->experimental_better_screen;
    g_potato.experimental_stable_stream = cfg->experimental_stable_stream;
    g_potato.experimental_ultra_potato = cfg->experimental_ultra_potato;
    g_potato.experimental_stereoscopic_3d =
        cfg->experimental_stereoscopic_3d;
    g_potato.render_crop_to_fit = g_potato.experimental_better_screen;
    g_potato.render_linear_filter =
        g_potato.experimental_better_screen &&
        !g_potato.experimental_stable_stream;

    if (g_potato.experimental_stereoscopic_3d) {
        g_potato.width = POTATO_STEREO_WIDTH;
        g_potato.height = POTATO_STEREO_HEIGHT;
        g_potato.fps = POTATO_STEREO_FPS;
        g_potato.bitrate_kbps = POTATO_STEREO_BITRATE_KBPS;
        g_potato.max_consecutive_skips = POTATO_STEREO_FRAME_SKIP;
        g_potato.packet_size = POTATO_STABLE_PACKET_SIZE;
        g_potato.render_crop_to_fit = false;
        g_potato.render_linear_filter = false;
    } else if (g_potato.experimental_ultra_potato) {
        g_potato.width = POTATO_ULTRA_WIDTH;
        g_potato.height = POTATO_ULTRA_HEIGHT;
        g_potato.fps = POTATO_ULTRA_FPS;
        g_potato.bitrate_kbps = POTATO_ULTRA_BITRATE_KBPS;
        g_potato.max_consecutive_skips = POTATO_ULTRA_FRAME_SKIP;
        g_potato.packet_size = POTATO_ULTRA_PACKET_SIZE;
        g_potato.render_crop_to_fit = false;
        g_potato.render_linear_filter = false;
    } else if (g_potato.experimental_better_screen) {
        g_potato.width = POTATO_BETTER_WIDTH;
        g_potato.height = POTATO_BETTER_HEIGHT;
        g_potato.fps = POTATO_BETTER_FPS;
        g_potato.bitrate_kbps = POTATO_BETTER_BITRATE_KBPS;
    } else {
        g_potato.width = POTATO_WIDTH;
        g_potato.height = POTATO_HEIGHT;
        g_potato.fps = POTATO_FPS;
        g_potato.bitrate_kbps = POTATO_BITRATE_KBPS;
    }

    if (g_potato.experimental_stereoscopic_3d) {
        // SBS 3D owns the stream size; other experimental profiles are disabled.
    } else if (g_potato.experimental_ultra_potato) {
        // Ultra mode owns the base stream profile and runtime fallback.
    } else if (g_potato.experimental_stable_stream) {
        g_potato.fps = POTATO_STABLE_FPS;
        g_potato.bitrate_kbps = g_potato.experimental_better_screen
                                    ? POTATO_STABLE_BETTER_BITRATE_KBPS
                                    : POTATO_STABLE_BITRATE_KBPS;
        g_potato.max_consecutive_skips = POTATO_STABLE_FRAME_SKIP;
        g_potato.packet_size = POTATO_STABLE_PACKET_SIZE;
    } else {
        g_potato.max_consecutive_skips = POTATO_FRAME_SKIP;
        g_potato.packet_size = POTATO_PACKET_SIZE;
    }

    g_potato.frame_budget_ticks = FRAME_BUDGET(g_potato.fps);
}

bool potato_init(void) {
    memset(&g_potato, 0, sizeof(g_potato));

    Result rc = cfguInit();
    if (R_FAILED(rc)) {
        printf("[P.S.] cfguInit failed (0x%08lX), assuming Old 3DS\n", rc);
        g_potato.model = CFG_MODEL_2DS;
    } else {
        rc = CFGU_GetSystemModel(&g_potato.model);
        cfguExit();
        if (R_FAILED(rc)) {
            printf("[P.S.] CFGU_GetSystemModel failed (0x%08lX), assuming Old 3DS\n",
                   rc);
            g_potato.model = CFG_MODEL_2DS;
        }
    }

    switch (g_potato.model) {
    case CFG_MODEL_3DS:
    case CFG_MODEL_3DSXL:
    case CFG_MODEL_2DS:
        g_potato.is_potato = true;
        break;
    default:
        g_potato.is_potato = false;
        break;
    }

    if (!g_potato.is_potato) {
        printf("[P.S.] New 3DS detected (model=%d), keeping normal mode\n",
               g_potato.model);
        return false;
    }

    g_potato.width = POTATO_WIDTH;
    g_potato.height = POTATO_HEIGHT;
    g_potato.fps = POTATO_FPS;
    g_potato.bitrate_kbps = POTATO_BITRATE_KBPS;
    g_potato.hw_decode = false;
    g_potato.frame_skip_enabled = true;
    g_potato.experimental_better_screen = false;
    g_potato.experimental_stable_stream = false;
    g_potato.experimental_ultra_potato = false;
    g_potato.experimental_stereoscopic_3d = false;
    g_potato.dynamic_ultra_active = false;
    g_potato.render_crop_to_fit = false;
    g_potato.render_linear_filter = false;
    g_potato.host_audio = true;
    g_potato.max_consecutive_skips = POTATO_FRAME_SKIP;
    g_potato.audio_buf_ms = POTATO_AUDIO_BUF_MS;
    g_potato.packet_size = POTATO_PACKET_SIZE;
    g_potato.frame_budget_ticks = FRAME_BUDGET(POTATO_FPS);
    g_potato.ultra_pressure = 0;

    const char *model_name = "Unknown";
    switch (g_potato.model) {
    case CFG_MODEL_3DS:
        model_name = "Old 3DS";
        break;
    case CFG_MODEL_3DSXL:
        model_name = "Old 3DS XL";
        break;
    case CFG_MODEL_2DS:
        model_name = "Old 2DS";
        break;
    }

    printf("[P.S.] StreamPotato mode enabled\n");
    printf("[P.S.] Model   : %s\n", model_name);
    printf("[P.S.] Stream  : %dx%d @ %d fps\n", g_potato.width,
           g_potato.height, g_potato.fps);
    printf("[P.S.] Bitrate : %d Kbps\n", g_potato.bitrate_kbps);
    printf("[P.S.] Audio   : host playback only\n");
    printf("[P.S.] Budget  : %llu ticks/frame\n",
           g_potato.frame_budget_ticks);

    return true;
}

bool potato_should_skip_frame(void) {
    if (!g_potato.is_potato || !g_potato.frame_skip_enabled) {
        return false;
    }

    if (g_potato.last_frame_tick == 0) {
        return false;
    }

    if (g_potato.consecutive_skips >=
        (uint32_t)g_potato.max_consecutive_skips) {
        return false;
    }

    uint64_t now = svcGetSystemTick();
    uint64_t elapsed = now - g_potato.last_frame_tick;
    uint64_t grace_ticks = g_potato.experimental_stable_stream
                               ? (g_potato.frame_budget_ticks / 8)
                               : (g_potato.frame_budget_ticks / 2);
    if (g_potato.experimental_ultra_potato) {
        grace_ticks = g_potato.dynamic_ultra_active
                          ? (g_potato.frame_budget_ticks / 16)
                          : (g_potato.frame_budget_ticks / 6);
    }
    if (elapsed > g_potato.frame_budget_ticks + grace_ticks) {
        return true;
    }

    if (g_potato.experimental_stable_stream && g_potato.decode_avg_ticks > 0) {
        const uint64_t avg_threshold = (g_potato.frame_budget_ticks * 3) / 4;
        const uint64_t last_threshold = (g_potato.frame_budget_ticks * 9) / 10;
        if (g_potato.decode_avg_ticks >= avg_threshold &&
            elapsed > (g_potato.frame_budget_ticks / 3)) {
            return true;
        }
        if (g_potato.last_decode_ticks >= last_threshold &&
            elapsed > (g_potato.frame_budget_ticks / 4)) {
            return true;
        }
    }
    if (g_potato.experimental_ultra_potato && g_potato.dynamic_ultra_active &&
        elapsed > (g_potato.frame_budget_ticks / 5)) {
        return true;
    }
    return false;
}

void potato_frame_decoded(void) {
    g_potato.frames_decoded++;
    g_potato.consecutive_skips = 0;
    g_potato.last_frame_tick = svcGetSystemTick();
}

void potato_frame_skipped(void) {
    g_potato.frames_skipped++;
    g_potato.consecutive_skips++;
}

void potato_record_decode_ticks(uint64_t decode_ticks) {
    g_potato.last_decode_ticks = decode_ticks;
    if (g_potato.decode_avg_ticks == 0) {
        g_potato.decode_avg_ticks = decode_ticks;
    } else {
        g_potato.decode_avg_ticks =
            ((g_potato.decode_avg_ticks * 7) + decode_ticks) / 8;
    }

    if (!g_potato.experimental_ultra_potato) {
        return;
    }

    const bool under_pressure =
        decode_ticks >= ((g_potato.frame_budget_ticks * 7) / 10) ||
        g_potato.decode_avg_ticks >= ((g_potato.frame_budget_ticks * 5) / 8);

    if (under_pressure) {
        if (g_potato.ultra_pressure < 30) {
            g_potato.ultra_pressure += 2;
        }
    } else if (g_potato.ultra_pressure > 0) {
        g_potato.ultra_pressure--;
    }

    if (!g_potato.dynamic_ultra_active && g_potato.ultra_pressure >= 8) {
        g_potato.dynamic_ultra_active = true;
        printf("[P.S.] ultra runtime mode engaged\n");
    } else if (g_potato.dynamic_ultra_active && g_potato.ultra_pressure <= 2) {
        g_potato.dynamic_ultra_active = false;
        printf("[P.S.] ultra runtime mode recovered\n");
    }
}

void potato_print_stats(void) {
    if (!g_potato.is_potato) {
        return;
    }

    uint32_t total = g_potato.frames_decoded + g_potato.frames_skipped;
    if (total == 0) {
        return;
    }

    uint32_t skip_pct = (g_potato.frames_skipped * 100) / total;
    printf("[P.S.] Stats: %lu decoded, %lu skipped (%lu%%)\n",
           (unsigned long)g_potato.frames_decoded,
           (unsigned long)g_potato.frames_skipped,
           (unsigned long)skip_pct);
}

void potato_apply_config(void *moonlight_config) {
    if (!g_potato.is_potato || moonlight_config == NULL) {
        return;
    }

    CONFIGURATION *cfg = static_cast<CONFIGURATION *>(moonlight_config);
    potato_resolve_profile(cfg);
    cfg->stream.width = g_potato.width;
    cfg->stream.height = g_potato.height;
    cfg->stream.fps = g_potato.fps;
    cfg->stream.bitrate = g_potato.bitrate_kbps;
    cfg->stream.packetSize = g_potato.packet_size;
    cfg->stream.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    cfg->video_decoder = VIDEO_DECODER_TYPE::SOFTWARE_VIDEO_DECODER;
    cfg->localaudio = g_potato.host_audio;
    cfg->motion_controls = false;
}

void potato_set_stereoscopic_3d(bool enabled) {
    g_potato.experimental_stereoscopic_3d = enabled;
}
