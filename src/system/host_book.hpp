#pragma once

#include <string>
#include <vector>

struct HostRecord {
    std::string address;
    unsigned short port = 47989;
    std::string name;
};

std::string host_book_format_key(const std::string &address,
                                 unsigned short port);
bool host_book_parse_key(const std::string &key, std::string *address,
                         unsigned short *port);
std::vector<HostRecord> host_book_list();
bool host_book_contains(const std::string &address, unsigned short port);
void host_book_add(const std::string &address, unsigned short port,
                   const std::string &name);
void host_book_set_name(const std::string &address, unsigned short port,
                        const std::string &name);
void host_book_remove(const std::string &address, unsigned short port);
