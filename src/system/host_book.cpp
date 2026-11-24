#include "host_book.hpp"
#include "pair_record.hpp"

#include <fstream>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#define HOST_BOOK_PATH STREAMPOTATO_3DS_PATH "/hosts"

std::string host_book_format_key(const std::string &address,
                                 unsigned short port) {
    std::string trimmed = address;
    trim(trimmed);
    return trimmed + ":" + std::to_string(port);
}

bool host_book_parse_key(const std::string &key, std::string *address,
                         unsigned short *port) {
    std::string trimmed = key;
    trim(trimmed);
    if (trimmed.empty()) {
        return false;
    }

    std::string parsed_address = trimmed;
    unsigned short parsed_port = 47989;

    const size_t separator_pos = trimmed.rfind(':');
    if (separator_pos != std::string::npos) {
        parsed_address = trimmed.substr(0, separator_pos);
        const std::string port_string = trimmed.substr(separator_pos + 1);
        char *end = NULL;
        const long value = strtol(port_string.c_str(), &end, 10);
        if (end == NULL || *end != '\0' || value <= 0 || value > 65535) {
            return false;
        }
        parsed_port = (unsigned short)value;
    }

    trim(parsed_address);
    if (parsed_address.empty()) {
        return false;
    }

    *address = parsed_address;
    *port = parsed_port;
    return true;
}

static std::vector<HostRecord> read_book(bool *opened) {
    std::vector<HostRecord> records;
    std::ifstream book(HOST_BOOK_PATH);
    *opened = book.good();
    if (!*opened) {
        return records;
    }

    std::string line;
    while (std::getline(book, line)) {
        trim(line);
        if (line.empty()) {
            continue;
        }

        std::string key = line;
        std::string name;
        const size_t name_pos = line.find('|');
        if (name_pos != std::string::npos) {
            key = line.substr(0, name_pos);
            name = line.substr(name_pos + 1);
            trim(name);
        }

        HostRecord record;
        if (!host_book_parse_key(key, &record.address, &record.port)) {
            continue;
        }
        record.name = name.empty() ? record.address : name;
        records.push_back(record);
    }
    return records;
}

static void write_book(const std::vector<HostRecord> &records) {
    mkdir(STREAMPOTATO_3DS_PATH, 0775);
    FILE *fd = fopen(HOST_BOOK_PATH, "w");
    if (fd == NULL) {
        return;
    }
    for (const auto &record : records) {
        fprintf(fd, "%s:%u|%s\n", record.address.c_str(), (unsigned)record.port,
                record.name.c_str());
    }
    fclose(fd);
}

std::vector<HostRecord> host_book_list() {
    bool opened = false;
    auto records = read_book(&opened);
    if (opened) {
        return records;
    }

    for (auto entry : list_paired_addresses()) {
        HostRecord record;
        if (!host_book_parse_key(entry, &record.address, &record.port)) {
            continue;
        }
        record.name = record.address;
        records.push_back(record);
    }
    if (!records.empty()) {
        write_book(records);
    }
    return records;
}

bool host_book_contains(const std::string &address, unsigned short port) {
    std::string trimmed = address;
    trim(trimmed);
    for (const auto &record : host_book_list()) {
        if (record.address == trimmed && record.port == port) {
            return true;
        }
    }
    return false;
}

void host_book_add(const std::string &address, unsigned short port,
                   const std::string &name) {
    std::string trimmed = address;
    trim(trimmed);
    if (trimmed.empty()) {
        return;
    }

    auto records = host_book_list();
    for (const auto &record : records) {
        if (record.address == trimmed && record.port == port) {
            return;
        }
    }

    HostRecord record;
    record.address = trimmed;
    record.port = port;
    record.name = name.empty() ? trimmed : name;
    records.push_back(record);
    write_book(records);
}

void host_book_set_name(const std::string &address, unsigned short port,
                        const std::string &name) {
    std::string trimmed = address;
    trim(trimmed);
    std::string new_name = name;
    trim(new_name);
    if (new_name.empty()) {
        new_name = trimmed;
    }

    auto records = host_book_list();
    bool changed = false;
    for (auto &record : records) {
        if (record.address == trimmed && record.port == port) {
            record.name = new_name;
            changed = true;
            break;
        }
    }
    if (changed) {
        write_book(records);
    }
}

void host_book_remove(const std::string &address, unsigned short port) {
    std::string trimmed = address;
    trim(trimmed);

    auto records = host_book_list();
    std::vector<HostRecord> kept;
    for (const auto &record : records) {
        if (record.address == trimmed && record.port == port) {
            continue;
        }
        kept.push_back(record);
    }
    write_book(kept);

    remove_pair_address(trimmed, port);
}
