#pragma once

#include "video.hpp"
#include "potato/potato_profile.h"
#include <3ds/types.h>

class PotatoVideoDecoder : public VideoDecoderBase {
  public:
    PotatoVideoDecoder(int videoFormat, int width, int height, int redrawRate,
                       void *context, int drFlags);
    ~PotatoVideoDecoder();
    int submit_decode_unit(PDECODE_UNIT decodeUnit);

  private:
    int _write_yuv_to_framebuffer(const u8 **source, int width,
                                  int height, int px_size);
    void _record_frame_cost(u64 frame_start_ticks);
  private:
    void   *ffmpeg_buffer      = nullptr;
    size_t  ffmpeg_buffer_size = 0;
    u8     *rgb_img_buffer     = nullptr;

    uint64_t decode_time_total = 0;
    uint32_t decode_count      = 0;
    uint32_t slow_frame_count  = 0;
};

extern DECODER_RENDERER_CALLBACKS decoder_callbacks_potato;
