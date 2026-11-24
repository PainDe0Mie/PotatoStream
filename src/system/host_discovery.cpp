#include "host_discovery.hpp"

#include <3ds.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#define SCAN_BATCH 20
#define CONNECT_TIMEOUT_MS 1200
#define TRANSFER_TIMEOUT_MS 1500
#define MAX_SCAN_HOSTS 254
#define SERVERINFO_BUFFER_SIZE 8192

static int set_nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static bool socket_wait(int fd, bool for_write, int timeout_ms) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);

    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    const int ready = for_write ? select(fd + 1, NULL, &set, NULL, &tv)
                                : select(fd + 1, &set, NULL, NULL, &tv);
    return ready > 0 && FD_ISSET(fd, &set);
}

static bool socket_is_connected(int fd) {
    int so_error = 0;
    socklen_t length = sizeof(so_error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &length) < 0) {
        return false;
    }
    return so_error == 0;
}

static int open_connection(const struct sockaddr_in *addr, int timeout_ms) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    if (set_nonblocking(fd) < 0) {
        closesocket(fd);
        return -1;
    }

    // 3DS sockets don't always return EINPROGRESS for a pending connect, keep anything like it
    if (connect(fd, (const struct sockaddr *)addr, sizeof(*addr)) < 0 &&
        errno != EINPROGRESS && errno != EALREADY && errno != EAGAIN &&
        errno != EWOULDBLOCK) {
        closesocket(fd);
        return -1;
    }

    if (!socket_wait(fd, true, timeout_ms) || !socket_is_connected(fd)) {
        closesocket(fd);
        return -1;
    }
    return fd;
}

static bool is_gamestream_host(const std::string &address,
                               unsigned short port) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(address.c_str());

    const int fd = open_connection(&addr, CONNECT_TIMEOUT_MS);
    if (fd < 0) {
        return false;
    }

    char request[256];
    const int request_length =
        snprintf(request, sizeof(request),
                 "GET /serverinfo?uniqueid=0123456789ABCDEF HTTP/1.1\r\n"
                 "Host: %s:%u\r\n"
                 "User-Agent: StreamPotato-3DS\r\n"
                 "Connection: close\r\n\r\n",
                 address.c_str(), (unsigned)port);
    if (request_length <= 0 || request_length >= (int)sizeof(request)) {
        closesocket(fd);
        return false;
    }

    int sent = 0;
    while (sent < request_length) {
        if (!socket_wait(fd, true, TRANSFER_TIMEOUT_MS)) {
            closesocket(fd);
            return false;
        }
        const ssize_t written =
            send(fd, request + sent, request_length - sent, 0);
        if (written <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            closesocket(fd);
            return false;
        }
        sent += (int)written;
    }

    std::string payload;
    char chunk[512];
    while (payload.size() < SERVERINFO_BUFFER_SIZE) {
        if (!socket_wait(fd, false, TRANSFER_TIMEOUT_MS)) {
            break;
        }
        const ssize_t received = recv(fd, chunk, sizeof(chunk), 0);
        if (received == 0) {
            break;
        }
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            break;
        }
        payload.append(chunk, (size_t)received);
        if (payload.find("<root") != std::string::npos) {
            break;
        }
    }
    closesocket(fd);

    return payload.find("<root") != std::string::npos;
}

static void probe_batch(const std::vector<u32> &targets, size_t begin,
                        size_t end, unsigned short port,
                        std::vector<u32> *open_hosts, HostScanResult *stats) {
    int fds[SCAN_BATCH];
    u32 ips[SCAN_BATCH];
    int count = 0;

    for (size_t i = begin; i < end && count < SCAN_BATCH; i++) {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            stats->socket_errors++;
            continue;
        }
        if (set_nonblocking(fd) < 0) {
            stats->socket_errors++;
            closesocket(fd);
            continue;
        }

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(targets[i]);

        // ignore what connect() returns, SO_ERROR decides later
        connect(fd, (struct sockaddr *)&addr, sizeof(addr));

        fds[count] = fd;
        ips[count] = targets[i];
        count++;
    }

    stats->probed += count;
    if (count == 0) {
        return;
    }

    bool resolved[SCAN_BATCH];
    for (int i = 0; i < count; i++) {
        resolved[i] = false;
    }

    const u64 deadline = osGetTime() + CONNECT_TIMEOUT_MS;
    int pending = count;
    while (pending > 0) {
        const u64 now = osGetTime();
        if (now >= deadline) {
            break;
        }
        const u64 remaining_ms = deadline - now;

        fd_set write_set;
        FD_ZERO(&write_set);
        int max_fd = 0;
        for (int i = 0; i < count; i++) {
            if (resolved[i]) {
                continue;
            }
            FD_SET(fds[i], &write_set);
            if (fds[i] > max_fd) {
                max_fd = fds[i];
            }
        }

        struct timeval tv;
        tv.tv_sec = (long)(remaining_ms / 1000);
        tv.tv_usec = (long)((remaining_ms % 1000) * 1000);

        if (select(max_fd + 1, NULL, &write_set, NULL, &tv) <= 0) {
            break;
        }

        for (int i = 0; i < count; i++) {
            if (resolved[i] || !FD_ISSET(fds[i], &write_set)) {
                continue;
            }
            resolved[i] = true;
            pending--;
            if (socket_is_connected(fds[i])) {
                stats->open_ports++;
                open_hosts->push_back(ips[i]);
            }
        }
    }

    for (int i = 0; i < count; i++) {
        closesocket(fds[i]);
    }
}

HostScanResult host_discovery_scan(unsigned short port,
                                   const volatile bool *cancel) {
    HostScanResult result;

    struct in_addr ip;
    struct in_addr netmask;
    struct in_addr broadcast;
    if (SOCU_GetIPInfo(&ip, &netmask, &broadcast) != 0) {
        result.failed = true;
        result.error = "Could not read the console network configuration";
        return result;
    }

    const u32 local_ip = ntohl(ip.s_addr);
    u32 mask = ntohl(netmask.s_addr);
    if (local_ip == 0 || mask == 0) {
        result.failed = true;
        result.error = "The console is not connected to a network";
        return result;
    }

    u32 range = ~mask;
    if (range > MAX_SCAN_HOSTS + 1) {
        // Wider than a /24: sweep the console's own slice instead of giving up
        mask = 0xFFFFFF00u;
        range = 0xFFu;
    }
    if (range < 2) {
        result.failed = true;
        result.error = "Local network is too small to hold a host";
        return result;
    }

    const u32 network = local_ip & mask;
    std::vector<u32> targets;
    for (u32 host = 1; host < range; host++) {
        const u32 candidate = network | host;
        if (candidate == local_ip) {
            continue;
        }
        targets.push_back(candidate);
    }

    std::vector<u32> open_hosts;
    for (size_t base = 0; base < targets.size(); base += SCAN_BATCH) {
        if (cancel != nullptr && *cancel) {
            return result;
        }
        size_t batch_end = base + SCAN_BATCH;
        if (batch_end > targets.size()) {
            batch_end = targets.size();
        }
        probe_batch(targets, base, batch_end, port, &open_hosts, &result);
    }

    for (u32 open_ip : open_hosts) {
        if (cancel != nullptr && *cancel) {
            return result;
        }
        struct in_addr found;
        found.s_addr = htonl(open_ip);

        DiscoveredHost host;
        host.address = inet_ntoa(found);
        host.port = port;

        if (!is_gamestream_host(host.address, port)) {
            continue;
        }
        result.hosts.push_back(host);
    }

    return result;
}
