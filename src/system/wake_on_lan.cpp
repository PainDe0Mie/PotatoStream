#include "wake_on_lan.hpp"
#include "host_book.hpp"
#include "pair_record.hpp"

#include <3ds.h>
#include <arpa/inet.h>
#include <fstream>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#define WOL_BOOK_PATH STREAMPOTATO_3DS_PATH "/host_macs"

// the usual WoL ports plus the ones Moonlight clients use
static const unsigned short WOL_PORTS[] = {9, 7, 47009, 47998, 47999, 48000};

static bool parse_mac(const std::string &text, unsigned char out[6]) {
    unsigned int b[6];
    char tail;
    if (sscanf(text.c_str(), "%2x:%2x:%2x:%2x:%2x:%2x%c", &b[0], &b[1], &b[2],
               &b[3], &b[4], &b[5], &tail) != 6) {
        return false;
    }
    bool any = false;
    for (int i = 0; i < 6; i++) {
        out[i] = (unsigned char)b[i];
        any = any || b[i] != 0;
    }
    return any; // Sunshine sends 00:00:00:00:00:00 to unpaired clients
}

struct MacEntry {
    std::string key;
    std::string mac;
};

static std::vector<MacEntry> read_entries() {
    std::vector<MacEntry> entries;
    std::ifstream book(WOL_BOOK_PATH);
    std::string line;
    while (std::getline(book, line)) {
        trim(line);
        const size_t sep = line.find('|');
        if (sep == std::string::npos) {
            continue;
        }
        MacEntry entry = {line.substr(0, sep), line.substr(sep + 1)};
        trim(entry.key);
        trim(entry.mac);
        if (!entry.key.empty() && !entry.mac.empty()) {
            entries.push_back(entry);
        }
    }
    return entries;
}

static void write_entries(const std::vector<MacEntry> &entries) {
    FILE *f = fopen(WOL_BOOK_PATH, "w");
    if (f == NULL) {
        return;
    }
    for (const auto &entry : entries) {
        fprintf(f, "%s|%s\n", entry.key.c_str(), entry.mac.c_str());
    }
    fclose(f);
}

void wol_remember(const std::string &address, unsigned short port,
                  const char *mac) {
    unsigned char bytes[6];
    if (mac == NULL || !parse_mac(mac, bytes)) {
        return;
    }
    const std::string key = host_book_format_key(address, port);
    auto entries = read_entries();
    for (auto &entry : entries) {
        if (entry.key == key) {
            if (entry.mac == mac) {
                return; // nothing new, skip the SD write
            }
            entry.mac = mac;
            write_entries(entries);
            return;
        }
    }
    entries.push_back({key, mac});
    write_entries(entries);
}

bool wol_lookup(const std::string &address, unsigned short port,
                std::string *mac) {
    const std::string key = host_book_format_key(address, port);
    unsigned char bytes[6];
    for (const auto &entry : read_entries()) {
        if (entry.key == key && parse_mac(entry.mac, bytes)) {
            *mac = entry.mac;
            return true;
        }
    }
    return false;
}

void wol_forget(const std::string &address, unsigned short port) {
    const std::string key = host_book_format_key(address, port);
    auto entries = read_entries();
    std::vector<MacEntry> kept;
    for (const auto &entry : entries) {
        if (entry.key != key) {
            kept.push_back(entry);
        }
    }
    if (kept.size() != entries.size()) {
        write_entries(kept);
    }
}

bool wol_send(const std::string &address, const std::string &mac,
              std::string *error) {
    unsigned char bytes[6];
    if (!parse_mac(mac, bytes)) {
        *error = "unknown MAC address";
        return false;
    }

    // magic packet: 6x0xFF then the MAC 16 times
    unsigned char packet[6 + 16 * 6];
    memset(packet, 0xFF, 6);
    for (int i = 0; i < 16; i++) {
        memcpy(packet + 6 + i * 6, bytes, 6);
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        *error = "no socket";
        return false;
    }
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));

    std::vector<in_addr_t> targets;
    struct in_addr ip, netmask, broadcast;
    if (SOCU_GetIPInfo(&ip, &netmask, &broadcast) == 0 &&
        broadcast.s_addr != 0) {
        targets.push_back(broadcast.s_addr);
    }
    targets.push_back(htonl(INADDR_BROADCAST));
    struct in_addr host;
    if (inet_aton(address.c_str(), &host) != 0) {
        targets.push_back(host.s_addr); // works when the router still knows it
    }

    int sent = 0;
    for (in_addr_t target : targets) {
        for (unsigned short port : WOL_PORTS) {
            struct sockaddr_in dst;
            memset(&dst, 0, sizeof(dst));
            dst.sin_family = AF_INET;
            dst.sin_port = htons(port);
            dst.sin_addr.s_addr = target;
            if (sendto(fd, packet, sizeof(packet), 0,
                       (struct sockaddr *)&dst, sizeof(dst)) ==
                (ssize_t)sizeof(packet)) {
                sent++;
            }
        }
    }
    close(fd);

    if (sent == 0) {
        *error = "could not send on this network";
        return false;
    }
    return true;
}
