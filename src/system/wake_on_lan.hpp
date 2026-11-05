#pragma once

#include <string>

void wol_remember(const std::string &address, unsigned short port,
                  const char *mac);
bool wol_lookup(const std::string &address, unsigned short port,
                std::string *mac);
void wol_forget(const std::string &address, unsigned short port);

bool wol_send(const std::string &address, const std::string &mac,
              std::string *error);
