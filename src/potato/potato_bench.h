#pragma once

#include <string>

// Debug tool: record the video of the next stream to the SD card, then decode

void potato_bench_request_record(bool enabled);
bool potato_bench_record_requested();

void potato_bench_record_start(int video_format, int width, int height,
                               int fps);
void potato_bench_record_frame(const void *data, int length);
void potato_bench_record_stop();

bool potato_bench_dump_info(std::string *info);

typedef bool (*PotatoBenchProgress)(int done, int total, void *context);
bool potato_bench_run(std::string *report, PotatoBenchProgress progress,
                      void *context);
