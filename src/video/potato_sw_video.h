#pragma once

#include "video.hpp"
#include "y2r_converter.hpp"
#include "potato/potato_profile.h"
#include <3ds.h>
#include <atomic>

class PotatoVideoDecoder : public VideoDecoderBase {
  public:
    PotatoVideoDecoder(int videoFormat, int width, int height, int redrawRate,
                       void *context, int drFlags);
    ~PotatoVideoDecoder();
    int submit_decode_unit(PDECODE_UNIT decodeUnit);

  private:
  
    // decoded frame for the presenter, planes owned by ffmpeg until the next get_frame
    struct PresentJob {
        const u8 *planes[3];
        int strides[3];
    };

    static void _presenter_entry(void *arg);
    void _presenter_loop();
    void _lower_decoder_priority();
    void _wait_presenter_idle();
    void _queue_present(u8 *const *planes, const int *strides);
    void _present_frame(const PresentJob &frame);
    void _record_frame_cost(u64 frame_start_ticks);
  private:
    void   *ffmpeg_buffer      = nullptr;
    size_t  ffmpeg_buffer_size = 0;
    u8     *rgb_img_buffer     = nullptr;
    std::unique_ptr<Y2rConverter> y2r;

    // Y2R + GPU run here so the decoder can move on to the next frame
    Thread presenter = nullptr;
    s32 presenter_priority = 0x30;
    bool async_present = false;
    bool decoder_priority_checked = false;
    LightEvent job_ready;
    LightEvent presenter_idle;
    PresentJob job;
    std::atomic<bool> presenter_stop{false};

    uint64_t decode_time_total = 0;
    uint32_t decode_count      = 0;
    uint32_t slow_frame_count  = 0;
};

extern DECODER_RENDERER_CALLBACKS decoder_callbacks_potato;

// splits the slices of each frame with a helper thread on core 1 
extern bool g_potato_video_2nd_core;

int potato_decoder_perf_flags();