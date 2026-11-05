#include "potato_bench.h"

#include "../system/pair_record.hpp"
#include "../video/ffmpeg.h"
#include "../video/potato_sw_video.h"

#include <3ds.h>
#include <algorithm>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <vector>

namespace {

constexpr char DUMP_PATH[] = STREAMPOTATO_3DS_PATH "/stream_dump.bin";
constexpr char REPORT_PATH[] = STREAMPOTATO_3DS_PATH "/bench.txt";
constexpr char DUMP_MAGIC[8] = {'P', 'S', 'D', 'U', 'M', 'P', '0', '1'};

// 2 minutes at 1.5 Mbps
constexpr uint32_t DUMP_MAX_BYTES = 24 * 1024 * 1024;
constexpr uint32_t FRAME_MAX_BYTES = 1024 * 1024;
constexpr size_t RECORD_BUFFER_SIZE = 64 * 1024;

struct DumpHeader {
    char magic[8];
    uint32_t video_format;
    uint32_t width;
    uint32_t height;
    uint32_t fps;
};

bool g_record_requested = false;
FILE *g_record_file = nullptr;
char *g_record_buffer = nullptr;
uint32_t g_record_bytes = 0;
uint32_t g_record_frames = 0;

uint32_t hash_frame(uint32_t hash, const AVFrame *frame) {
    for (int plane = 0; plane < 3; plane++) {
        const int width = plane ? (frame->width + 1) / 2 : frame->width;
        const int height = plane ? (frame->height + 1) / 2 : frame->height;
        for (int y = 0; y < height; y++) {
            const uint8_t *row = frame->data[plane] + y * frame->linesize[plane];
            int x = 0;
            for (; x + 4 <= width; x += 4) {
                uint32_t word;
                memcpy(&word, row + x, 4);
                hash = (hash ^ word) * 16777619u;
            }
            for (; x < width; x++) {
                hash = (hash ^ row[x]) * 16777619u;
            }
        }
    }
    return hash;
}

bool read_header(FILE *file, DumpHeader *header) {
    return fread(header, sizeof(*header), 1, file) == 1 &&
           memcmp(header->magic, DUMP_MAGIC, sizeof(DUMP_MAGIC)) == 0 &&
           header->width > 0 && header->height > 0;
}

struct BitReader {
    const uint8_t *data;
    int size;
    int pos = 0;

    int bit() {
        if (pos >= size * 8) {
            return 0;
        }
        const int b = (data[pos >> 3] >> (7 - (pos & 7))) & 1;
        pos++;
        return b;
    }

    uint32_t ue() {
        int zeros = 0;
        while (!bit() && zeros < 31) {
            zeros++;
        }
        uint32_t value = 0;
        for (int i = 0; i < zeros; i++) {
            value = (value << 1) | bit();
        }
        return (1u << zeros) - 1 + value;
    }
};

// profile from the SPS and entropy coder from the PPS, -1 if not seen yet
void read_stream_info(const uint8_t *data, int length, int *profile,
                      int *cabac) {
    for (int i = 0; i + 4 < length; i++) {
        if (data[i] != 0 || data[i + 1] != 0 || data[i + 2] != 1) {
            continue;
        }
        const int nal_type = data[i + 3] & 0x1F;
        if (nal_type == 7) {
            *profile = data[i + 4];
        } else if (nal_type == 8) {
            BitReader reader = {data + i + 4, length - (i + 4)};
            reader.ue(); // pps id
            reader.ue(); // sps id
            *cabac = reader.bit();
        }
    }
}

const char *profile_name(int profile) {
    switch (profile) {
    case 66:
        return "Baseline";
    case 77:
        return "Main";
    case 100:
        return "High";
    default:
        return "?";
    }
}

const char *coder_name(int cabac) {
    return cabac < 0 ? "?" : cabac ? "CABAC" : "CAVLC";
}

double ms(uint32_t us) { return us / 1000.0; }

} // namespace

void potato_bench_request_record(bool enabled) {
    g_record_requested = enabled;
}

bool potato_bench_record_requested() { return g_record_requested; }

void potato_bench_record_start(int video_format, int width, int height,
                               int fps) {
    if (!g_record_requested) {
        return;
    }

    // one stream per request, so a forgotten toggle doesn't keep writing
    g_record_requested = false;
    potato_bench_record_stop();

    mkdir(STREAMPOTATO_3DS_PATH, 0775);
    g_record_file = fopen(DUMP_PATH, "wb");
    if (g_record_file == nullptr) {
        printf("[BENCH] can't create %s\n", DUMP_PATH);
        return;
    }
    g_record_buffer = (char *)malloc(RECORD_BUFFER_SIZE);
    if (g_record_buffer != nullptr) {
        setvbuf(g_record_file, g_record_buffer, _IOFBF, RECORD_BUFFER_SIZE);
    }

    DumpHeader header = {};
    memcpy(header.magic, DUMP_MAGIC, sizeof(DUMP_MAGIC));
    header.video_format = (uint32_t)video_format;
    header.width = (uint32_t)width;
    header.height = (uint32_t)height;
    header.fps = (uint32_t)fps;
    fwrite(&header, sizeof(header), 1, g_record_file);
    g_record_bytes = sizeof(header);
    g_record_frames = 0;
    printf("[BENCH] recording the stream to %s\n", DUMP_PATH);
}

void potato_bench_record_frame(const void *data, int length) {
    if (g_record_file == nullptr || length <= 0) {
        return;
    }
    if (g_record_bytes + 4 + (uint32_t)length > DUMP_MAX_BYTES) {
        printf("[BENCH] recording full\n");
        potato_bench_record_stop();
        return;
    }
    const uint32_t size = (uint32_t)length;
    if (fwrite(&size, sizeof(size), 1, g_record_file) != 1 ||
        fwrite(data, size, 1, g_record_file) != 1) {
        printf("[BENCH] write failed, recording stopped\n");
        potato_bench_record_stop();
        return;
    }
    g_record_bytes += 4 + size;
    g_record_frames++;
}

void potato_bench_record_stop() {
    if (g_record_file == nullptr) {
        return;
    }
    fclose(g_record_file);
    g_record_file = nullptr;
    free(g_record_buffer);
    g_record_buffer = nullptr;
    printf("[BENCH] recorded %lu frames (%lu KB)\n",
           (unsigned long)g_record_frames,
           (unsigned long)(g_record_bytes / 1024));
}

bool potato_bench_dump_info(std::string *info) {
    FILE *file = fopen(DUMP_PATH, "rb");
    if (file == nullptr) {
        return false;
    }
    DumpHeader header;
    const bool valid = read_header(file, &header);
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    fclose(file);
    if (!valid) {
        return false;
    }

    char text[96];
    snprintf(text, sizeof(text), "%lux%lu @ %lu fps, %ld KB",
             (unsigned long)header.width, (unsigned long)header.height,
             (unsigned long)header.fps, size / 1024);
    *info = text;
    return true;
}

bool potato_bench_run(std::string *report, PotatoBenchProgress progress,
                      void *context) {
    FILE *file = fopen(DUMP_PATH, "rb");
    if (file == nullptr) {
        *report = "No recording yet. Turn on Record next stream, then stream "
                  "for a minute.";
        return false;
    }
    DumpHeader header;
    if (!read_header(file, &header)) {
        fclose(file);
        *report = "The recording is damaged, record a new stream.";
        return false;
    }

    // frame sizes first, so the progress has a total and the buffer a size
    std::vector<uint32_t> sizes;
    uint32_t largest = 0;
    uint64_t total_bytes = 0;
    uint32_t size = 0;
    while (fread(&size, sizeof(size), 1, file) == 1) {
        if (size == 0 || size > FRAME_MAX_BYTES ||
            fseek(file, size, SEEK_CUR) != 0) {
            break;
        }
        sizes.push_back(size);
        largest = std::max(largest, size);
        total_bytes += size;
    }

    // a stream cut while recording can end with a partial frame
    if (!sizes.empty() && ftell(file) > 0) {
        long end = ftell(file);
        fseek(file, 0, SEEK_END);
        if (ftell(file) < end) {
            total_bytes -= sizes.back();
            sizes.pop_back();
        }
    }
    if (sizes.empty()) {
        fclose(file);
        *report = "The recording has no frames.";
        return false;
    }

    u8 *buffer = (u8 *)malloc(largest + AV_INPUT_BUFFER_PADDING_SIZE);
    if (buffer == nullptr) {
        fclose(file);
        *report = "Not enough memory for the benchmark.";
        return false;
    }

    // the recording is replayed once per decoder mode: one core, then the slices of each frame split between both cores 

    struct Mode {
        const char *name;
        int flags;
        u32 core1_limit;
    };
    const int base_flags = potato_decoder_perf_flags();
    const Mode modes[] = {
        {"1 core", base_flags, 0},
        {"2 cores, core 1 at 30%", base_flags | SLICE_CORE1, 30},
        {"2 cores, core 1 at 80%", base_flags | SLICE_CORE1, 80},
    };
    const int mode_count = (int)(sizeof(modes) / sizeof(modes[0]));
    u32 saved_limit = 0;
    const bool have_limit = R_SUCCEEDED(APT_GetAppCpuTimeLimit(&saved_limit));

    int profile = -1;
    int cabac = -1;
    bool cancelled = false;
    std::string screen;
    FILE *log = fopen(REPORT_PATH, "a");
    for (int m = 0; m < mode_count && !cancelled; m++) {
        const Mode &mode = modes[m];
        char limit_note[48] = "";
        if (mode.core1_limit != 0) {
            const Result res = APT_SetAppCpuTimeLimit(mode.core1_limit);
            if (R_FAILED(res)) {
                snprintf(limit_note, sizeof(limit_note), " (limit refused %08lX)",
                         (unsigned long)res);
            }
        }
        if (ffmpeg_init((int)header.video_format, (int)header.width,
                        (int)header.height, mode.flags, 1, 1) < 0) {
            screen += std::string(mode.name) + ": the decoder failed to start\n";
            continue;
        }

        fseek(file, sizeof(header), SEEK_SET);
        std::vector<uint32_t> times_us;
        times_us.reserve(sizes.size());
        int errors = 0;
        uint32_t picture_hash = 2166136261u;
        for (size_t i = 0; i < sizes.size(); i++) {
            if (fread(&size, sizeof(size), 1, file) != 1 ||
                fread(buffer, size, 1, file) != 1) {
                break;
            }
            memset(buffer + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
            if (profile < 0 || cabac < 0) {
                read_stream_info(buffer, (int)size, &profile, &cabac);
            }

            // only the decoder is timed, file reads and progress are not
            const u64 start = svcGetSystemTick();
            AVFrame *frame = nullptr;
            if (ffmpeg_decode(buffer, (int)size) < 0) {
                errors++;
            } else {
                frame = ffmpeg_get_frame(false);
            }
            const u64 ticks = svcGetSystemTick() - start;
            times_us.push_back((uint32_t)(ticks * 1000000ULL / SYSCLOCK_ARM11));
            if (frame != nullptr) {
                picture_hash = hash_frame(picture_hash, frame);
            }

            if (progress != nullptr && (i % 15) == 0 &&
                !progress((int)(m * sizes.size() + i + 1),
                          (int)(mode_count * sizes.size()), context)) {
                cancelled = true;
                break;
            }
        }
        ffmpeg_destroy();
        if (times_us.empty()) {
            continue;
        }

        uint64_t sum_us = 0;
        for (uint32_t t : times_us) {
            sum_us += t;
        }
        std::vector<uint32_t> sorted = times_us;
        std::sort(sorted.begin(), sorted.end());
        const uint32_t avg_us = (uint32_t)(sum_us / times_us.size());
        const uint32_t p95_us = sorted[(sorted.size() * 95) / 100];
        const uint32_t worst_us = sorted.back();
        const double fps_capacity = avg_us > 0 ? 1000000.0 / avg_us : 0.0;
        const uint32_t fps = header.fps > 0 ? header.fps : 1;
        const unsigned long kbps = (unsigned long)(total_bytes * 8 * fps /
                                                   sizes.size() / 1000);

        char text[256];
        if (m == 0) {
            snprintf(text, sizeof(text), "%lux%lu @ %lu fps, %lu kbps, %s %s, %u frames\n",
                     (unsigned long)header.width, (unsigned long)header.height,
                     (unsigned long)header.fps, kbps, profile_name(profile),
                     coder_name(cabac), (unsigned)times_us.size());
            screen += text;
        }
        snprintf(text, sizeof(text),
                 "%s%s: avg %.2f ms, p95 %.2f, %.1f fps%s\n", mode.name,
                 cancelled ? " (stopped)" : "", ms(avg_us), ms(p95_us),
                 fps_capacity, errors > 0 ? ", errors" : "");
        screen += text;

        if (log != nullptr) {
            fprintf(log,
                    "v%d.%d.%d | %lux%lu@%lu %lukbps %s %s | %u frames%s | "
                    "%s%s | avg %.2f p95 %.2f worst %.2f ms | %.1f fps | "
                    "%d errors | hash %08lx\n",
                    VERSION_MAJOR, VERSION_MINOR, VERSION_MICRO,
                    (unsigned long)header.width, (unsigned long)header.height,
                    (unsigned long)header.fps, kbps, profile_name(profile),
                    coder_name(cabac), (unsigned)times_us.size(),
                    cancelled ? " (stopped)" : "", mode.name, limit_note,
                    ms(avg_us), ms(p95_us), ms(worst_us), fps_capacity, errors,
                    (unsigned long)picture_hash);
        }
    }
    if (have_limit) {
        APT_SetAppCpuTimeLimit(saved_limit);
    }
    if (log != nullptr) {
        fclose(log);
    }
    free(buffer);
    fclose(file);

    if (screen.empty()) {
        *report = "Could not read the recording.";
        return false;
    }
    *report = screen;
    return !cancelled;
}
