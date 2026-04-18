#include "menu_ui.hpp"

#include <3ds.h>
#include <citro2d.h>

#include <algorithm>
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
};

MenuUiState g_ui;

constexpr u32 TOP_BG = C2D_Color32(12, 16, 24, 255);
constexpr u32 TOP_BAND = C2D_Color32(22, 31, 46, 255);
constexpr u32 TOP_ACCENT = C2D_Color32(241, 165, 74, 255);
constexpr u32 TOP_ACCENT_SOFT = C2D_Color32(166, 108, 39, 255);
constexpr u32 CARD = C2D_Color32(26, 35, 51, 255);
constexpr u32 CARD_ALT = C2D_Color32(18, 24, 36, 255);
constexpr u32 CARD_SELECTED = C2D_Color32(242, 177, 89, 255);
constexpr u32 CARD_SELECTED_TEXT = C2D_Color32(34, 23, 12, 255);
constexpr u32 TEXT_PRIMARY = C2D_Color32(245, 243, 238, 255);
constexpr u32 TEXT_MUTED = C2D_Color32(168, 180, 194, 255);
constexpr u32 TEXT_DIM = C2D_Color32(120, 132, 148, 255);
constexpr u32 BOTTOM_BG = C2D_Color32(15, 21, 31, 255);
constexpr u32 CHIP = C2D_Color32(34, 45, 63, 255);

void draw_wrapped_text(const std::string &text, float x, float y, float z,
                       float scale, float width, u32 color, u32 flags = 0) {
    if (!g_ui.initialized || text.empty()) {
        return;
    }

    C2D_Text drawable;
    C2D_TextParse(&drawable, g_ui.text_buf, text.c_str());
    C2D_TextOptimize(&drawable);

    u32 draw_flags = flags | C2D_WithColor;
    if (width > 0.0f) {
        draw_flags |= C2D_WordWrap;
        C2D_DrawText(&drawable, draw_flags, x, y, z, scale, scale, color,
                     width);
    } else {
        C2D_DrawText(&drawable, draw_flags, x, y, z, scale, scale, color);
    }
}

void draw_chip(float x, float y, float w, float h, const std::string &text,
               u32 bg, u32 fg) {
    C2D_DrawRectSolid(x, y, 0.1f, w, h, bg);
    draw_wrapped_text(text, x + 8.0f, y + 6.0f, 0.2f, 0.4f, w - 16.0f, fg);
}

void draw_loading_indicator(float x, float y, int frame) {
    constexpr int bar_count = 5;
    const int active_bar = frame % bar_count;
    for (int i = 0; i < bar_count; i++) {
        const float bar_x = x + (i * 18.0f);
        const float bar_h = (i == active_bar) ? 30.0f : 16.0f;
        const float bar_y = y + (30.0f - bar_h);
        const u32 color = (i == active_bar) ? TOP_ACCENT : TOP_ACCENT_SOFT;
        C2D_DrawRectSolid(bar_x, bar_y, 0.2f, 10.0f, bar_h, color);
    }
}

void draw_background() {
    C2D_TargetClear(g_ui.top, TOP_BG);
    C2D_TargetClear(g_ui.bottom, BOTTOM_BG);

    C2D_SceneBegin(g_ui.top);
    C2D_DrawRectSolid(0.0f, 0.0f, 0.0f, 400.0f, 52.0f, TOP_BAND);
    C2D_DrawRectSolid(0.0f, 52.0f, 0.0f, 400.0f, 6.0f, TOP_ACCENT);
    C2D_DrawRectSolid(284.0f, 0.0f, 0.0f, 116.0f, 240.0f, CARD_ALT);
    C2D_DrawRectSolid(296.0f, 18.0f, 0.0f, 80.0f, 80.0f, TOP_ACCENT_SOFT);
    C2D_DrawRectSolid(314.0f, 38.0f, 0.0f, 44.0f, 44.0f, TOP_ACCENT);

    draw_wrapped_text("STREAMPOTATO", 18.0f, 14.0f, 0.2f, 0.72f, 220.0f,
                      TOP_ACCENT);
    draw_wrapped_text("old 3DS / 2DS streaming build", 19.0f, 35.0f, 0.2f,
                      0.34f, 210.0f, TEXT_MUTED);

    C2D_SceneBegin(g_ui.bottom);
    C2D_DrawRectSolid(0.0f, 0.0f, 0.0f, 320.0f, 240.0f, BOTTOM_BG);
    C2D_DrawRectSolid(0.0f, 0.0f, 0.0f, 320.0f, 5.0f, TOP_ACCENT);
    C2D_DrawRectSolid(18.0f, 20.0f, 0.0f, 284.0f, 92.0f, CARD);
    C2D_DrawRectSolid(18.0f, 128.0f, 0.0f, 284.0f, 94.0f, CARD_ALT);
}

void begin_frame() {
    C2D_TextBufClear(g_ui.text_buf);
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    draw_background();
}

void end_frame() { C3D_FrameEnd(0); }

} // namespace

bool menu_ui_init() {
    if (g_ui.initialized) {
        return true;
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
    g_ui.text_buf = C2D_TextBufNew(8192);
    if (g_ui.top == nullptr || g_ui.bottom == nullptr ||
        g_ui.text_buf == nullptr) {
        if (g_ui.text_buf != nullptr) {
            C2D_TextBufDelete(g_ui.text_buf);
            g_ui.text_buf = nullptr;
        }
        if (g_ui.top != nullptr) {
            C3D_RenderTargetDelete(g_ui.top);
            g_ui.top = nullptr;
        }
        if (g_ui.bottom != nullptr) {
            C3D_RenderTargetDelete(g_ui.bottom);
            g_ui.bottom = nullptr;
        }
        C2D_Fini();
        C3D_Fini();
        gfxSetScreenFormat(GFX_TOP, g_ui.prev_top_format);
        gfxSetScreenFormat(GFX_BOTTOM, g_ui.prev_bottom_format);
        gfxSetDoubleBuffering(GFX_TOP, false);
        gfxSetDoubleBuffering(GFX_BOTTOM, false);
        return false;
    }

    g_ui.initialized = true;
    return true;
}

void menu_ui_shutdown() {
    if (!g_ui.initialized) {
        return;
    }

    if (g_ui.text_buf != nullptr) {
        C2D_TextBufDelete(g_ui.text_buf);
        g_ui.text_buf = nullptr;
    }
    if (g_ui.top != nullptr) {
        C3D_RenderTargetDelete(g_ui.top);
        g_ui.top = nullptr;
    }
    if (g_ui.bottom != nullptr) {
        C3D_RenderTargetDelete(g_ui.bottom);
        g_ui.bottom = nullptr;
    }

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
    if (!g_ui.initialized) {
        return;
    }

    begin_frame();

    C2D_SceneBegin(g_ui.top);
    draw_wrapped_text(title, 18.0f, 72.0f, 0.2f, 0.56f, 248.0f, TEXT_PRIMARY);
    draw_wrapped_text(subtitle, 18.0f, 100.0f, 0.2f, 0.34f, 248.0f,
                      TEXT_MUTED);

    const int visible_count = 5;
    const int item_count = static_cast<int>(options.size());
    const int first_visible =
        std::max(0, std::min(selected - 2, std::max(0, item_count - visible_count)));

    float y = 132.0f;
    for (int i = 0; i < visible_count && first_visible + i < item_count; i++) {
        const int option_index = first_visible + i;
        const bool is_selected = option_index == selected;
        C2D_DrawRectSolid(18.0f, y, 0.1f, 250.0f, 18.0f,
                          is_selected ? CARD_SELECTED : CARD);
        if (is_selected) {
            C2D_DrawRectSolid(18.0f, y, 0.2f, 6.0f, 18.0f, TOP_BG);
        }

        std::string label = std::to_string(option_index + 1) + ". " +
                            options[option_index];
        draw_wrapped_text(label, 30.0f, y + 3.0f, 0.2f, 0.38f, 226.0f,
                          is_selected ? CARD_SELECTED_TEXT : TEXT_PRIMARY);
        y += 20.0f;
    }

    std::string counter = std::to_string(std::min(selected + 1, item_count)) +
                          " / " + std::to_string(item_count);
    draw_chip(296.0f, 124.0f, 78.0f, 24.0f, counter, CHIP, TEXT_PRIMARY);
    draw_wrapped_text("D-Pad  Select", 296.0f, 160.0f, 0.2f, 0.34f, 82.0f,
                      TEXT_MUTED);
    draw_wrapped_text("A  Confirm", 296.0f, 184.0f, 0.2f, 0.34f, 82.0f,
                      TEXT_MUTED);
    draw_wrapped_text("B  Back", 296.0f, 204.0f, 0.2f, 0.34f, 82.0f,
                      TEXT_MUTED);

    C2D_SceneBegin(g_ui.bottom);
    draw_wrapped_text("STATUS", 30.0f, 30.0f, 0.2f, 0.36f, 110.0f, TOP_ACCENT);
    draw_wrapped_text(status, 30.0f, 52.0f, 0.2f, 0.36f, 260.0f,
                      TEXT_PRIMARY);
    draw_wrapped_text("CONTROLS", 30.0f, 138.0f, 0.2f, 0.36f, 110.0f,
                      TOP_ACCENT);
    draw_wrapped_text(footer_hint, 30.0f, 160.0f, 0.2f, 0.34f, 260.0f,
                      TEXT_MUTED);

    end_frame();
}

void menu_ui_draw_message(const std::string &title, const std::string &body,
                          const std::string &status,
                          const std::string &footer_hint) {
    if (!g_ui.initialized) {
        return;
    }

    begin_frame();

    C2D_SceneBegin(g_ui.top);
    C2D_DrawRectSolid(20.0f, 78.0f, 0.1f, 252.0f, 110.0f, CARD);
    draw_wrapped_text(title, 34.0f, 94.0f, 0.2f, 0.56f, 224.0f, TEXT_PRIMARY);
    draw_wrapped_text(body, 34.0f, 126.0f, 0.2f, 0.36f, 224.0f, TEXT_MUTED);

    C2D_SceneBegin(g_ui.bottom);
    draw_wrapped_text("DETAILS", 30.0f, 30.0f, 0.2f, 0.36f, 110.0f, TOP_ACCENT);
    draw_wrapped_text(status, 30.0f, 52.0f, 0.2f, 0.36f, 260.0f,
                      TEXT_PRIMARY);
    draw_wrapped_text("INPUT", 30.0f, 138.0f, 0.2f, 0.36f, 110.0f, TOP_ACCENT);
    draw_wrapped_text(footer_hint, 30.0f, 160.0f, 0.2f, 0.34f, 260.0f,
                      TEXT_MUTED);

    end_frame();
}

void menu_ui_draw_loading(const std::string &title, const std::string &body,
                          const std::string &status,
                          const std::string &footer_hint, int frame) {
    if (!g_ui.initialized) {
        return;
    }

    begin_frame();

    C2D_SceneBegin(g_ui.top);
    C2D_DrawRectSolid(20.0f, 78.0f, 0.1f, 252.0f, 116.0f, CARD);
    draw_wrapped_text(title, 34.0f, 92.0f, 0.2f, 0.56f, 224.0f, TEXT_PRIMARY);
    draw_wrapped_text(body, 34.0f, 122.0f, 0.2f, 0.36f, 224.0f, TEXT_MUTED);
    draw_loading_indicator(110.0f, 156.0f, frame);

    C2D_SceneBegin(g_ui.bottom);
    draw_wrapped_text("STATUS", 30.0f, 30.0f, 0.2f, 0.36f, 110.0f, TOP_ACCENT);
    draw_wrapped_text(status, 30.0f, 52.0f, 0.2f, 0.36f, 260.0f,
                      TEXT_PRIMARY);
    draw_wrapped_text("TASK", 30.0f, 138.0f, 0.2f, 0.36f, 110.0f, TOP_ACCENT);
    draw_wrapped_text(footer_hint, 30.0f, 160.0f, 0.2f, 0.34f, 260.0f,
                      TEXT_MUTED);

    end_frame();
}

void menu_ui_draw_pairing(const std::string &title, const std::string &pin,
                          const std::string &body, const std::string &status,
                          const std::string &footer_hint, int frame) {
    if (!g_ui.initialized) {
        return;
    }

    begin_frame();

    C2D_SceneBegin(g_ui.top);
    C2D_DrawRectSolid(18.0f, 72.0f, 0.1f, 258.0f, 126.0f, CARD);
    draw_wrapped_text(title, 34.0f, 86.0f, 0.2f, 0.54f, 224.0f, TEXT_PRIMARY);
    draw_wrapped_text(body, 34.0f, 112.0f, 0.2f, 0.34f, 224.0f, TEXT_MUTED);
    C2D_DrawRectSolid(34.0f, 138.0f, 0.2f, 154.0f, 38.0f, TOP_ACCENT);
    draw_wrapped_text(pin, 72.0f, 146.0f, 0.3f, 0.88f, 100.0f,
                      CARD_SELECTED_TEXT);
    draw_loading_indicator(220.0f, 146.0f, frame);

    C2D_SceneBegin(g_ui.bottom);
    draw_wrapped_text("STATUS", 30.0f, 30.0f, 0.2f, 0.36f, 110.0f, TOP_ACCENT);
    draw_wrapped_text(status, 30.0f, 52.0f, 0.2f, 0.36f, 260.0f,
                      TEXT_PRIMARY);
    draw_wrapped_text("PAIRING", 30.0f, 138.0f, 0.2f, 0.36f, 110.0f,
                      TOP_ACCENT);
    draw_wrapped_text(footer_hint, 30.0f, 160.0f, 0.2f, 0.34f, 260.0f,
                      TEXT_MUTED);

    end_frame();
}
