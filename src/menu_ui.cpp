/*
 * StreamPotato UI
 */

#include "menu_ui.hpp"

#include <3ds.h>
#include <citro2d.h>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

struct MenuEnhancedItem {
    std::string label;
    bool is_toggle = false;
    bool toggle_value = false;
};

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
};

static MenuUiState g_ui;

struct Palette {
    u32 bg;            // screen background
    u32 highlight;     // selection bar
    u32 highlight_dim; // scroll hint / partial highlight
    u32 text;          // primary text
    u32 text_sec;      // secondary / muted text
    u32 text_dim;      // dim / disabled text
    u32 separator;     // thin divider lines
    u32 accent_line;   // top accent stripe
    u32 toast_bg;      // toast background
};

static const Palette &pal() {
    static const Palette p = {
        C2D_Color32(18, 18, 30, 255),   // bg
        C2D_Color32(50, 120, 240, 255),  // highlight — solid blue
        C2D_Color32(30, 70, 140, 255),   // highlight_dim
        C2D_Color32(225, 230, 240, 255), // text
        C2D_Color32(150, 160, 180, 255), // text_sec
        C2D_Color32(90, 100, 120, 255),  // text_dim
        C2D_Color32(35, 40, 55, 255),    // separator
        C2D_Color32(50, 120, 240, 255),  // accent_line
        C2D_Color32(35, 40, 55, 230),    // toast_bg
    };
    return p;
}

// Replace the alpha channel of a C2D_Color32 value
static inline u32 color_alpha(u32 color, u8 alpha) {
    return (color & 0x00FFFFFFu) | ((u32)alpha << 24);
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

// Detect "label: enabled/disabled" pattern for state indicators
struct ToggleHint {
    bool is_toggle;
    bool is_on;
    std::string label;
};

static ToggleHint detect_toggle(const std::string &s) {
    ToggleHint h = {false, false, s};
    auto pos_en = s.rfind("enabled");
    auto pos_dis = s.rfind("disabled");
    size_t pos = std::string::npos;
    if (pos_en != std::string::npos) { pos = pos_en; h.is_on = true; }
    if (pos_dis != std::string::npos) { pos = pos_dis; h.is_on = false; }
    if (pos == std::string::npos) return h;
    h.is_toggle = true;
    h.label = s.substr(0, pos);
    while (!h.label.empty() && (h.label.back() == ' ' || h.label.back() == ':'))
        h.label.pop_back();
    return h;
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

void draw_toast() {
    if (g_ui.toast_timer <= 0.0f) return;
    float a = g_ui.toast_timer < 0.4f ? g_ui.toast_timer / 0.4f : 1.0f;
    u8 alpha = (u8)(255.0f * a);
    float tw = 288.0f, th = 22.0f, tx = 16.0f, ty = 214.0f;

    // Background
    C2D_DrawRectSolid(tx, ty, 0.5f, tw, th,
                      color_alpha(pal().toast_bg, alpha));
    // Accent line
    C2D_DrawRectSolid(tx, ty, 0.6f, tw, 2.0f,
                      color_alpha(pal().accent_line, alpha));
    // Text
    draw_text(g_ui.toast_msg, tx + 8.0f, ty + 5.0f, 0.5f, 0.32f,
              tw - 16.0f, color_alpha(pal().text, alpha));
}

void draw_top_header(const std::string &title, const std::string &subtitle) {
    // Title
    draw_text(title, 16.0f, 14.0f, 0.1f, 0.6f, 368.0f, pal().text);
    // Subtitle (muted, smaller)
    if (!subtitle.empty()) {
        draw_text(subtitle, 16.0f, 38.0f, 0.1f, 0.32f, 368.0f, pal().text_sec);
    }
    // Separator
    float sep_y = subtitle.empty() ? 36.0f : 58.0f;
    C2D_DrawRectSolid(16.0f, sep_y, 0.1f, 368.0f, 1.0f, pal().separator);
}

void draw_menu_list(const std::vector<std::string> &options, int selected,
                     float start_y, float item_h, int visible_count) {
    const auto &p = pal();
    const int n = (int)options.size();
    const int first = std::max(0, std::min(selected - 1, std::max(0, n - visible_count)));

    // Smooth highlight
    int local = selected - first;
    float target_y = start_y + local * item_h;
    if (g_ui.last_selected != selected || g_ui.highlight_y < 0.0f) {
        g_ui.highlight_y = target_y;
        g_ui.last_selected = selected;
    }
    g_ui.highlight_y += (target_y - g_ui.highlight_y) * 0.30f;

    for (int i = 0; i < visible_count && first + i < n; i++) {
        int idx = first + i;
        float y = start_y + i * item_h;
        bool sel = idx == selected;
        const std::string &opt = options[idx];

        // Selection bar (full width, SNES9x style)
        if (sel) {
            C2D_DrawRectSolid(8.0f, g_ui.highlight_y, 0.1f, 384.0f, item_h, p.highlight);
        }

        // Detect toggle for state indicator
        ToggleHint th = detect_toggle(opt);
        bool is_exp = opt.rfind("Experimental ", 0) == 0;

        if (th.is_toggle && !is_exp) {
            // Label
            draw_text(th.label, 20.0f, y + 4.0f, 0.2f, 0.40f, 280.0f,
                      sel ? C2D_Color32(255, 255, 255, 255) : p.text);
            // State text right-aligned
            std::string state = th.is_on ? "ON" : "OFF";
            u32 state_color = th.is_on ? p.highlight : p.text_dim;
            draw_text(state, 340.0f, y + 4.0f, 0.2f, 0.35f, 40.0f, state_color);
        } else if (is_exp) {
            // "EXP" tag + remaining text
            C2D_DrawRectSolid(20.0f, y + 5.0f, 0.2f, 28.0f, 14.0f, p.highlight_dim);
            draw_text("EXP", 24.0f, y + 7.0f, 0.3f, 0.25f, 24.0f,
                      C2D_Color32(255, 255, 255, 255));
            draw_text(opt.substr(13), 54.0f, y + 4.0f, 0.2f, 0.40f, 310.0f,
                      sel ? C2D_Color32(255, 255, 255, 255) : p.text);
            // Toggle state if applicable
            if (th.is_toggle) {
                std::string st = th.is_on ? "ON" : "OFF";
                draw_text(st, 340.0f, y + 4.0f, 0.2f, 0.35f, 40.0f,
                          th.is_on ? p.highlight : p.text_dim);
            }
        } else {
            // Plain item
            draw_text(opt, 20.0f, y + 4.0f, 0.2f, 0.40f, 348.0f,
                      sel ? C2D_Color32(255, 255, 255, 255) : p.text);
        }
    }

    // Scrollbar (thin, right edge)
    if (n > visible_count) {
        float track_h = visible_count * item_h;
        C2D_DrawRectSolid(394.0f, start_y, 0.3f, 2.0f, track_h, p.separator);
        float thumb_h = std::max(8.0f, track_h * (float)visible_count / (float)n);
        float thumb_y = start_y + (track_h - thumb_h) * (float)first / (float)(n - visible_count);
        C2D_DrawRectSolid(394.0f, thumb_y, 0.4f, 2.0f, thumb_h, p.text_dim);
    }
}

void draw_bottom(const std::string &status, const std::string &footer_hint,
                 const std::string &counter, bool show_controls) {
    C2D_SceneBegin(g_ui.bottom);
    C2D_DrawRectSolid(0.0f, 0.0f, 0.0f, 320.0f, 240.0f, pal().bg);
    // Accent line
    C2D_DrawRectSolid(0.0f, 0.0f, 0.1f, 320.0f, 2.0f, pal().accent_line);

    // Status section
    draw_text("STATUS", 16.0f, 14.0f, 0.2f, 0.32f, 100.0f, pal().highlight);
    draw_text(status, 16.0f, 34.0f, 0.2f, 0.36f, 288.0f, pal().text);

    // Separator
    C2D_DrawRectSolid(16.0f, 100.0f, 0.1f, 288.0f, 1.0f, pal().separator);

    if (show_controls && !counter.empty()) {
        // Counter
        draw_text("SELECTED  " + counter, 16.0f, 114.0f, 0.2f, 0.32f,
                  200.0f, pal().text_dim);
    }

    // Controls hints
    if (!footer_hint.empty()) {
        float hint_y = show_controls ? 140.0f : 114.0f;
        draw_text(footer_hint, 16.0f, hint_y, 0.2f, 0.30f, 288.0f, pal().text_sec);
    }

    draw_toast();
}

void draw_bg() {
    C2D_TargetClear(g_ui.top, pal().bg);
    C2D_TargetClear(g_ui.bottom, pal().bg);
    // Top screen: flat bg + thin accent line at very top
    C2D_SceneBegin(g_ui.top);
    C2D_DrawRectSolid(0.0f, 0.0f, 0.0f, 400.0f, 240.0f, pal().bg);
    C2D_DrawRectSolid(0.0f, 0.0f, 0.1f, 400.0f, 2.0f, pal().accent_line);
    // Bottom screen: flat bg + accent line
    C2D_SceneBegin(g_ui.bottom);
    C2D_DrawRectSolid(0.0f, 0.0f, 0.0f, 320.0f, 240.0f, pal().bg);
    C2D_DrawRectSolid(0.0f, 0.0f, 0.1f, 320.0f, 2.0f, pal().accent_line);
}

} // namespace

bool menu_ui_init() {
    if (g_ui.initialized) return true;

    // Wait for GPU rights — the software keyboard or other system applets
    // may still hold the GPU when control returns to us.
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
    C2D_TargetClear(g_ui.top, pal().bg);
    C2D_DrawRectSolid(0.0f, 0.0f, 0.0f, 400.0f, 240.0f, pal().bg);
    C2D_SceneBegin(g_ui.bottom);
    C2D_TargetClear(g_ui.bottom, pal().bg);
    C2D_DrawRectSolid(0.0f, 0.0f, 0.0f, 320.0f, 240.0f, pal().bg);
    C3D_FrameEnd(0);
    gspWaitForVBlank();

    return true;
}

void menu_ui_shutdown() {
    if (!g_ui.initialized) return;

    // Let the GPU finish whatever it was doing before we tear down.
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

void menu_ui_draw_menu(const std::string &title, const std::string &subtitle,
                       const std::vector<std::string> &options, int selected,
                       const std::string &status,
                       const std::string &footer_hint) {
    if (!begin_frame()) return;
    draw_bg();

    // Top screen
    C2D_SceneBegin(g_ui.top);
    draw_top_header(title, subtitle);

    constexpr float list_y = 68.0f;
    constexpr float item_h = 38.0f;
    constexpr int visible = 4;
    draw_menu_list(options, selected, list_y, item_h, visible);

    // Counter
    int n = (int)options.size();
    std::string counter = n > 0
        ? std::to_string(std::min(selected + 1, n)) + " / " + std::to_string(n)
        : "";

    // Bottom screen
    draw_bottom(status, footer_hint, counter, true);
    end_frame();
}

void menu_ui_draw_menu_enhanced(const std::string &title,
                                const std::string &subtitle,
                                const std::vector<MenuEnhancedItem> &items,
                                int selected, const std::string &status,
                                const std::string &footer_hint) {
    if (!begin_frame()) return;
    draw_bg();

    C2D_SceneBegin(g_ui.top);
    draw_top_header(title, subtitle);

    const auto &p = pal();
    const int n = (int)items.size();
    constexpr float list_y = 68.0f;
    constexpr float item_h = 38.0f;
    constexpr int visible = 4;
    const int first = std::max(0, std::min(selected - 1, std::max(0, n - visible)));

    // Smooth highlight
    int local = selected - first;
    float target_y = list_y + local * item_h;
    if (g_ui.last_selected != selected || g_ui.highlight_y < 0.0f) {
        g_ui.highlight_y = target_y;
        g_ui.last_selected = selected;
    }
    g_ui.highlight_y += (target_y - g_ui.highlight_y) * 0.30f;

    for (int i = 0; i < visible && first + i < n; i++) {
        int idx = first + i;
        float y = list_y + i * item_h;
        bool sel = idx == selected;
        const auto &item = items[idx];
        bool is_exp = item.label.rfind("Experimental ", 0) == 0;

        if (sel) {
            C2D_DrawRectSolid(8.0f, g_ui.highlight_y, 0.1f, 384.0f, item_h, p.highlight);
        }

        if (item.is_toggle) {
            draw_text(item.label, 20.0f, y + 4.0f, 0.2f, 0.40f, 280.0f,
                      sel ? C2D_Color32(255, 255, 255, 255) : p.text);
            std::string st = item.toggle_value ? "ON" : "OFF";
            draw_text(st, 340.0f, y + 4.0f, 0.2f, 0.35f, 40.0f,
                      item.toggle_value ? C2D_Color32(255, 255, 255, 255) : p.text_dim);
        } else if (is_exp) {
            C2D_DrawRectSolid(20.0f, y + 5.0f, 0.2f, 28.0f, 14.0f, p.highlight_dim);
            draw_text("EXP", 24.0f, y + 7.0f, 0.3f, 0.25f, 24.0f,
                      C2D_Color32(255, 255, 255, 255));
            draw_text(item.label.substr(13), 54.0f, y + 4.0f, 0.2f, 0.40f, 310.0f,
                      sel ? C2D_Color32(255, 255, 255, 255) : p.text);
        } else {
            draw_text(item.label, 20.0f, y + 4.0f, 0.2f, 0.40f, 348.0f,
                      sel ? C2D_Color32(255, 255, 255, 255) : p.text);
        }
    }

    // Scrollbar
    if (n > visible) {
        float track_h = visible * item_h;
        C2D_DrawRectSolid(394.0f, list_y, 0.3f, 2.0f, track_h, p.separator);
        float thumb_h = std::max(8.0f, track_h * (float)visible / (float)n);
        float thumb_y = list_y + (track_h - thumb_h) * (float)first / (float)(n - visible);
        C2D_DrawRectSolid(394.0f, thumb_y, 0.4f, 2.0f, thumb_h, p.text_dim);
    }

    std::string counter = n > 0
        ? std::to_string(std::min(selected + 1, n)) + " / " + std::to_string(n)
        : "";
    draw_bottom(status, footer_hint, counter, true);
    end_frame();
}

void menu_ui_draw_message(const std::string &title, const std::string &body,
                          const std::string &status,
                          const std::string &footer_hint) {
    if (!begin_frame()) return;
    draw_bg();

    C2D_SceneBegin(g_ui.top);
    draw_top_header(title, "");
    draw_text(body, 16.0f, 48.0f, 0.2f, 0.38f, 368.0f, pal().text_sec);

    draw_bottom(status, footer_hint, "", false);
    end_frame();
}

void menu_ui_draw_loading(const std::string &title, const std::string &body,
                          const std::string &status,
                          const std::string &footer_hint, int frame) {
    if (!begin_frame()) return;
    draw_bg();

    C2D_SceneBegin(g_ui.top);
    draw_top_header(title, "");
    draw_text(body, 16.0f, 48.0f, 0.2f, 0.38f, 368.0f, pal().text_sec);

    // Simple animated progress bar (SNES9x style — thin, no glow)
    float bar_x = 16.0f, bar_y = 100.0f, bar_w = 368.0f, bar_h = 3.0f;
    C2D_DrawRectSolid(bar_x, bar_y, 0.2f, bar_w, bar_h, pal().separator);
    float progress = ((g_ui.frame % 60) / 60.0f) * bar_w;
    C2D_DrawRectSolid(bar_x, bar_y, 0.3f, progress, bar_h, pal().highlight);

    // Simple "..." cycling text
    int dots = (g_ui.frame / 20) % 4;
    std::string loading_text = "Loading" + std::string(dots, '.');
    draw_text(loading_text, 16.0f, 112.0f, 0.2f, 0.32f, 100.0f, pal().text_dim);

    draw_bottom(status, footer_hint, "", false);
    end_frame();
}

void menu_ui_draw_pairing(const std::string &title, const std::string &pin,
                          const std::string &body, const std::string &status,
                          const std::string &footer_hint, int frame) {
    if (!begin_frame()) return;
    draw_bg();

    C2D_SceneBegin(g_ui.top);
    draw_top_header(title, "");
    draw_text(body, 16.0f, 48.0f, 0.2f, 0.34f, 368.0f, pal().text_sec);

    // PIN display — large, centered, clean (no box, no glow — just text)
    C2D_Text pin_t;
    if (g_ui.text_buf && C2D_TextParse(&pin_t, g_ui.text_buf, pin.c_str())) {
        C2D_TextOptimize(&pin_t);
        float pw = 0.0f;
        C2D_TextGetDimensions(&pin_t, 1.0f, 1.0f, &pw, nullptr);
        float px = (400.0f - pw) / 2.0f;
        C2D_DrawText(&pin_t, C2D_WithColor, px, 90.0f, 0.2f, 1.0f, 1.0f, pal().text);
    }

    // Simple bar underneath
    float bar_y = 140.0f;
    C2D_DrawRectSolid(16.0f, bar_y, 0.2f, 368.0f, 3.0f, pal().separator);
    float progress = ((g_ui.frame % 60) / 60.0f) * 368.0f;
    C2D_DrawRectSolid(16.0f, bar_y, 0.3f, progress, 3.0f, pal().highlight);

    // Hint
    draw_text("Enter this PIN on the host PC", 16.0f, 154.0f, 0.2f, 0.32f,
              250.0f, pal().text_dim);

    draw_bottom(status, footer_hint, "", false);
    end_frame();
}


void menu_ui_show_toast(const std::string &message, float duration_sec) {
    g_ui.toast_msg = message;
    g_ui.toast_timer = duration_sec;
}

void menu_ui_draw_number_editor(const std::string &title,
                                const std::string &subtitle,
                                const std::string &value,
                                const std::string &range_hint,
                                const std::string &status,
                                const std::string &footer_hint) {
    if (!begin_frame()) return;
    draw_bg();

    C2D_SceneBegin(g_ui.top);
    draw_top_header(title, subtitle);

    const auto &p = pal();

    const float box_w = 220.0f;
    const float box_h = 56.0f;
    const float box_x = (400.0f - box_w) / 2.0f;
    const float box_y = 96.0f;

    C2D_DrawRectSolid(box_x, box_y, 0.2f, box_w, box_h, p.highlight);

    C2D_Text t;
    if (g_ui.text_buf && !value.empty() &&
        C2D_TextParse(&t, g_ui.text_buf, value.c_str())) {
        C2D_TextOptimize(&t);
        float tw = 0.0f, th = 0.0f;
        C2D_TextGetDimensions(&t, 0.9f, 0.9f, &tw, &th);
        float tx = box_x + (box_w - tw) / 2.0f;
        float ty = box_y + (box_h - th) / 2.0f;
        C2D_DrawText(&t, C2D_WithColor, tx, ty, 0.3f, 0.9f, 0.9f,
                     C2D_Color32(255, 255, 255, 255));
    }

    draw_text("+", box_x + box_w / 2.0f - 4.0f, box_y - 18.0f, 0.3f, 0.45f,
              16.0f, p.highlight);
    draw_text("-", box_x + box_w / 2.0f - 4.0f, box_y + box_h + 2.0f, 0.3f,
              0.45f, 16.0f, p.highlight);

    if (!range_hint.empty()) {
        draw_text(range_hint, 16.0f, 175.0f, 0.2f, 0.34f, 368.0f, p.text_sec);
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

    C2D_SceneBegin(g_ui.top);
    draw_top_header(title, subtitle);

    const auto &p = pal();

    const float box_w = 70.0f;
    const float box_h = 50.0f;
    const float gap = 16.0f;
    const float total_w = 4 * box_w + 3 * gap;
    const float start_x = (400.0f - total_w) / 2.0f;
    const float box_y = 100.0f;

    char buf[8];
    for (int i = 0; i < 4; i++) {
        float x = start_x + i * (box_w + gap);
        bool sel = (i == selected_octet);

        u32 box_bg = sel ? p.highlight : p.highlight_dim;
        C2D_DrawRectSolid(x, box_y, 0.2f, box_w, box_h, box_bg);

        snprintf(buf, sizeof(buf), "%d", octets[i]);
        C2D_Text t;
        if (g_ui.text_buf && C2D_TextParse(&t, g_ui.text_buf, buf)) {
            C2D_TextOptimize(&t);
            float tw = 0.0f, th = 0.0f;
            C2D_TextGetDimensions(&t, 0.9f, 0.9f, &tw, &th);
            float tx = x + (box_w - tw) / 2.0f;
            float ty = box_y + (box_h - th) / 2.0f;
            C2D_DrawText(&t, C2D_WithColor, tx, ty, 0.3f, 0.9f, 0.9f,
                         C2D_Color32(255, 255, 255, 255));
        }

        if (i < 3) {
            float dot_x = x + box_w + gap / 2.0f - 4.0f;
            draw_text(".", dot_x, box_y + 12.0f, 0.3f, 0.8f, 16.0f, p.text_sec);
        }
    }

    float sel_x = start_x + selected_octet * (box_w + gap);
    draw_text("+", sel_x + box_w / 2.0f - 4.0f, box_y - 16.0f, 0.3f, 0.45f,
              16.0f, p.highlight);
    draw_text("-", sel_x + box_w / 2.0f - 4.0f, box_y + box_h + 2.0f, 0.3f,
              0.45f, 16.0f, p.highlight);

    const char *octet_names[] = {"1st octet", "2nd octet", "3rd octet",
                                 "4th octet"};
    std::string hint = "Editing: " + std::string(octet_names[selected_octet]);
    draw_text(hint, 16.0f, 175.0f, 0.2f, 0.34f, 368.0f, p.text_sec);

    draw_bottom(status, footer_hint, "", false);
    end_frame();
}