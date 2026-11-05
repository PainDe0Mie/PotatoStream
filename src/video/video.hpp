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

#pragma once

#include <Limelight.h>

#include <stdbool.h>

#define DISPLAY_FULLSCREEN 1
#define ENABLE_HARDWARE_ACCELERATION_1 2
#define ENABLE_HARDWARE_ACCELERATION_2 4
#define DISPLAY_ROTATE_MASK 24
#define DISPLAY_ROTATE_90 8
#define DISPLAY_ROTATE_180 16
#define DISPLAY_ROTATE_270 24

#define INIT_EGL 1
#define INIT_VDPAU 2
#define INIT_VAAPI 3

#define INITIAL_DECODER_BUFFER_SIZE (256 * 1024)

#include "../system/AtomicVar.hpp"
#include "../system/ThreadLock.hpp"
#include "../system/subscriber.hpp"
#include "../util.h"
#include "renderer/N3dsRenderer.hpp"
#include "y2r_converter.hpp"
#include <3ds/types.h>

enum DecodeReturnStatus { SUCCESS, NO_FRAME_PRODUCED, ERROR };

class VideoDecoderBase : public ISubscriber {
  public:
    VideoDecoderBase(int width, int height, int fps = 0);
    virtual ~VideoDecoderBase();
    void accept(IMessage *msg) override;

  private:
    void _accept_touch_state_changed(N3dsTouchType ttype);
    void _accept_keyboard_state_changed(KeyboardStateChangedMsg *msg);

  protected:
    bool _stream_fell_behind(PDECODE_UNIT decodeUnit); // call first in submit_decode_unit(), true = return DR_NEED_IDR
    void _record_frame_stats(PDECODE_UNIT decodeUnit); // call after decoding a frame (debug stats)
 
  protected:
    int image_width, image_height, texture_width, texture_height,
        surface_width, surface_height, pixel_size;
    std::unique_ptr<IN3dsRenderer> renderer = nullptr;
    ThreadLock renderer_lock;

  private:
    void _print_stats_header();

  private:
    u64 max_queue_delay_us;
    u64 last_resync_us = 0;
    int late_frames = 0;
    unsigned resync_count = 0;

    // debug stats, decode thread only
    AtomicVar<bool> debug_stats = false;
    bool stats_printing = false;
    bool stats_header_pending = false;
    u64 frame_start_us = 0;
    u64 stats_window_start_us = 0;
    s64 best_offset_us = 0;
    bool has_best_offset = false;
    u32 stats_frames = 0, stats_lag_frames = 0;
    u64 stats_decode_us = 0, stats_queue_us = 0, stats_lag_us = 0,
        stats_lag_max_us = 0;
    unsigned long stats_audio_us_start = 0;
};

class MockVideoDecoder : public VideoDecoderBase {
  public:
    MockVideoDecoder(int videoFormat, int width, int height, int redrawRate,
                     void *context, int drFlags);
    ~MockVideoDecoder();
};

class SoftVideoDecoder : public VideoDecoderBase {
  public:
    SoftVideoDecoder(int videoFormat, int width, int height, int redrawRate,
                     void *context, int drFlags);
    ~SoftVideoDecoder();
    int submit_decode_unit(PDECODE_UNIT decodeUnit);

  private:
    inline int _write_yuv_to_framebuffer(const u8 **source, const int *strides);

  private:
    void *ffmpeg_buffer;
    size_t ffmpeg_buffer_size;
    u8 *rgb_img_buffer;
    std::unique_ptr<Y2rConverter> y2r;
};

class MvdDecoder : public VideoDecoderBase {
  public:
    MvdDecoder(int videoFormat, int width, int height, int redrawRate,
               void *context, int drFlags);
    ~MvdDecoder();
    int submit_decode_unit(PDECODE_UNIT decodeUnit);

  private:
    DecodeReturnStatus _decode(unsigned char *indata, int inlen);
    void _log_first_pixel();

  private:
    void *nal_unit_buffer = NULL;
    size_t nal_unit_buffer_size = 0;
    MVDSTD_Config mvdstd_config;
    u8 *rgb_img_buffer;
    bool first_frame = true;
    bool color_24bit = false;
};

// true if direct submit, shown in debug stats
extern bool g_video_direct_submit;

// ask MVD for 24-bit output, falls back to RGB565
extern bool g_mvd_color_24bit;

extern DECODER_RENDERER_CALLBACKS decoder_callbacks_n3ds;
extern DECODER_RENDERER_CALLBACKS decoder_callbacks_n3ds_mvd;
extern DECODER_RENDERER_CALLBACKS decoder_callbacks_mock;
extern DECODER_RENDERER_CALLBACKS decoder_callbacks_potato;

// o3DS: decode part of each frame on core 1
extern bool g_potato_video_2nd_core;
