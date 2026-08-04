#pragma once

#include <string>
#include <vector>

bool menu_ui_init();
void menu_ui_shutdown();
bool menu_ui_is_active();

void menu_ui_draw_menu(const std::string &title, const std::string &subtitle,
                       const std::vector<std::string> &options, int selected,
                       const std::string &status,
                       const std::string &footer_hint);

void menu_ui_draw_message(const std::string &title, const std::string &body,
                          const std::string &status,
                          const std::string &footer_hint);

void menu_ui_draw_loading(const std::string &title, const std::string &body,
                          const std::string &status,
                          const std::string &footer_hint, int frame);

void menu_ui_draw_pairing(const std::string &title, const std::string &pin,
                          const std::string &body, const std::string &status,
                          const std::string &footer_hint, int frame);

void menu_ui_draw_number_editor(const std::string &title,
                                const std::string &subtitle,
                                const std::string &value,
                                const std::string &range_hint,
                                const std::string &status,
                                const std::string &footer_hint);

void menu_ui_draw_ip_picker(const std::string &title,
                            const std::string &subtitle, const int octets[4],
                            int selected_octet, const std::string &status,
                            const std::string &footer_hint);