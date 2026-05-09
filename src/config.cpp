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

#include "config.hpp"
#include "audio/audio.h"
#include "system/pair_record.hpp"
#include "util.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <getopt.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#define USER_PATHS "."
#define DEFAULT_CONFIG_DIR "/.config"
#define DEFAULT_CACHE_DIR "/.cache"

#define write_config_string(fd, key, value) fprintf(fd, "%s = %s\n", key, value)
#define write_config_int(fd, key, value) fprintf(fd, "%s = %d\n", key, value)
#define write_config_bool(fd, key, value)                                      \
    fprintf(fd, "%s = %s\n", key, value ? "true" : "false")

static struct option long_options[] = {
    {"width", required_argument, NULL, 'c'},
    {"height", required_argument, NULL, 'd'},
    {"bitrate", required_argument, NULL, 'g'},
    {"packetsize", required_argument, NULL, 'h'},
    {"app", required_argument, NULL, 'i'},
    {"sops", required_argument, NULL, 'l'},
    {"localaudio", required_argument, NULL, 'n'},
    {"fps", required_argument, NULL, 'v'},
    {"quitappafter", required_argument, NULL, '1'},
    {"viewonly", required_argument, NULL, '2'},
    {"port", required_argument, NULL, '6'},
    {"video_decoder", required_argument, NULL, '8'},
    {"motion_controls", required_argument, NULL, 'e'},
    {"swapfacebuttons", required_argument, NULL, 'A'},
    {"swaptriggersandshoulders", required_argument, NULL, 'B'},
    {"usetriggersformouse", required_argument, NULL, 'C'},
    {"better_screen", required_argument, NULL, 'D'},
    {"stable_stream", required_argument, NULL, 'E'},
    {"ultra_potato", required_argument, NULL, 'F'},
    {"stereoscopic_3d", required_argument, NULL, 'G'},
    {0, 0, 0, 0},
};

static char *copy_config_string(const std::string &value) {
    char *buffer = static_cast<char *>(malloc(value.size() + 1));
    if (buffer == NULL) {
        return NULL;
    }

    memcpy(buffer, value.c_str(), value.size() + 1);
    return buffer;
}

void parse_argument(int c, char *value, PCONFIGURATION config) {
    switch (c) {
    case 'c':
        config->stream.width = atoi(value);
        break;
    case 'd':
        config->stream.height = atoi(value);
        break;
    case 'e':
        config->motion_controls =
            ((value != NULL) && (strcmp(value, "true") == 0));
        break;
    case 'g':
        config->stream.bitrate = atoi(value);
        break;
    case 'h':
        config->stream.packetSize = atoi(value);
        break;
    case 'i':
        config->app = value;
        break;
    case 'l':
        config->sops = ((value == NULL) || (strcmp(value, "false") != 0));
        break;
    case 'n':
        config->localaudio = ((value != NULL) && (strcmp(value, "true") == 0));
        break;
    case 'v':
        config->stream.fps = atoi(value);
        break;
    case '1':
        config->quitappafter =
            ((value != NULL) && (strcmp(value, "true") == 0));
        break;
    case '2':
        config->viewonly = ((value != NULL) && (strcmp(value, "true") == 0));
        break;
    case '6':
        config->port = atoi(value);
        break;
    case '8':
        if (value == NULL) {
            config->video_decoder = VIDEO_DECODER_TYPE::HARDWARE_VIDEO_DECODER;
        } else {
            config->video_decoder = (VIDEO_DECODER_TYPE)atoi(value);
        }
        break;
    case 'A':
        config->swap_face_buttons =
            ((value != NULL) && (strcmp(value, "true") == 0));
        break;
    case 'B':
        config->swap_triggers_and_shoulders =
            ((value != NULL) && (strcmp(value, "true") == 0));
        break;
    case 'C':
        config->use_triggers_for_mouse =
            ((value != NULL) && (strcmp(value, "true") == 0));
        break;
    case 'D':
        config->experimental_better_screen =
            ((value != NULL) && (strcmp(value, "true") == 0));
        break;
    case 'E':
        config->experimental_stable_stream =
            ((value != NULL) && (strcmp(value, "true") == 0));
        break;
    case 'F':
        config->experimental_ultra_potato =
            ((value != NULL) && (strcmp(value, "true") == 0));
        break;
    case 'G':
        config->experimental_stereoscopic_3d =
            ((value != NULL) && (strcmp(value, "true") == 0));
        break;
    case 1:
        if (config->action == NULL)
            config->action = value;
        else if (config->address == NULL)
            config->address = value;
        else {
            fprintf(stderr, "Ignoring unexpected extra argument: %s\n",
                    value != NULL ? value : "(null)");
        }
        break;
    }
}

static bool config_file_parse_path(const char *filename, PCONFIGURATION config) {
    std::ifstream config_file(filename);
    if (!config_file.good()) {
        return false;
    }

    std::string line;
    while (std::getline(config_file, line)) {
        trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }

        size_t separator_pos = line.find('=');
        if (separator_pos == std::string::npos) {
            continue;
        }

        std::string key = line.substr(0, separator_pos);
        std::string value = line.substr(separator_pos + 1);
        trim(key);
        trim(value);
        if (key.empty() || value.empty()) {
            continue;
        }

        if (key == "address") {
            config->address = copy_config_string(value);
            continue;
        }
        if (key == "app") {
            config->app = copy_config_string(value);
            continue;
        }

        for (int i = 0; long_options[i].name != NULL; i++) {
            if (key == long_options[i].name) {
                parse_argument(long_options[i].val,
                               const_cast<char *>(value.c_str()), config);
                break;
            }
        }
    }
    return true;
}

bool config_file_parse(PCONFIGURATION config) {
    if (config_file_parse_path(STREAMPOTATO_CONFIG_PATH, config)) {
        return true;
    }
    return config_file_parse_path(LEGACY_MOONLIGHT_CONFIG_PATH, config);
}

bool config_save(const char *filename, PCONFIGURATION config) {
    if (strcmp(filename, STREAMPOTATO_CONFIG_PATH) == 0) {
        mkdir(STREAMPOTATO_3DS_PATH, 0775);
    } else if (strcmp(filename, LEGACY_MOONLIGHT_CONFIG_PATH) == 0) {
        mkdir(LEGACY_MOONLIGHT_3DS_PATH, 0775);
    }

    FILE *fd = fopen(filename, "w");
    if (fd == NULL && strcmp(filename, STREAMPOTATO_CONFIG_PATH) == 0) {
        mkdir(LEGACY_MOONLIGHT_3DS_PATH, 0775);
        fd = fopen(LEGACY_MOONLIGHT_CONFIG_PATH, "w");
    }
    if (fd == NULL) {
        fprintf(stderr, "Can't open configuration file: %s\n", filename);
        return false;
    }

    write_config_int(fd, "width", config->stream.width);
    write_config_int(fd, "height", config->stream.height);
    write_config_int(fd, "fps", config->stream.fps);
    write_config_int(fd, "bitrate", config->stream.bitrate);
    write_config_int(fd, "packetsize", config->stream.packetSize);
    write_config_bool(fd, "sops", config->sops);
    write_config_bool(fd, "localaudio", config->localaudio);
    write_config_bool(fd, "quitappafter", config->quitappafter);
    write_config_bool(fd, "viewonly", config->viewonly);
    write_config_int(fd, "video_decoder", config->video_decoder);
    write_config_bool(fd, "swapfacebuttons", config->swap_face_buttons);
    write_config_bool(fd, "swaptriggersandshoulders",
                      config->swap_triggers_and_shoulders);
    write_config_bool(fd, "usetriggersformouse",
                      config->use_triggers_for_mouse);
    write_config_bool(fd, "motion_controls", config->motion_controls);
    write_config_bool(fd, "better_screen", config->experimental_better_screen);
    write_config_bool(fd, "stable_stream", config->experimental_stable_stream);
    write_config_bool(fd, "ultra_potato", config->experimental_ultra_potato);
    write_config_bool(fd, "stereoscopic_3d",
                      config->experimental_stereoscopic_3d);

    if (strcmp(config->app, "Steam") != 0)
        write_config_string(fd, "app", config->app);

    fclose(fd);
    return true;
}

void config_parse(int argc, char *argv[], PCONFIGURATION config) {
    LiInitializeStreamConfiguration(&config->stream);

    config->stream.bitrate = -1;
    config->stream.packetSize = 1392;
    config->stream.streamingRemotely = STREAM_CFG_AUTO;
    config->stream.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    config->stream.supportedVideoFormats = SCM_H264;

    config->platform = "auto";
    config->app = "Steam";
    config->action = NULL;
    config->address = NULL;
    config->config_file = NULL;
    config->audio_device = NULL;
    config->sops = true;
    config->localaudio = false;
    config->unsupported = true;
    config->quitappafter = false;
    config->viewonly = false;
    config->port = 47989;

    if (access(STREAMPOTATO_3DS_PATH, F_OK) == 0) {
        strcpy(config->key_dir, STREAMPOTATO_3DS_PATH "/keys");
    } else {
        strcpy(config->key_dir, LEGACY_MOONLIGHT_3DS_PATH "/keys");
    }

    config->stream.width = 800;
    config->stream.height = 480;
    config->stream.fps = 60;
    config->stream.encryptionFlags = ENCFLG_NONE;
    config->video_decoder = VIDEO_DECODER_TYPE::HARDWARE_VIDEO_DECODER;
    config->motion_controls = false;
    config->swap_face_buttons = false;
    config->swap_triggers_and_shoulders = false;
    config->use_triggers_for_mouse = false;
    config->experimental_better_screen = false;
    config->experimental_stable_stream = false;
    config->experimental_ultra_potato = false;
    config->experimental_stereoscopic_3d = false;

    config_file_parse(config);

    if (config->stream.bitrate == -1) {
        // This table prefers 16:10 resolutions because they are
        // only slightly more pixels than the 16:9 equivalents, so
        // we don't want to bump those 16:10 resolutions up to the
        // next 16:9 slot.

        if (config->stream.width * config->stream.height <= 640 * 360) {
            config->stream.bitrate = (int)(1000 * (config->stream.fps / 30.0));
        } else if (config->stream.width * config->stream.height <= 854 * 480) {
            config->stream.bitrate = (int)(1500 * (config->stream.fps / 30.0));
        } else if (config->stream.width * config->stream.height <= 1366 * 768) {
            // This covers 1280x720 and 1280x800 too
            config->stream.bitrate = (int)(5000 * (config->stream.fps / 30.0));
        } else if (config->stream.width * config->stream.height <=
                   1920 * 1200) {
            config->stream.bitrate = (int)(10000 * (config->stream.fps / 30.0));
        } else if (config->stream.width * config->stream.height <=
                   2560 * 1600) {
            config->stream.bitrate = (int)(20000 * (config->stream.fps / 30.0));
        } else /* if (config->stream.width * config->stream.height <= 3840 *
                  2160) */
        {
            config->stream.bitrate = (int)(40000 * (config->stream.fps / 30.0));
        }
    }
}
