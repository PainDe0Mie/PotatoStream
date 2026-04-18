// potato_sw_video.cpp
// Pipeline vidéo soft-decode optimisé Old 3DS/2DS
// Basé sur n3ds_video_soft.cpp mais avec :
//   - frame skip intelligent (potato_profile)
//   - stats perf
//   - Y2RU hardware (dispo sur ALL 3DS, pas seulement New)
//   - pas de swscale (pas disponible via ffmpeg.h du projet)

#include "potato_sw_video.h"

#include "ffmpeg.h"

#include "../util.h"

#include <3ds.h>
#include <cstdio>
#include <memory>
#include <stdbool.h>
#include <stdexcept>

#define SLICES_PER_FRAME 1
#define N3DS_BUFFER_FRAMES 1

static std::unique_ptr<PotatoVideoDecoder> instance = nullptr;

// ─── Constructeur ─────────────────────────────────────────────────────────

PotatoVideoDecoder::PotatoVideoDecoder(int videoFormat, int width, int height,
                                       int redrawRate, void *context, int drFlags)
    : VideoDecoderBase(width, height) {
    const int perf_flags = FAST_DECODE |
                           ((g_potato.experimental_stable_stream ||
                             g_potato.experimental_ultra_potato)
                                ? VERY_FAST_DECODE
                                : 0);

    printf("[POTATO] SoftDecoder init %dx%d @ %d fps\n", width, height, redrawRate);

    if (ffmpeg_init(videoFormat, width, height, perf_flags,
                    N3DS_BUFFER_FRAMES,
                    SLICES_PER_FRAME) < 0) {
        fprintf(stderr, "[POTATO] ffmpeg_init failed\n");
        throw std::runtime_error("ffmpeg_init failed");
    }

    ensure_buf_size(&ffmpeg_buffer, &ffmpeg_buffer_size,
                    INITIAL_DECODER_BUFFER_SIZE + AV_INPUT_BUFFER_PADDING_SIZE);

    // Y2RU : hardware YUV->RGB disponible sur TOUTES les 3DS (Old et New)
    if (y2rInit()) {
        fprintf(stderr, "[POTATO] y2rInit failed\n");
        throw std::runtime_error("y2rInit failed");
    }

    Y2RU_ConversionParams y2r_params;
    y2r_params.input_format         = INPUT_YUV420_INDIV_8;
    y2r_params.output_format        = OUTPUT_RGB_16_565;
    y2r_params.rotation             = ROTATION_NONE;
    y2r_params.block_alignment      = BLOCK_LINE;
    y2r_params.input_line_width     = image_width;
    y2r_params.input_lines          = image_height;
    y2r_params.standard_coefficient = COEFFICIENT_ITU_R_BT_709_SCALING;
    y2r_params.alpha                = 0xFF;

    int status = Y2RU_SetConversionParams(&y2r_params);
    if (status) {
        fprintf(stderr, "[POTATO] Y2RU_SetConversionParams failed: %d\n", status);
        throw std::runtime_error("Y2RU_SetConversionParams failed");
    }

    rgb_img_buffer =
        (u8 *)linearAlloc(texture_width * texture_height * pixel_size);
    if (!rgb_img_buffer) {
        fprintf(stderr, "[POTATO] Out of memory for rgb_img_buffer\n");
        throw std::runtime_error("Out of memory");
    }

    printf("[POTATO] init OK — Y2RU ready, buf=%dKB\n",
           (texture_width * texture_height * pixel_size) / 1024);
}

// ─── Destructeur ──────────────────────────────────────────────────────────

PotatoVideoDecoder::~PotatoVideoDecoder() {
    ffmpeg_destroy();
    y2rExit();
    linearFree(rgb_img_buffer);
    potato_print_stats();
    printf("[POTATO] decoder shutdown\n");
}

// ─── Conversion YUV → RGB via Y2RU hardware ───────────────────────────────

int PotatoVideoDecoder::_write_yuv_to_framebuffer(const u8 **source,
                                                   int width, int height,
                                                   int px_size) {
    Handle conv_event;
    int status = 0;
    u64 t_start = svcGetSystemTick();

    status = Y2RU_SetSendingY(source[0], width * height, width, 0);
    if (status) { fprintf(stderr, "[POTATO] Y2RU_SetSendingY failed\n"); goto fail; }

    status = Y2RU_SetSendingU(source[1], width * height / 4, width / 2, 0);
    if (status) { fprintf(stderr, "[POTATO] Y2RU_SetSendingU failed\n"); goto fail; }

    status = Y2RU_SetSendingV(source[2], width * height / 4, width / 2, 0);
    if (status) { fprintf(stderr, "[POTATO] Y2RU_SetSendingV failed\n"); goto fail; }

    status = Y2RU_SetReceiving(
        rgb_img_buffer,
        texture_width * texture_height * px_size,
        width * px_size,
        (texture_width - width) * px_size);
    if (status) { fprintf(stderr, "[POTATO] Y2RU_SetReceiving failed\n"); goto fail; }

    status = Y2RU_StartConversion();
    if (status) { fprintf(stderr, "[POTATO] Y2RU_StartConversion failed\n"); goto fail; }

    status = Y2RU_GetTransferEndEvent(&conv_event);
    if (status) { fprintf(stderr, "[POTATO] Y2RU_GetTransferEndEvent failed\n"); goto fail; }

    svcWaitSynchronization(conv_event, 10000000); // 10ms max
    svcCloseHandle(conv_event);

    {
        u64 elapsed_ticks = svcGetSystemTick() - t_start;
        u64 elapsed_us    = (elapsed_ticks * 1000000ULL) / 268000000ULL;
        potato_record_decode_ticks(elapsed_ticks);
        decode_time_total += elapsed_us;
        decode_count++;

        // Warn si on dépasse le budget (41ms @ 24fps)
        u64 budget_us = 1000000ULL / (u64)(g_potato.fps > 0 ? g_potato.fps : 24);
        if (elapsed_us > budget_us) {
            slow_frame_count++;
            if ((slow_frame_count % 120) == 0) {
                printf("[POTATO] slow frames: %lu, last=%llums, budget=%llums\n",
                       (unsigned long)slow_frame_count,
                       (unsigned long long)elapsed_us / 1000,
                       (unsigned long long)budget_us / 1000);
            }
        }
    }

    renderer_lock.lock();
    renderer->set_perf_decode_ticks(svcGetSystemTick() - t_start);
    renderer->write_px_to_framebuffer(rgb_img_buffer);
    renderer_lock.unlock();

    potato_frame_decoded();
    return DR_OK;

fail:
    potato_frame_skipped();
    return DR_OK;
}

// ─── Submit decode unit (appelé par Moonlight pour chaque frame) ───────────

int PotatoVideoDecoder::submit_decode_unit(PDECODE_UNIT decodeUnit) {

    // Frame skip : si on est en retard, on droppe sans décoder
    if (potato_should_skip_frame()) {
        potato_frame_skipped();
        return DR_OK;
    }

    PLENTRY entry = decodeUnit->bufferList;
    int length = 0;

    ensure_buf_size(&ffmpeg_buffer, &ffmpeg_buffer_size,
                    decodeUnit->fullLength + AV_INPUT_BUFFER_PADDING_SIZE);

    while (entry != NULL) {
        memcpy((u8*)ffmpeg_buffer + length, entry->data, entry->length);
        length += entry->length;
        entry = entry->next;
    }

    if (ffmpeg_decode((unsigned char *)ffmpeg_buffer, length) < 0) {
        potato_frame_skipped();
        return DR_OK;
    }

    AVFrame *frame = ffmpeg_get_frame(false);
    if (!frame) {
        potato_frame_skipped();
        return DR_OK;
    }

    return _write_yuv_to_framebuffer(
        (const u8 **)frame->data, image_width, image_height, pixel_size);
}

// ─── Callbacks C pour Moonlight ───────────────────────────────────────────

static int potato_setup(int videoFormat, int width, int height, int redrawRate,
                        void *context, int drFlags) {
    try {
        instance = std::make_unique<PotatoVideoDecoder>(
            videoFormat, width, height, redrawRate, context, drFlags);
        return 0;
    } catch (const std::exception &e) {
        fprintf(stderr, "[POTATO] init failed: %s\n", e.what());
        return -1;
    }
}

static void potato_cleanup() { instance = nullptr; }

static int potato_submit(PDECODE_UNIT decodeUnit) {
    if (!instance) return DR_OK;
    return instance->submit_decode_unit(decodeUnit);
}

DECODER_RENDERER_CALLBACKS decoder_callbacks_potato = {
    .setup            = potato_setup,
    .cleanup          = potato_cleanup,
    .submitDecodeUnit = potato_submit,
    .capabilities     = CAPABILITY_DIRECT_SUBMIT |
                        CAPABILITY_REFERENCE_FRAME_INVALIDATION_AVC,
};
