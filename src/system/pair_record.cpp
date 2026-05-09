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

#include <algorithm>
#include <cctype>
#include <errno.h>
#include <fstream>
#include <locale>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "pair_record.hpp"

static FILE *open_pair_record_for_write() {
    mkdir(STREAMPOTATO_3DS_PATH, 0775);
    FILE *fd = fopen(STREAMPOTATO_3DS_PATH "/paired", "w");
    if (fd != NULL) {
        return fd;
    }
    mkdir(LEGACY_MOONLIGHT_3DS_PATH, 0775);
    return fopen(LEGACY_MOONLIGHT_3DS_PATH "/paired", "w");
}

static FILE *open_confirmed_pair_record_for_write() {
    mkdir(STREAMPOTATO_3DS_PATH, 0775);
    return fopen(STREAMPOTATO_3DS_PATH "/paired_confirmed", "w");
}

static std::vector<std::string> read_pair_records(const char *path,
                                                  bool *opened) {
    std::vector<std::string> addresses;
    std::ifstream pair_file(path);
    *opened = pair_file.good();
    if (!*opened) {
        return addresses;
    }

    std::string line;
    while (std::getline(pair_file, line)) {
        trim(line);
        if (line.empty()) {
            continue;
        }
        addresses.push_back(line);
    }
    return addresses;
}

static std::string format_pair_address(std::string address, uint16_t port) {
    trim(address);
    return address + ":" + std::to_string(port);
}

static void add_record(const char *path, FILE *(*open_for_write)(),
                       std::string entry) {
    trim(entry);
    bool opened = false;
    auto address_list = read_pair_records(path, &opened);
    for (auto existing : address_list) {
        trim(existing);
        if (existing == entry) {
            return;
        }
    }
    address_list.push_back(entry);

    remove(path);

    FILE *fd = open_for_write();
    if (fd == NULL) {
        return;
    }
    for (auto addr_string : address_list) {
        trim(addr_string);
        fprintf(fd, "%s\n", addr_string.c_str());
    }
    fclose(fd);
}

static void remove_record(const char *path, FILE *(*open_for_write)(),
                          std::string entry) {
    bool opened = false;
    auto address_list = read_pair_records(path, &opened);
    trim(entry);

    remove(path);

    FILE *fd = open_for_write();
    if (fd == NULL) {
        return;
    }
    for (auto addr_string : address_list) {
        trim(addr_string);
        if (addr_string != entry) {
            fprintf(fd, "%s\n", addr_string.c_str());
        }
    }
    fclose(fd);
}

// trim from start (in place)
inline void ltrim(std::string &s) {
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) {
                return !std::isspace(ch);
            }));
}

// trim from end (in place)
inline void rtrim(std::string &s) {
    s.erase(std::find_if(s.rbegin(), s.rend(),
                         [](unsigned char ch) { return !std::isspace(ch); })
                .base(),
            s.end());
}

// trim from both ends (in place)
void trim(std::string &s) {
    rtrim(s);
    ltrim(s);
}

void add_pair_address(std::string address, uint16_t port) {
    address = format_pair_address(address, port);

    auto address_list = list_paired_addresses();
    for (auto entry : address_list) {
        if (entry == address) {
            return;
        }
    }
    address_list.push_back(address);

    remove(STREAMPOTATO_3DS_PATH "/paired");
    remove(LEGACY_MOONLIGHT_3DS_PATH "/paired");

    FILE *fd = open_pair_record_for_write();
    if (fd == NULL) {
        return;
    }
    for (auto addr_string : address_list) {
        trim(addr_string);
        fprintf(fd, "%s\n", addr_string.c_str());
    }
    fclose(fd);
}

void remove_pair_address(std::string address, uint16_t port) {
    remove_pair_address_entry(format_pair_address(address, port));
}

void remove_pair_address_entry(std::string address) {
    auto address_list = list_paired_addresses();
    trim(address);

    remove(STREAMPOTATO_3DS_PATH "/paired");
    remove(LEGACY_MOONLIGHT_3DS_PATH "/paired");

    FILE *fd = open_pair_record_for_write();
    if (fd == NULL) {
        return;
    }
    for (auto addr_string : address_list) {
        if (addr_string != address) {
            trim(addr_string);
            fprintf(fd, "%s\n", addr_string.c_str());
        }
    }
    fclose(fd);

    remove_confirmed_pair_entry(address);
}

bool is_saved_pair_address(std::string address, uint16_t port) {
    const std::string entry = format_pair_address(address, port);
    auto address_list = list_paired_addresses();
    for (auto addr_string : address_list) {
        trim(addr_string);
        if (addr_string == entry) {
            return true;
        }
    }
    return false;
}

void add_confirmed_pair(std::string address, uint16_t port) {
    add_record(STREAMPOTATO_3DS_PATH "/paired_confirmed",
               open_confirmed_pair_record_for_write,
               format_pair_address(address, port));
}

void remove_confirmed_pair(std::string address, uint16_t port) {
    remove_confirmed_pair_entry(format_pair_address(address, port));
}

void remove_confirmed_pair_entry(std::string address) {
    remove_record(STREAMPOTATO_3DS_PATH "/paired_confirmed",
                  open_confirmed_pair_record_for_write, address);
}

bool is_confirmed_pair(std::string address, uint16_t port) {
    const std::string entry = format_pair_address(address, port);
    bool opened = false;
    auto confirmed =
        read_pair_records(STREAMPOTATO_3DS_PATH "/paired_confirmed", &opened);
    if (!opened) {
        return false;
    }
    for (auto confirmed_entry : confirmed) {
        trim(confirmed_entry);
        if (confirmed_entry == entry) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> list_paired_addresses() {
    bool opened = false;
    auto addresses = read_pair_records(STREAMPOTATO_3DS_PATH "/paired", &opened);
    if (opened) {
        return addresses;
    }

    return read_pair_records(LEGACY_MOONLIGHT_3DS_PATH "/paired", &opened);
}
