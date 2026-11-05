#include "potato_sw_video.h"

#include "ffmpeg.h"

#include "../potato/potato_bench.h"

#include "../util.h"

#include <3ds.h>
#include <cstdio>
#include <memory>
#include <stdbool.h>
#include <stdexcept>

#define SLICES_PER_FRAME 1
#define N3DS_BUFFER_FRAMES 1

#ifndef POTATO_ASYNC_PRESENT
#define POTATO_ASYNC_PRESENT 1
#endif

static std::unique_ptr<PotatoVideoDecoder> instance = nullptr;

bool g_potato_video_2nd_core = false;
static u32 saved_core1_limit = 0;

int potato_decoder_perf_flags() {
    return FAST_DECODE | ((g_potato.experimental_stable_stream ||
                           g_potato.experimental_ultra_potato)
                              ? VERY_FAST_DECODE
                              : 0);
}

PotatoVideoDecoder::PotatoVideoDecoder(int videoFormat, int width, int height, int redrawRate, void *context, int drFlags)
    : VideoDecoderBase(width, height, redrawRate) {
    int perf_flags = potato_decoder_perf_flags();
    
    if (g_potato_video_2nd_core) {
        // core 1 runs at most this share of the time for the app, the slices sent there need much more
        saved_core1_limit = 0;
        APT_GetAppCpuTimeLimit(&saved_core1_limit);
        const Result res = APT_SetAppCpuTimeLimit(80);
        printf("[POTATO] video on core 1, time limit 80%%: %s\n",
               R_SUCCEEDED(res) ? "ok" : "refused");
        perf_flags |= SLICE_CORE1;
    }

    printf("[POTATO] SoftDecoder init %dx%d @ %d fps\n", width, height, redrawRate);

    if (ffmpeg_init(videoFormat, width, height, perf_flags,
                    N3DS_BUFFER_FRAMES,
                    SLICES_PER_FRAME) < 0) {
        fprintf(stderr, "[POTATO] ffmpeg_init failed\n");
        throw std::runtime_error("ffmpeg_init failed");
    }

    ensure_buf_size(&ffmpeg_buffer, &ffmpeg_buffer_size,
                    INITIAL_DECODER_BUFFER_SIZE + AV_INPUT_BUFFER_PADDING_SIZE);

    // Y2RU: hardware YUV->RGB
    y2r = std::make_unique<Y2rConverter>(image_width, image_height,
                                         texture_width, texture_height,
                                         pixel_size);

    rgb_img_buffer = (u8 *)linearAlloc(texture_width * texture_height * pixel_size);
    if (!rgb_img_buffer) {
        fprintf(stderr, "[POTATO] Out of memory for rgb_img_buffer\n");
        throw std::runtime_error("Out of memory");
    }

    LightEvent_Init(&job_ready, RESET_ONESHOT);
    LightEvent_Init(&presenter_idle, RESET_STICKY);
    LightEvent_Signal(&presenter_idle);

    // same priority as main, the decoder drops one step on its first frame.
    svcGetThreadPriority(&presenter_priority, CUR_THREAD_HANDLE);
    if (presenter_priority >= 0x3F) {
        presenter_priority = 0x3E;
    }
    if (POTATO_ASYNC_PRESENT) {
        presenter = threadCreate(_presenter_entry, this, 0x10000,
                                 presenter_priority, -1, false);
    }
    async_present = presenter != nullptr;

    potato_bench_record_start(videoFormat, width, height, redrawRate);

    printf("[POTATO] init OK — Y2RU ready (%s, %s), buf=%dKB\n",
           y2r->can_tile() ? "tiled" : "linear",
           presenter ? "async" : "sync",
           (texture_width * texture_height * pixel_size) / 1024);
}

PotatoVideoDecoder::~PotatoVideoDecoder() {
    printf("[POTATO] stopping decoder\n");
    if (presenter) {
        presenter_stop = true;
        LightEvent_Signal(&job_ready);
        while (threadJoin(presenter, 1000000000ULL) != 0) {
            printf("[POTATO] waiting for the presenter to stop\n");
        }
        threadFree(presenter);
        presenter = nullptr;
    }
    potato_bench_record_stop();
    ffmpeg_destroy();
    if (saved_core1_limit != 0) {
        APT_SetAppCpuTimeLimit(saved_core1_limit);
        saved_core1_limit = 0;
    }
    y2r = nullptr;
    linearFree(rgb_img_buffer);
    potato_print_stats();
    printf("[POTATO] decoder shutdown\n");
}

void PotatoVideoDecoder::_presenter_entry(void *arg) {
    static_cast<PotatoVideoDecoder *>(arg)->_presenter_loop();
}

void PotatoVideoDecoder::_presenter_loop() {
    for (;;) {
        LightEvent_Wait(&job_ready);
        if (presenter_stop) {
            break;
        }
        _present_frame(job);
        LightEvent_Signal(&presenter_idle);
    }
}

void PotatoVideoDecoder::_lower_decoder_priority() {
    decoder_priority_checked = true;
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    if (priority > presenter_priority) {
        return;
    }
    if (R_FAILED(svcSetThreadPriority(CUR_THREAD_HANDLE,
                                      presenter_priority + 1))) {
        async_present = false;
        printf("[POTATO] decoder priority unchanged, presenting in sync\n");
    }
}

void PotatoVideoDecoder::_wait_presenter_idle() {
    if (!async_present) {
        return;
    }
    // timeout so a stuck presenter shows up instead of a silent freeze
    while (LightEvent_WaitTimeout(&presenter_idle, 1000000000LL)) {
        printf("[POTATO] presenter still busy after 1 s\n");
    }
}

void PotatoVideoDecoder::_queue_present(u8 *const *planes,
                                        const int *strides) {
    PresentJob frame;
    for (int i = 0; i < 3; i++) {
        frame.planes[i] = planes[i];
        frame.strides[i] = strides[i];
    }

    if (!async_present) {
        _present_frame(frame);
        return;
    }

    // caller already waited for the presenter
    job = frame;
    LightEvent_Clear(&presenter_idle);
    LightEvent_Signal(&job_ready);
}

void PotatoVideoDecoder::_present_frame(const PresentJob &frame) {
    u64 t_start = svcGetSystemTick();

    // keep the lock so the renderer can't switch tiled/linear mid frame
    renderer_lock.lock();
    const bool tiled = y2r->can_tile() && renderer->accepts_tiled_source();
    if (y2r->convert(frame.planes, frame.strides, rgb_img_buffer, tiled)) {
        renderer->set_perf_decode_ticks(svcGetSystemTick() - t_start);
        if (tiled) {
            renderer->write_tiled_px_to_framebuffer(rgb_img_buffer);
        } else {
            renderer->write_px_to_framebuffer(rgb_img_buffer);
        }
    }
    renderer_lock.unlock();
}

void PotatoVideoDecoder::_record_frame_cost(u64 frame_start_ticks) {
    const u64 elapsed_ticks = svcGetSystemTick() - frame_start_ticks;
    const u64 elapsed_us = (elapsed_ticks * 1000000ULL) / 268000000ULL;

    potato_record_decode_ticks(elapsed_ticks);
    decode_time_total += elapsed_us;
    decode_count++;

    const u64 budget_us =
        1000000ULL / (u64)(g_potato.fps > 0 ? g_potato.fps : 24);
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

int PotatoVideoDecoder::submit_decode_unit(PDECODE_UNIT decodeUnit) {
    if (async_present && !decoder_priority_checked) {
        _lower_decoder_priority();
    }
    if (_stream_fell_behind(decodeUnit)) {
        return DR_NEED_IDR;
    }
    const bool present_frame = !potato_should_skip_frame();

    const u64 frame_start_ticks = svcGetSystemTick();
    PLENTRY entry = decodeUnit->bufferList;
    int length = 0;

    ensure_buf_size(&ffmpeg_buffer, &ffmpeg_buffer_size,
                    decodeUnit->fullLength + AV_INPUT_BUFFER_PADDING_SIZE);

    while (entry != NULL) {
        memcpy((u8*)ffmpeg_buffer + length, entry->data, entry->length);
        length += entry->length;
        entry = entry->next;
    }
    potato_bench_record_frame(ffmpeg_buffer, length);

    if (ffmpeg_decode((unsigned char *)ffmpeg_buffer, length) < 0) {
        _record_frame_cost(frame_start_ticks);
        potato_frame_skipped();
        return DR_NEED_IDR;
    }

    // ffmpeg_get_frame() frees the previous frame, Y2R may still be reading it (rarely waits)
    _wait_presenter_idle();

    AVFrame *frame = ffmpeg_get_frame(false);
    if (!frame) {
        _record_frame_cost(frame_start_ticks);
        potato_frame_skipped();
        return DR_OK;
    }

    if (!present_frame) {
        _record_frame_cost(frame_start_ticks);
        _record_frame_stats(decodeUnit);
        potato_frame_skipped();
        return DR_OK;
    }

    _queue_present(frame->data, frame->linesize);
    potato_frame_decoded();
    _record_frame_cost(frame_start_ticks);
    _record_frame_stats(decodeUnit);
    return DR_OK;
}

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
    .capabilities     = CAPABILITY_REFERENCE_FRAME_INVALIDATION_AVC,
};
