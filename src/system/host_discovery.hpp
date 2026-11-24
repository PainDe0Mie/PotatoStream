#pragma once

#include <string>
#include <vector>

struct DiscoveredHost {
    std::string address;
    unsigned short port = 47989;
};

struct HostScanResult {
    std::vector<DiscoveredHost> hosts;
    bool failed = false;
    std::string error;
    int probed = 0;
    int open_ports = 0;
    int socket_errors = 0;
};

HostScanResult host_discovery_scan(unsigned short port,
                                   const volatile bool *cancel);
