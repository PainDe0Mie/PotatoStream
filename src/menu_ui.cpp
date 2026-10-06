/*
 StreamPotato UI
 */

#include "menu_ui.hpp"

#include <3ds.h>
#include <citro2d.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct MenuUiState {
    bool initialized = false;
    C3D_RenderTarget *top = nullptr;
    C3D_RenderTarget *bottom = nullptr;
    C2D_TextBuf text_buf = nullptr;
    GSPGPU_FramebufferFormat prev_top_format = GSP_RGB565_OES;
    GSPGPU_FramebufferFormat prev_bottom_format = GSP_RGB565_OES;
    int frame = 0;
    float highlight_y = -1.0f;
    int last_selected = -1;
    std::string toast_msg;
    float toast_timer = 0.0f;
    MenuStatus status;
};

static MenuUiState g_ui;

struct Palette {
    u32 bg_top;  
    u32 bg_bottom;  
    u32 card;     
    u32 card_hi;
    u32 signal;  
    u32 signal_dk;  
    u32 signal_lt;  
    u32 spud;     
    u32 spud_lt;
    u32 ember;    
    u32 mint;     
    u32 text;      
    u32 text_sec; 
    u32 text_dim; 
    u32 line;    
    u32 toast_bg;
    u32 white;
    u32 ink;
};

static const Palette &pal() {
    static const Palette p = {
        C2D_Color32(26, 29, 62, 255),    // bg_top
        C2D_Color32(18, 18, 30, 255),    // bg_bottom
        C2D_Color32(35, 40, 80, 255),    // card
        C2D_Color32(47, 53, 98, 255),    // card_hi
        C2D_Color32(50, 120, 240, 255),  // signal
        C2D_Color32(36, 89, 196, 255),   // signal_dk
        C2D_Color32(127, 166, 255, 255), // signal_lt
        C2D_Color32(242, 178, 76, 255),  // spud
        C2D_Color32(255, 217, 142, 255), // spud_lt
        C2D_Color32(228, 120, 40, 255),  // ember
        C2D_Color32(70, 215, 160, 255),  // mint
        C2D_Color32(232, 236, 248, 255), // text
        C2D_Color32(155, 165, 198, 255), // text_sec
        C2D_Color32(95, 105, 143, 255),  // text_dim
        C2D_Color32(60, 66, 110, 255),   // line
        C2D_Color32(47, 53, 98, 235),    // toast_bg
        C2D_Color32(255, 255, 255, 255), // white
        C2D_Color32(43, 28, 4, 255),     // ink
    };
    return p;
}


static inline u32 color_alpha(u32 color, u8 alpha) {
    return (color & 0x00FFFFFFu) | ((u32)alpha << 24);
}

// Linear blend of two colours
static u32 mix(u32 a, u32 b, float t) {
    u32 out = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        const float ca = (float)((a >> shift) & 0xFFu);
        const float cb = (float)((b >> shift) & 0xFFu);
        out |= ((u32)(ca + (cb - ca) * t + 0.5f) & 0xFFu) << shift;
    }
    return out;
}


void draw_text(const std::string &text, float x, float y, float z,
               float scale, float width, u32 color, u32 flags = 0) {
    if (!g_ui.initialized || !g_ui.text_buf || text.empty()) return;
    C2D_Text t;
    if (!C2D_TextParse(&t, g_ui.text_buf, text.c_str())) return;
    C2D_TextOptimize(&t);
    u32 f = flags | C2D_WithColor;
    if (width > 0.0f) {
        f |= C2D_WordWrap;
        C2D_DrawText(&t, f, x, y, z, scale, scale, color, width);
    } else {
        C2D_DrawText(&t, f, x, y, z, scale, scale, color);
    }
}

static float text_width(const std::string &text, float scale) {
    C2D_Text t;
    if (!g_ui.text_buf || text.empty() ||
        !C2D_TextParse(&t, g_ui.text_buf, text.c_str())) {
        return 0.0f;
    }
    C2D_TextOptimize(&t);
    float width = 0.0f;
    C2D_TextGetDimensions(&t, scale, scale, &width, nullptr);
    return width;
}

static float text_block_height(const std::string &text, float scale,
                               float width) {
    if (text.empty()) return 0.0f;

    float line_h = 0.0f;
    C2D_Text probe;
    if (g_ui.text_buf && C2D_TextParse(&probe, g_ui.text_buf, "A")) {
        float probe_w = 0.0f;
        C2D_TextGetDimensions(&probe, scale, scale, &probe_w, &line_h);
    }
    if (line_h <= 0.0f) line_h = 30.0f * scale;

    int lines = 0;
    size_t start = 0;
    while (true) {
        size_t nl = text.find('\n', start);
        std::string line = nl == std::string::npos
                               ? text.substr(start)
                               : text.substr(start, nl - start);
        float w = text_width(line, scale);
        lines += width > 0.0f && w > width
                     ? (int)((w - 0.01f) / width) + 1
                     : 1;
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return line_h * (float)lines;
}

static std::string ellipsize(const std::string &text, float scale,
                             float width) {
    if (text.empty() || width <= 0.0f || text_width(text, scale) <= width) {
        return text;
    }
    std::string s = text;
    while (!s.empty()) {
        s.pop_back();
        // never leave half of a UTF-8 sequence behind
        while (!s.empty() && (((unsigned char)s.back()) & 0xC0u) == 0x80u) {
            s.pop_back();
        }
        if (!s.empty() && ((unsigned char)s.back()) >= 0xC0u) {
            s.pop_back();
        }
        if (text_width(s + "...", scale) <= width) break;
    }
    return s + "...";
}

static void draw_text_centered(const std::string &text, float cx, float y,
                               float z, float scale, u32 color) {
    draw_text(text, cx - text_width(text, scale) * 0.5f, y, z, scale, 0.0f,
              color);
}

static void draw_text_right(const std::string &text, float right, float y,
                            float z, float scale, u32 color) {
    draw_text(text, right - text_width(text, scale), y, z, scale, 0.0f, color);
}

static bool ends_with(const std::string &s, const char *suffix) {
    const size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

struct ToggleHint {
    bool is_toggle;
    bool is_on;
    std::string label;
};

static ToggleHint detect_toggle(const std::string &s) {
    ToggleHint h = {false, false, s};
    if (ends_with(s, ": enabled")) {
        h.is_toggle = true;
        h.is_on = true;
        h.label = s.substr(0, s.size() - 9);
    } else if (ends_with(s, ": disabled")) {
        h.is_toggle = true;
        h.is_on = false;
        h.label = s.substr(0, s.size() - 10);
    }
    return h;
}

static void rrect(float x, float y, float z, float w, float h, float r,
                  u32 color) {
    r = std::min(r, std::min(w, h) * 0.5f);
    C2D_DrawRectSolid(x + r, y, z, w - 2.0f * r, h, color);
    C2D_DrawRectSolid(x, y + r, z, r, h - 2.0f * r, color);
    C2D_DrawRectSolid(x + w - r, y + r, z, r, h - 2.0f * r, color);
    C2D_DrawCircleSolid(x + r, y + r, z, r, color);
    C2D_DrawCircleSolid(x + w - r, y + r, z, r, color);
    C2D_DrawCircleSolid(x + r, y + h - r, z, r, color);
    C2D_DrawCircleSolid(x + w - r, y + h - r, z, r, color);
}

static void rrect_hgrad(float x, float y, float z, float w, float h, float r,
                        u32 left, u32 right) {
    r = std::min(r, std::min(w, h) * 0.5f);
    C2D_DrawRectangle(x + r, y, z, w - 2.0f * r, h, left, right, left, right);
    C2D_DrawRectSolid(x, y + r, z, r, h - 2.0f * r, left);
    C2D_DrawRectSolid(x + w - r, y + r, z, r, h - 2.0f * r, right);
    C2D_DrawCircleSolid(x + r, y + r, z, r, left);
    C2D_DrawCircleSolid(x + r, y + h - r, z, r, left);
    C2D_DrawCircleSolid(x + w - r, y + r, z, r, right);
    C2D_DrawCircleSolid(x + w - r, y + h - r, z, r, right);
}

static void draw_glow(float x, float y, float w, float h, u32 color) {
    C2D_DrawRectSolid(x + 4.0f, y + h - 1.0f, 0.12f, w - 8.0f, 3.0f,
                      color_alpha(color, 54));
    C2D_DrawRectSolid(x + 9.0f, y + h + 2.0f, 0.12f, w - 18.0f, 2.0f,
                      color_alpha(color, 30));
    C2D_DrawRectSolid(x + 16.0f, y + h + 4.0f, 0.12f, w - 32.0f, 2.0f,
                      color_alpha(color, 14));
}

bool begin_frame() {
    if (!g_ui.initialized || !g_ui.text_buf) return false;
    if (!g_ui.top || !g_ui.bottom) return false;
    if (!gspHasGpuRight()) return false;

    g_ui.frame++;
    if (g_ui.toast_timer > 0.0f)
        g_ui.toast_timer -= 1.0f / 60.0f;
    C2D_TextBufClear(g_ui.text_buf);
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    return true;
}

void end_frame() { C3D_FrameEnd(0); }

// Vertical gradient, a faint blue wash in the top-right corner and the
// blue-to-gold stripe along the top edge
static void draw_screen_bg(float w) {
    const auto &p = pal();
    C2D_DrawRectangle(0.0f, 0.0f, 0.0f, w, 240.0f, p.bg_top, p.bg_top,
                      p.bg_bottom, p.bg_bottom);
    const u32 clear = color_alpha(p.signal, 0);
    C2D_DrawRectangle(w - 220.0f, 0.0f, 0.01f, 220.0f, 130.0f, clear,
                      color_alpha(p.signal, 46), clear, clear);
    const float mid = w * 0.62f;
    C2D_DrawRectangle(0.0f, 0.0f, 0.02f, mid, 2.0f, p.signal, p.signal_lt,
                      p.signal, p.signal_lt);
    C2D_DrawRectangle(mid, 0.0f, 0.02f, w - mid, 2.0f, p.signal_lt, p.spud,
                      p.signal_lt, p.spud);
}

void draw_bg() {
    C2D_TargetClear(g_ui.top, pal().bg_bottom);
    C2D_TargetClear(g_ui.bottom, pal().bg_bottom);
    C2D_SceneBegin(g_ui.top);
    draw_screen_bg(400.0f);
    C2D_SceneBegin(g_ui.bottom);
    draw_screen_bg(320.0f);
}

void draw_toast() {
    if (g_ui.toast_timer <= 0.0f) return;
    const auto &p = pal();
    float a = g_ui.toast_timer < 0.4f ? g_ui.toast_timer / 0.4f : 1.0f;
    u8 alpha = (u8)(255.0f * a);
    const float tw = 288.0f, tx = 16.0f;
    const float th = std::max(
        26.0f, text_block_height(g_ui.toast_msg, 0.32f, tw - 20.0f) + 14.0f);
    const float ty = 232.0f - th;

    C2D_DrawRectSolid(tx, ty, 0.5f, tw, th, color_alpha(p.toast_bg, alpha));
    C2D_DrawRectSolid(tx, ty, 0.6f, tw, 2.0f, color_alpha(p.signal, alpha));
    draw_text(g_ui.toast_msg, tx + 10.0f, ty + 7.0f, 0.7f, 0.32f, tw - 20.0f,
              color_alpha(p.text, alpha));
}

float draw_top_header(const std::string &title, const std::string &subtitle) {
    const auto &p = pal();
    const float text_x = 13.0f;
    const float text_w = 400.0f - text_x - 16.0f;

    draw_text(title, text_x, 13.0f, 0.3f, 0.6f, text_w, p.text);
    float y = std::max(36.0f, 13.0f + text_block_height(title, 0.6f, text_w)) +
              3.0f;

    float sep_y;
    if (subtitle.empty()) {
        sep_y = y + 2.0f;
    } else {
        draw_text(subtitle, 16.0f, y, 0.3f, 0.32f, 368.0f, p.text_sec);
        sep_y = y + text_block_height(subtitle, 0.32f, 368.0f) + 7.0f;
    }

    C2D_DrawRectangle(16.0f, sep_y, 0.1f, 368.0f, 1.0f,
                      color_alpha(p.text_sec, 90), color_alpha(p.text_sec, 12),
                      color_alpha(p.text_sec, 90), color_alpha(p.text_sec, 12));
    return sep_y + 1.0f;
}

void draw_menu_list(const std::vector<std::string> &options, int selected,
                    float start_y, float item_h, int visible_count,
                    const std::vector<MenuRowAction> *row_actions,
                    bool action_focused) {
    const auto &p = pal();
    const int n = (int)options.size();
    const int first =
        std::max(0, std::min(selected - 1, std::max(0, n - visible_count)));

    bool list_has_action = false;
    if (row_actions != nullptr) {
        for (const MenuRowAction &a : *row_actions) {
            if (!a.label.empty()) {
                list_has_action = true;
                break;
            }
        }
    }

    constexpr float row_x = 8.0f;
    constexpr float row_pad = 2.0f;
    constexpr float radius = 8.0f;
    constexpr float box_w = 46.0f;
    constexpr float gutter = 54.0f;
    constexpr float label_scale = 0.45f;
    constexpr float sub_scale = 0.32f;
    constexpr float state_scale = 0.27f;
    constexpr float value_scale = 0.42f;
    const float row_w = list_has_action ? 384.0f - gutter : 384.0f;
    const float box_x = row_x + row_w + (gutter - box_w) * 0.5f;
    const float bar_h = item_h - row_pad * 2.0f;
    const u32 pill_bg = C2D_Color32(22, 25, 52, 255);
    const u32 pill_bg_sel = C2D_Color32(20, 48, 115, 255);
    const u32 sub_sel = C2D_Color32(210, 224, 252, 255);

    auto action_of = [&](int idx) -> const MenuRowAction * {
        return row_actions != nullptr && idx >= 0 &&
                       idx < (int)row_actions->size() &&
                       !(*row_actions)[idx].label.empty()
                   ? &(*row_actions)[idx]
                   : nullptr;
    };

    const float target_y = start_y + (selected - first) * item_h;
    if (g_ui.highlight_y < 0.0f ||
        std::fabs(target_y - g_ui.highlight_y) > item_h * 2.5f) {
        g_ui.highlight_y = target_y;
    } else {
        g_ui.highlight_y += (target_y - g_ui.highlight_y) * 0.35f;
        if (std::fabs(target_y - g_ui.highlight_y) < 0.4f) {
            g_ui.highlight_y = target_y;
        }
    }
    g_ui.last_selected = selected;

    // Selection bar
    const bool sel_action_focus = action_of(selected) != nullptr && action_focused;
    {
        const float bar_y = g_ui.highlight_y + row_pad;
        if (sel_action_focus) {
            rrect_hgrad(row_x, bar_y, 0.15f, row_w, bar_h, radius,
                        C2D_Color32(35, 64, 127, 255),
                        C2D_Color32(29, 54, 112, 255));
        } else {
            draw_glow(row_x, bar_y, row_w, bar_h, p.signal);
            rrect_hgrad(row_x, bar_y, 0.15f, row_w, bar_h, radius, p.signal,
                        p.signal_dk);
            C2D_DrawRectSolid(row_x + radius, bar_y + 1.0f, 0.16f,
                              row_w - 2.0f * radius, 1.0f,
                              color_alpha(p.white, 70));
        }
    }

    for (int i = 0; i < visible_count && first + i < n; i++) {
        const int idx = first + i;
        const float y = start_y + i * item_h;
        const float card_y = y + row_pad;
        const bool sel = idx == selected;
        const MenuRowAction *action = action_of(idx);
        const bool act_sel = sel && action != nullptr && action_focused;
        const bool name_sel = sel && !act_sel;

        if (!sel) {
            rrect(row_x, card_y, 0.1f, row_w, bar_h, radius, p.card);
        }

        // Secondary button on the right edge
        if (action != nullptr) {
            u32 caption_color = p.text_sec;
            if (act_sel) {
                draw_glow(box_x, card_y, box_w, bar_h, p.signal);
                rrect_hgrad(box_x, card_y, 0.15f, box_w, bar_h, radius,
                            p.signal, p.signal_dk);
                caption_color = p.white;
            } else if (action->active) {
                rrect(box_x, card_y, 0.15f, box_w, bar_h, radius,
                      mix(p.card, p.spud, 0.2f));
                caption_color = p.spud_lt;
            } else {
                rrect(box_x, card_y, 0.15f, box_w, bar_h, radius, p.card_hi);
            }

            // Shrink the caption until it fits the box
            const float natural = text_width(action->label, 1.0f);
            float caption_scale = 0.36f;
            if (natural > 0.0f && natural * caption_scale > box_w - 10.0f) {
                caption_scale = (box_w - 10.0f) / natural;
            }
            const float caption_w = text_width(action->label, caption_scale);
            draw_text(action->label, box_x + (box_w - caption_w) * 0.5f,
                      card_y + (bar_h - 30.0f * caption_scale) * 0.5f, 0.3f,
                      caption_scale, 0.0f, caption_color);
        }

        // Row content
        // and enabled"/"disabled" becomes a switch.
        std::string name = options[idx];
        std::string sub;
        std::string value;
        const size_t nl = name.find('\n');
        if (nl != std::string::npos) {
            sub = name.substr(nl + 1);
            name = name.substr(0, nl);
        }
        const size_t tab = name.find('\t');
        if (tab != std::string::npos) {
            value = name.substr(tab + 1);
            name = name.substr(0, tab);
        }

        const ToggleHint th = detect_toggle(name);
        std::string label = th.is_toggle ? th.label : name;
        const bool is_exp = label.rfind("Experimental ", 0) == 0;

        const u32 label_color = name_sel ? p.white : p.text;
        const u32 sub_color = name_sel ? sub_sel : p.text_sec;
        const float content_right = row_x + row_w - 12.0f;
        float x_text = row_x + 12.0f;

        if (is_exp) {
            constexpr float tag_w = 28.0f;
            constexpr float tag_h = 14.0f;
            const float tag_y = card_y + (bar_h - tag_h) * 0.5f;
            rrect(x_text, tag_y, 0.2f, tag_w, tag_h, 4.0f,
                  name_sel ? mix(p.signal, p.white, 0.25f)
                           : mix(p.card, p.signal, 0.3f));
            draw_text("EXP", x_text + 4.0f, tag_y + 3.0f, 0.3f, 0.25f, 0.0f,
                      name_sel ? p.white : p.signal_lt);
            x_text += tag_w + 6.0f;
            label = label.substr(13);
        }

        // Right side: switch
        float right_w = 0.0f;
        if (th.is_toggle) {
            constexpr float sw_w = 34.0f;
            constexpr float sw_h = 18.0f;
            const float sw_x = content_right - sw_w;
            const float sw_y = card_y + (bar_h - sw_h) * 0.5f;
            u32 track;
            u32 knob;
            if (name_sel) {
                track = th.is_on ? p.white : pill_bg_sel;
                knob = th.is_on ? p.signal_dk : sub_sel;
            } else {
                track = th.is_on ? p.signal : p.card_hi;
                knob = th.is_on ? p.white : p.text_dim;
            }
            rrect(sw_x, sw_y, 0.2f, sw_w, sw_h, sw_h * 0.5f, track);
            C2D_DrawCircleSolid(th.is_on ? sw_x + sw_w - 9.0f : sw_x + 9.0f,
                                sw_y + sw_h * 0.5f, 0.25f, 6.5f, knob);

            const std::string state = th.is_on ? "ON" : "OFF";
            u32 state_color = p.text_dim;
            if (name_sel) {
                state_color = sub_sel;
            } else if (th.is_on) {
                state_color = p.signal_lt;
            }
            draw_text_right(state, sw_x - 8.0f,
                            card_y + (bar_h - 30.0f * state_scale) * 0.5f,
                            0.3f, state_scale, state_color);
            right_w = sw_w + 8.0f + text_width(state, state_scale) + 10.0f;
        } else if (!value.empty()) {
            const float vw = std::min(
                160.0f, std::max(30.0f, text_width(value, value_scale) + 20.0f));
            const float pill_h = 20.0f;
            const float pill_x = content_right - vw;
            const float pill_y = card_y + (bar_h - pill_h) * 0.5f;
            rrect(pill_x, pill_y, 0.2f, vw, pill_h, 6.0f,
                  name_sel ? pill_bg_sel : pill_bg);
            draw_text(ellipsize(value, value_scale, vw - 14.0f),
                      pill_x + 10.0f,
                      pill_y + (pill_h - 30.0f * value_scale) * 0.5f, 0.3f,
                      value_scale, 0.0f, name_sel ? p.white : p.text);
            right_w = vw + 10.0f;
        }

        const float text_w = content_right - x_text - right_w;
        const bool has_sub = !sub.empty();
        const float name_h = 30.0f * label_scale;
        const float total_h = has_sub ? name_h + 2.0f + 30.0f * sub_scale : name_h;
        const float text_y = card_y + (bar_h - total_h) * 0.5f;

        draw_text(ellipsize(label, label_scale, text_w), x_text, text_y, 0.3f,
                  label_scale, 0.0f, label_color);
        if (has_sub) {
            draw_text(ellipsize(sub, sub_scale, text_w), x_text,
                      text_y + name_h + 2.0f, 0.3f, sub_scale, 0.0f, sub_color);
        }
    }

    // Scrollbar (right edge)
    if (n > visible_count) {
        const float track_h = visible_count * item_h;
        rrect(394.0f, start_y, 0.3f, 4.0f, track_h, 2.0f, p.card_hi);
        const float thumb_h =
            std::max(10.0f, track_h * (float)visible_count / (float)n);
        const float thumb_y = start_y + (track_h - thumb_h) * (float)first /
                                            (float)(n - visible_count);
        rrect(394.0f, thumb_y, 0.4f, 4.0f, thumb_h, 2.0f, p.text_dim);
    }
}

static float draw_badge(const std::string &label, float right, float y,
                        u32 bg, u32 fg) {
    const float scale = 0.28f;
    const float width = text_width(label, scale) + 16.0f;
    const float height = 16.0f;
    rrect(right - width, y, 0.2f, width, height, 5.0f, bg);
    draw_text(label, right - width + 8.0f, y + (height - 30.0f * scale) * 0.5f,
              0.3f, scale, 0.0f, fg);
    return width;
}

static float draw_status_panel(const std::string &fallback) {
    const auto &p = pal();
    const MenuStatus &s = g_ui.status;

    draw_text("STATUS", 16.0f, 12.0f, 0.3f, 0.28f, 0.0f, p.signal_lt);

    if (s.mode.empty()) {
        const float h =
            std::max(26.0f, text_block_height(fallback, 0.34f, 268.0f) + 14.0f);
        rrect(16.0f, 28.0f, 0.1f, 288.0f, h, 8.0f, p.card);
        draw_text(fallback, 26.0f, 35.0f, 0.3f, 0.34f, 268.0f, p.text);
        return 28.0f + h + 8.0f;
    }

    // Badge: gold/orange
    const bool old_profile = s.mode.rfind("OLD", 0) == 0;
    u32 badge_bg = p.signal;
    u32 badge_fg = p.white;
    if (s.alert) {
        badge_bg = p.ember;
    } else if (old_profile) {
        badge_bg = p.spud;
        badge_fg = p.ink;
    }
    draw_badge(s.mode, 304.0f, 8.0f, badge_bg, badge_fg);

    float y = 30.0f;
    if (!s.detail.empty()) {
        draw_text(s.detail, 16.0f, y, 0.3f, 0.32f, 288.0f, p.text_sec);
        y += text_block_height(s.detail, 0.32f, 288.0f) + 8.0f;
    } else {
        y += 4.0f;
    }

    if (!s.resolution.empty()) {
        struct Tile {
            const char *label;
            const std::string *value;
            int span;
        };
        const Tile tiles[5] = {
            {"RES", &s.resolution, 1}, {"FPS", &s.fps, 1},
            {"AUDIO", &s.audio, 1},    {"RATE", &s.bitrate, 2},
            {"PKT", &s.packet, 1},
        };
        constexpr float gap = 6.0f;
        constexpr float tile_h = 38.0f;
        const float unit = (288.0f - 2.0f * gap) / 3.0f;

        int col = 0;
        int row = 0;
        for (const Tile &t : tiles) {
            if (t.value->empty()) {
                continue;
            }
            if (col + t.span > 3) {
                col = 0;
                row++;
            }
            const float tx = 16.0f + col * (unit + gap);
            const float ty = y + row * (tile_h + gap);
            const float tw = t.span * unit + (t.span - 1) * gap;
            rrect(tx, ty, 0.1f, tw, tile_h, 8.0f, p.card);
            draw_text(t.label, tx + 9.0f, ty + 6.0f, 0.3f, 0.24f, 0.0f,
                      p.text_dim);

            float value_scale = 0.50f;
            const float natural = text_width(*t.value, 1.0f);
            if (natural > 0.0f && natural * value_scale > tw - 18.0f) {
                value_scale = (tw - 18.0f) / natural;
            }
            draw_text(*t.value, tx + 9.0f, ty + 17.0f, 0.3f, value_scale, 0.0f,
                      p.text);
            col += t.span;
            if (col >= 3) {
                col = 0;
                row++;
            }
        }
        const int rows = row + (col > 0 ? 1 : 0);
        y += rows * (tile_h + gap);
    }

    float chip_x = 16.0f;
    for (const auto &flag : s.flags) {
        const float width = text_width(flag, 0.27f) + 16.0f;
        if (chip_x + width > 304.0f) {
            chip_x = 16.0f;
            y += 22.0f;
        }
        const bool emergency = flag == "emergency";
        rrect(chip_x, y, 0.1f, width, 16.0f, 8.0f,
              emergency ? mix(p.card, p.ember, 0.35f) : p.card_hi);
        draw_text(flag, chip_x + 8.0f, y + 4.0f, 0.3f, 0.27f, 0.0f,
                  emergency ? p.white : p.text_sec);
        chip_x += width + 5.0f;
    }
    if (!s.flags.empty()) {
        y += 22.0f;
    }

    return y;
}

// A and B style
struct Hint {
    std::string key;
    std::string label;
};

static std::vector<Hint> parse_hints(const std::string &text) {
    std::vector<Hint> out;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t cut = text.find("   ", start);
        std::string seg = cut == std::string::npos
                              ? text.substr(start)
                              : text.substr(start, cut - start);
        const size_t first = seg.find_first_not_of(' ');
        seg = first == std::string::npos ? std::string() : seg.substr(first);
        if (!seg.empty()) {
            Hint h;
            const size_t colon = seg.find(": ");
            if (colon != std::string::npos && colon <= 10) {
                h.key = seg.substr(0, colon);
                h.label = seg.substr(colon + 2);
            } else {
                h.label = seg;
            }
            out.push_back(h);
        }
        if (cut == std::string::npos) {
            break;
        }
        start = cut + 3;
    }
    return out;
}

static float layout_hints(const std::vector<Hint> &hints, float x0, float y0,
                          float width, bool draw) {
    const auto &p = pal();
    constexpr float line_h = 20.0f;
    constexpr float glyph = 15.0f;
    constexpr float gap = 12.0f;
    constexpr float key_scale = 0.27f;
    constexpr float label_scale = 0.30f;

    if (hints.empty()) {
        return 0.0f;
    }
    if (hints.size() == 1 && hints[0].key.empty()) {
        if (draw) {
            draw_text(hints[0].label, x0, y0, 0.3f, label_scale, width,
                      p.text_sec);
        }
        return text_block_height(hints[0].label, label_scale, width);
    }

    float x = x0;
    float y = y0;
    bool first = true;
    for (const Hint &h : hints) {
        const bool circle = h.key.size() == 1;
        float key_w = 0.0f;
        if (!h.key.empty()) {
            key_w = circle ? glyph : text_width(h.key, key_scale) + 12.0f;
        }
        const float label_w = text_width(h.label, label_scale);
        const float item_w = (h.key.empty() ? 0.0f : key_w + 5.0f) + label_w;
        if (!first && x + item_w > x0 + width) {
            x = x0;
            y += line_h;
        }
        if (draw) {
            if (!h.key.empty()) {
                if (circle) {
                    C2D_DrawCircleSolid(x + glyph * 0.5f, y + glyph * 0.5f,
                                        0.2f, glyph * 0.5f, p.text_dim);
                    C2D_DrawCircleSolid(x + glyph * 0.5f, y + glyph * 0.5f,
                                        0.21f, glyph * 0.5f - 1.5f, p.card);
                } else {
                    rrect(x, y, 0.2f, key_w, glyph, glyph * 0.5f, p.card_hi);
                }
                draw_text_centered(h.key, x + key_w * 0.5f,
                                   y + (glyph - 30.0f * key_scale) * 0.5f,
                                   0.3f, key_scale, p.text);
            }
            draw_text(h.label, x + (h.key.empty() ? 0.0f : key_w + 5.0f),
                      y + (glyph - 30.0f * label_scale) * 0.5f, 0.3f,
                      label_scale, 0.0f, p.text_sec);
        }
        x += item_w + gap;
        first = false;
    }
    return (y - y0) + glyph;
}

void draw_bottom(const std::string &status, const std::string &footer_hint,
                 const std::string &counter, bool show_controls) {
    const auto &p = pal();
    C2D_SceneBegin(g_ui.bottom);

    const float content_end = draw_status_panel(status);

    const std::vector<Hint> hints = parse_hints(footer_hint);
    const bool has_counter = show_controls && !counter.empty();
    const float hint_w = has_counter ? 244.0f : 288.0f;
    const float hint_h = layout_hints(hints, 16.0f, 0.0f, hint_w, false);

    if (hint_h > 0.0f || has_counter) {
        const float footer_h = std::max(hint_h, 15.0f);
        float foot_y = 228.0f - footer_h;
        if (foot_y < content_end + 12.0f) {
            foot_y = content_end + 12.0f;
        }
        C2D_DrawRectSolid(16.0f, foot_y - 9.0f, 0.1f, 288.0f, 1.0f,
                          color_alpha(p.text_sec, 40));
        layout_hints(hints, 16.0f, foot_y, hint_w, true);
        if (has_counter) {
            draw_text_right(counter, 304.0f, foot_y + 3.0f, 0.3f, 0.30f,
                            p.text_dim);
        }
    }

    draw_toast();
}

}




bool menu_ui_init() {
    if (g_ui.initialized) return true;

    for (int i = 0; i < 60 && !gspHasGpuRight(); i++) {
        gspWaitForVBlank();
    }

    g_ui.prev_top_format = gfxGetScreenFormat(GFX_TOP);
    g_ui.prev_bottom_format = gfxGetScreenFormat(GFX_BOTTOM);
    gfxSetScreenFormat(GFX_TOP, GSP_BGR8_OES);
    gfxSetScreenFormat(GFX_BOTTOM, GSP_BGR8_OES);
    gfxSetDoubleBuffering(GFX_TOP, true);
    gfxSetDoubleBuffering(GFX_BOTTOM, true);

    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE)) {
        gfxSetScreenFormat(GFX_TOP, g_ui.prev_top_format);
        gfxSetScreenFormat(GFX_BOTTOM, g_ui.prev_bottom_format);
        gfxSetDoubleBuffering(GFX_TOP, false);
        gfxSetDoubleBuffering(GFX_BOTTOM, false);
        return false;
    }
    if (!C2D_Init(C2D_DEFAULT_MAX_OBJECTS)) {
        C3D_Fini();
        gfxSetScreenFormat(GFX_TOP, g_ui.prev_top_format);
        gfxSetScreenFormat(GFX_BOTTOM, g_ui.prev_bottom_format);
        gfxSetDoubleBuffering(GFX_TOP, false);
        gfxSetDoubleBuffering(GFX_BOTTOM, false);
        return false;
    }
    C2D_Prepare();

    g_ui.top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    g_ui.bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    g_ui.text_buf = C2D_TextBufNew(16384);

    if (!g_ui.top || !g_ui.bottom || !g_ui.text_buf) {
        if (g_ui.text_buf) { C2D_TextBufDelete(g_ui.text_buf); g_ui.text_buf = nullptr; }
        if (g_ui.top) { C3D_RenderTargetDelete(g_ui.top); g_ui.top = nullptr; }
        if (g_ui.bottom) { C3D_RenderTargetDelete(g_ui.bottom); g_ui.bottom = nullptr; }
        C2D_Fini();
        C3D_Fini();
        gfxSetScreenFormat(GFX_TOP, g_ui.prev_top_format);
        gfxSetScreenFormat(GFX_BOTTOM, g_ui.prev_bottom_format);
        gfxSetDoubleBuffering(GFX_TOP, false);
        gfxSetDoubleBuffering(GFX_BOTTOM, false);
        return false;
    }

    g_ui.initialized = true;
    g_ui.frame = 0;
    g_ui.highlight_y = -1.0f;
    g_ui.last_selected = -1;
    g_ui.toast_timer = 0.0f;

    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_SceneBegin(g_ui.top);
    C2D_TargetClear(g_ui.top, pal().bg_bottom);
    draw_screen_bg(400.0f);
    C2D_SceneBegin(g_ui.bottom);
    C2D_TargetClear(g_ui.bottom, pal().bg_bottom);
    draw_screen_bg(320.0f);
    C3D_FrameEnd(0);
    gspWaitForVBlank();

    return true;
}

void menu_ui_shutdown() {
    if (!g_ui.initialized) return;

    // let the gpu finish whatever it was doing
    gspWaitForVBlank();

    if (g_ui.text_buf) { C2D_TextBufDelete(g_ui.text_buf); g_ui.text_buf = nullptr; }
    if (g_ui.top) { C3D_RenderTargetDelete(g_ui.top); g_ui.top = nullptr; }
    if (g_ui.bottom) { C3D_RenderTargetDelete(g_ui.bottom); g_ui.bottom = nullptr; }
    C2D_Fini();
    C3D_Fini();
    gfxSetScreenFormat(GFX_TOP, g_ui.prev_top_format);
    gfxSetScreenFormat(GFX_BOTTOM, g_ui.prev_bottom_format);
    gfxSetDoubleBuffering(GFX_TOP, false);
    gfxSetDoubleBuffering(GFX_BOTTOM, false);
    g_ui.initialized = false;
}

bool menu_ui_is_active() { return g_ui.initialized; }

void menu_ui_set_status(const MenuStatus &status) { g_ui.status = status; }

void menu_ui_show_toast(const std::string &message, float duration_sec) {
    g_ui.toast_msg = message;
    g_ui.toast_timer = duration_sec;
}

void menu_ui_draw_menu(const std::string &title, const std::string &subtitle,
                       const std::vector<std::string> &options, int selected,
                       const std::string &status,
                       const std::string &footer_hint,
                       const std::vector<MenuRowAction> *row_actions,
                       bool action_focused) {
    if (!begin_frame()) return;
    draw_bg();

    // top screen
    C2D_SceneBegin(g_ui.top);
    const float header_bottom = draw_top_header(title, subtitle);

    // the list
    const float list_y = header_bottom + 7.0f;
    constexpr float item_h = 40.0f;
    const int visible =
        std::max(1, std::min(4, (int)((234.0f - list_y) / item_h)));
    draw_menu_list(options, selected, list_y, item_h, visible, row_actions,
                   action_focused);

    // counter
    int n = (int)options.size();
    std::string counter = n > 0
        ? std::to_string(std::min(selected + 1, n)) + " / " + std::to_string(n)
        : "";

    // bottom screen
    draw_bottom(status, footer_hint, counter, true);
    end_frame();
}

void menu_ui_draw_message(const std::string &title, const std::string &body,
                          const std::string &status,
                          const std::string &footer_hint) {
    if (!begin_frame()) return;
    draw_bg();

    C2D_SceneBegin(g_ui.top);
    const float y = draw_top_header(title, "");
    draw_text(body, 16.0f, y + 12.0f, 0.3f, 0.38f, 368.0f, pal().text_sec);

    draw_bottom(status, footer_hint, "", false);
    end_frame();
}

void menu_ui_draw_loading(const std::string &title, const std::string &body,
                          const std::string &status,
                          const std::string &footer_hint, int frame) {
    (void)frame;
    if (!begin_frame()) return;
    draw_bg();
    const auto &p = pal();

    C2D_SceneBegin(g_ui.top);
    const float hy = draw_top_header(title, "");
    const float body_y = hy + 12.0f;
    draw_text(body, 16.0f, body_y, 0.3f, 0.38f, 368.0f, p.text_sec);

    const float bar_x = 16.0f;
    const float bar_w = 368.0f;
    const float bar_h = 6.0f;
    const float bar_y =
        std::max(104.0f, body_y + text_block_height(body, 0.38f, 368.0f) + 16.0f);
    rrect(bar_x, bar_y, 0.1f, bar_w, bar_h, 3.0f, p.card_hi);

    const float chunk_w = 110.0f;
    const float t = (float)(g_ui.frame % 90) / 90.0f;
    const float x0 = bar_x - chunk_w + t * (bar_w + chunk_w);
    const float left = std::max(x0, bar_x);
    const float right = std::min(x0 + chunk_w, bar_x + bar_w);
    if (right > left) {
        C2D_DrawRectangle(left, bar_y, 0.2f, right - left, bar_h, p.signal_dk,
                          p.signal_lt, p.signal_dk, p.signal_lt);
    }

    const int dots = (g_ui.frame / 20) % 4;
    draw_text("Loading" + std::string(dots, '.'), 16.0f, bar_y + 14.0f, 0.3f,
              0.32f, 0.0f, p.text_dim);

    draw_bottom(status, footer_hint, "", false);
    end_frame();
}

void menu_ui_draw_pairing(const std::string &title, const std::string &pin,
                          const std::string &body, const std::string &status,
                          const std::string &footer_hint, int frame) {
    if (!begin_frame()) return;
    draw_bg();
    const auto &p = pal();

    C2D_SceneBegin(g_ui.top);
    const float hy = draw_top_header(title, "");
    draw_text(body, 16.0f, hy + 10.0f, 0.3f, 0.34f, 368.0f, p.text_sec);

    // gold-edged digits
    const int digits = (int)pin.size();
    if (digits > 0) {
        constexpr float gap = 10.0f;
        const float tile_w =
            std::min(64.0f, (368.0f - gap * (float)(digits - 1)) / (float)digits);
        const float tile_h = 80.0f;
        const float total_w = tile_w * digits + gap * (digits - 1);
        const float start_x = (400.0f - total_w) * 0.5f;
        const float tile_y = 84.0f;
        const float digit_scale = 1.65f;
        const u32 edge = mix(p.card, p.spud, 0.6f);

        for (int i = 0; i < digits; i++) {
            const float x = start_x + i * (tile_w + gap);
            rrect(x, tile_y + 5.0f, 0.08f, tile_w, tile_h, 12.0f,
                  color_alpha(p.bg_bottom, 255));
            rrect(x, tile_y, 0.1f, tile_w, tile_h, 12.0f, edge);
            rrect(x + 2.0f, tile_y + 2.0f, 0.12f, tile_w - 4.0f, tile_h - 4.0f,
                  10.0f, p.card);
            const std::string d(1, pin[i]);
            draw_text_centered(d, x + tile_w * 0.5f,
                               tile_y + (tile_h - 30.0f * digit_scale) * 0.5f,
                               0.3f, digit_scale, p.spud_lt);
        }
    }

    // 90 second countdown
    const float total_s = 90.0f;
    const float left_s = std::max(0.0f, total_s - (float)frame / 60.0f);
    const float bar_y = 180.0f;
    rrect(16.0f, bar_y, 0.1f, 368.0f, 6.0f, 3.0f, p.card_hi);
    const float fill = 368.0f * (left_s / total_s);
    if (fill > 6.0f) {
        rrect_hgrad(16.0f, bar_y, 0.2f, fill, 6.0f, 3.0f,
                    mix(p.spud, p.ember, 0.35f), p.spud_lt);
    }
    char clock[32];
    snprintf(clock, sizeof(clock), "Expires in %d:%02d", (int)left_s / 60,
             (int)left_s % 60);
    draw_text("Waiting for the host", 16.0f, bar_y + 12.0f, 0.3f, 0.30f, 0.0f,
              p.text_sec);
    draw_text_right(clock, 384.0f, bar_y + 12.0f, 0.3f, 0.30f, p.spud_lt);

    draw_bottom(status, footer_hint, "", false);
    end_frame();
}

void menu_ui_draw_number_editor(const std::string &title,
                                const std::string &subtitle,
                                const std::string &value,
                                const std::string &range_hint,
                                const std::string &status,
                                const std::string &footer_hint) {
    if (!begin_frame()) return;
    draw_bg();
    const auto &p = pal();

    C2D_SceneBegin(g_ui.top);
    draw_top_header(title, subtitle);

    const float box_w = 220.0f;
    const float box_h = 56.0f;
    const float box_x = (400.0f - box_w) / 2.0f;
    const float box_y = 104.0f;
    const float cx = box_x + box_w * 0.5f;

    draw_glow(box_x, box_y, box_w, box_h, p.signal);
    rrect_hgrad(box_x, box_y, 0.15f, box_w, box_h, 12.0f, p.signal,
                p.signal_dk);
    C2D_DrawRectSolid(box_x + 12.0f, box_y + 1.0f, 0.16f, box_w - 24.0f, 1.0f,
                      color_alpha(p.white, 70));

    C2D_Text t;
    if (g_ui.text_buf && !value.empty() &&
        C2D_TextParse(&t, g_ui.text_buf, value.c_str())) {
        C2D_TextOptimize(&t);
        float tw = 0.0f, th = 0.0f;
        C2D_TextGetDimensions(&t, 0.9f, 0.9f, &tw, &th);
        C2D_DrawText(&t, C2D_WithColor, box_x + (box_w - tw) / 2.0f,
                     box_y + (box_h - th) / 2.0f, 0.3f, 0.9f, 0.9f, p.white);
    }

    // Arrows
    C2D_DrawTriangle(cx, box_y - 17.0f, p.signal_lt, cx - 9.0f, box_y - 6.0f,
                     p.signal_lt, cx + 9.0f, box_y - 6.0f, p.signal_lt, 0.2f);
    C2D_DrawTriangle(cx, box_y + box_h + 17.0f, p.signal_lt, cx - 9.0f,
                     box_y + box_h + 6.0f, p.signal_lt, cx + 9.0f,
                     box_y + box_h + 6.0f, p.signal_lt, 0.2f);

    if (!range_hint.empty()) {
        draw_text_centered(range_hint, 200.0f, box_y + box_h + 24.0f, 0.3f,
                           0.34f, p.text_sec);
    }

    draw_bottom(status, footer_hint, "", false);
    end_frame();
}

void menu_ui_draw_ip_picker(const std::string &title,
                            const std::string &subtitle,
                            const int octets[4],
                            int selected_octet,
                            const std::string &status,
                            const std::string &footer_hint) {
    if (!begin_frame()) return;
    draw_bg();
    const auto &p = pal();

    C2D_SceneBegin(g_ui.top);
    draw_top_header(title, subtitle);

    constexpr float box_w = 78.0f;
    constexpr float box_h = 58.0f;
    constexpr float gap = 14.0f;
    constexpr float lift = 5.0f;
    constexpr float num_scale = 0.95f;
    const float total_w = 4 * box_w + 3 * gap;
    const float start_x = (400.0f - total_w) / 2.0f;
    const float base_y = 100.0f;

    char buf[8];
    for (int i = 0; i < 4; i++) {
        const float x = start_x + i * (box_w + gap);
        const bool sel = (i == selected_octet);
        const float y = sel ? base_y - lift : base_y;

        if (sel) {
            draw_glow(x, y, box_w, box_h, p.signal);
            rrect_hgrad(x, y, 0.15f, box_w, box_h, 12.0f, p.signal,
                        p.signal_dk);
            C2D_DrawRectSolid(x + 12.0f, y + 1.0f, 0.16f, box_w - 24.0f, 1.0f,
                              color_alpha(p.white, 70));
        } else {
            rrect(x, y, 0.1f, box_w, box_h, 12.0f, p.card);
        }

        snprintf(buf, sizeof(buf), "%d", octets[i]);
        draw_text_centered(buf, x + box_w * 0.5f,
                           y + (box_h - 30.0f * num_scale) * 0.5f, 0.3f,
                           num_scale, sel ? p.white : p.text_sec);

        if (i < 3) {
            C2D_DrawCircleSolid(x + box_w + gap * 0.5f, base_y + box_h * 0.5f,
                                0.2f, 2.0f, p.text_dim);
        }
    }

    // Arrows (octet being edited)
    const float sx = start_x + selected_octet * (box_w + gap) + box_w * 0.5f;
    const float top_y = base_y - lift;
    C2D_DrawTriangle(sx, top_y - 17.0f, p.signal_lt, sx - 8.0f, top_y - 7.0f,
                     p.signal_lt, sx + 8.0f, top_y - 7.0f, p.signal_lt, 0.2f);
    C2D_DrawTriangle(sx, top_y + box_h + 17.0f, p.signal_lt, sx - 8.0f,
                     top_y + box_h + 7.0f, p.signal_lt, sx + 8.0f,
                     top_y + box_h + 7.0f, p.signal_lt, 0.2f);

    const char *octet_names[] = {"1st octet", "2nd octet", "3rd octet",
                                 "4th octet"};
    const int named = std::max(0, std::min(3, selected_octet));
    draw_text("Editing: " + std::string(octet_names[named]), start_x,
              base_y + box_h + 24.0f, 0.3f, 0.34f, 0.0f, p.text_sec);
    draw_text_right("Range: 0 - 255", start_x + total_w, base_y + box_h + 24.0f,
                    0.3f, 0.34f, p.text_dim);

    draw_bottom(status, footer_hint, "", false);
    end_frame();
}
