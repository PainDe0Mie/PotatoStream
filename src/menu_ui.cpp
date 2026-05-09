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

constexpr u32 TOP_BG = C2D_Color32(8, 12, 20, 255);
constexpr u32 TOP_BAND = C2D_Color32(15, 23, 36, 255);
constexpr u32 TOP_ACCENT = C2D_Color32(255, 179, 64, 255);
constexpr u32 TOP_ACCENT_SOFT = C2D_Color32(180, 112, 38, 255);
constexpr u32 CARD = C2D_Color32(24, 34, 51, 255);
constexpr u32 CARD_ALT = C2D_Color32(17, 25, 39, 255);
constexpr u32 CARD_EDGE = C2D_Color32(47, 62, 86, 255);
constexpr u32 CARD_SELECTED = C2D_Color32(255, 187, 83, 255);
constexpr u32 CARD_SELECTED_TEXT = C2D_Color32(26, 17, 8, 255);
constexpr u32 TEXT_PRIMARY = C2D_Color32(250, 246, 236, 255);
constexpr u32 TEXT_MUTED = C2D_Color32(174, 189, 207, 255);
constexpr u32 TEXT_DIM = C2D_Color32(102, 119, 142, 255);
constexpr u32 BOTTOM_BG = C2D_Color32(9, 14, 23, 255);
constexpr u32 CHIP = C2D_Color32(30, 43, 63, 255);
constexpr u32 SHADOW = C2D_Color32(0, 0, 0, 90);

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

void draw_panel(float x, float y, float w, float h, u32 bg = CARD,
                u32 edge = CARD_EDGE) {
    C2D_DrawRectSolid(x + 3.0f, y + 3.0f, 0.05f, w, h, SHADOW);
    C2D_DrawRectSolid(x, y, 0.1f, w, h, bg);
    C2D_DrawRectSolid(x, y, 0.2f, 3.0f, h, edge);
}

void draw_section_label(const std::string &text, float x, float y) {
    C2D_DrawRectSolid(x, y + 12.0f, 0.1f, 28.0f, 2.0f, TOP_ACCENT);
    draw_wrapped_text(text, x + 36.0f, y, 0.2f, 0.34f, 160.0f, TOP_ACCENT);
}

void draw_loading_indicator(float x, float y, int frame) {
    constexpr int dot_count = 6;
    const int active_dot = frame % dot_count;
    for (int i = 0; i < dot_count; i++) {
        const float dot_x = x + (i * 16.0f);
        const float dot_h = (i == active_dot) ? 13.0f : 8.0f;
        const float dot_y = y + (13.0f - dot_h);
        const u32 color = (i == active_dot) ? TOP_ACCENT : TOP_ACCENT_SOFT;
        C2D_DrawRectSolid(dot_x, dot_y, 0.2f, 10.0f, dot_h, color);
    }
}

void draw_background() {
    C2D_TargetClear(g_ui.top, TOP_BG);
    C2D_TargetClear(g_ui.bottom, BOTTOM_BG);

    C2D_SceneBegin(g_ui.top);
    C2D_DrawRectSolid(0.0f, 0.0f, 0.0f, 400.0f, 240.0f, TOP_BG);
    C2D_DrawRectSolid(0.0f, 0.0f, 0.1f, 400.0f, 48.0f, TOP_BAND);
    C2D_DrawRectSolid(0.0f, 48.0f, 0.2f, 400.0f, 4.0f, TOP_ACCENT);

    draw_wrapped_text("STREAMPOTATO", 20.0f, 13.0f, 0.2f, 0.72f, 340.0f,
                      TOP_ACCENT);
    draw_wrapped_text("old 3DS / 2DS streaming", 22.0f, 34.0f, 0.2f, 0.31f,
                      300.0f, TEXT_MUTED);

    C2D_SceneBegin(g_ui.bottom);
    C2D_DrawRectSolid(0.0f, 0.0f, 0.0f, 320.0f, 240.0f, BOTTOM_BG);
    C2D_DrawRectSolid(0.0f, 0.0f, 0.1f, 320.0f, 5.0f, TOP_ACCENT);
}

bool begin_frame() {
    if (!g_ui.initialized || g_ui.text_buf == nullptr || !gspHasGpuRight()) {
        return false;
    }

    C2D_TextBufClear(g_ui.text_buf);
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    draw_background();
    return true;
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

    if (!begin_frame()) {
        return;
    }

    C2D_SceneBegin(g_ui.top);
    draw_panel(18.0f, 66.0f, 364.0f, 154.0f, CARD_ALT);
    draw_wrapped_text(title, 30.0f, 78.0f, 0.2f, 0.54f, 334.0f,
                      TEXT_PRIMARY);
    draw_wrapped_text(subtitle, 30.0f, 104.0f, 0.2f, 0.33f, 330.0f,
                      TEXT_MUTED);

    const int visible_count = 4;
    const int item_count = static_cast<int>(options.size());
    const int first_visible = std::max(
        0, std::min(selected - 1, std::max(0, item_count - visible_count)));

    float y = 132.0f;
    if (item_count == 0) {
        C2D_DrawRectSolid(30.0f, y, 0.1f, 338.0f, 44.0f, CARD);
        draw_wrapped_text("No items available.", 30.0f, y + 12.0f, 0.2f, 0.38f,
                          320.0f, TEXT_MUTED);
    } else {
        for (int i = 0; i < visible_count && first_visible + i < item_count;
             i++) {
            const int option_index = first_visible + i;
            const bool is_selected = option_index == selected;
            C2D_DrawRectSolid(30.0f, y, 0.1f, 338.0f, 18.0f,
                              is_selected ? CARD_SELECTED : CARD);
            if (is_selected) {
                C2D_DrawRectSolid(30.0f, y, 0.2f, 6.0f, 18.0f, TOP_BG);
            }

            std::string label = std::to_string(option_index + 1) + ". " +
                                options[option_index];
            const bool experimental =
                options[option_index].rfind("Experimental ", 0) == 0;
            if (experimental) {
                const u32 badge_bg = is_selected ? TOP_BG : TOP_ACCENT;
                const u32 badge_fg =
                    is_selected ? TOP_ACCENT : CARD_SELECTED_TEXT;
                draw_wrapped_text(std::to_string(option_index + 1) + ".",
                                  42.0f, y + 3.0f, 0.2f, 0.38f, 28.0f,
                                  is_selected ? CARD_SELECTED_TEXT
                                              : TEXT_PRIMARY);
                C2D_DrawRectSolid(67.0f, y + 3.0f, 0.2f, 28.0f, 12.0f,
                                  badge_bg);
                draw_wrapped_text("EXP", 72.0f, y + 5.0f, 0.3f, 0.26f,
                                  22.0f, badge_fg);
                draw_wrapped_text(options[option_index].substr(13), 102.0f,
                                  y + 3.0f, 0.2f, 0.38f, 250.0f,
                                  is_selected ? CARD_SELECTED_TEXT
                                              : TEXT_PRIMARY);
            } else {
                draw_wrapped_text(label, 42.0f, y + 3.0f, 0.2f, 0.38f, 310.0f,
                                  is_selected ? CARD_SELECTED_TEXT
                                              : TEXT_PRIMARY);
            }
            y += 20.0f;
        }
    }

    std::string counter = std::to_string(std::min(selected + 1, item_count)) +
                          " / " + std::to_string(item_count);

    C2D_SceneBegin(g_ui.bottom);
    draw_panel(18.0f, 24.0f, 284.0f, 88.0f, CARD);
    draw_panel(18.0f, 130.0f, 284.0f, 86.0f, CARD_ALT);
    draw_section_label("STATUS", 30.0f, 36.0f);
    draw_wrapped_text(status, 30.0f, 62.0f, 0.2f, 0.34f, 260.0f,
                      TEXT_PRIMARY);
    draw_section_label("CONTROLS", 30.0f, 142.0f);
    draw_wrapped_text("SELECTED", 30.0f, 166.0f, 0.2f, 0.30f, 66.0f,
                      TEXT_DIM);
    draw_chip(98.0f, 160.0f, 82.0f, 20.0f, counter, CHIP, TEXT_PRIMARY);
    draw_wrapped_text(footer_hint, 30.0f, 190.0f, 0.2f, 0.32f, 260.0f,
                      TEXT_MUTED);

    end_frame();
}

void menu_ui_draw_message(const std::string &title, const std::string &body,
                          const std::string &status,
                          const std::string &footer_hint) {
    if (!g_ui.initialized) {
        return;
    }

    if (!begin_frame()) {
        return;
    }

    C2D_SceneBegin(g_ui.top);
    draw_panel(20.0f, 74.0f, 360.0f, 120.0f, CARD_ALT);
    draw_wrapped_text(title, 36.0f, 92.0f, 0.2f, 0.56f, 328.0f,
                      TEXT_PRIMARY);
    draw_wrapped_text(body, 36.0f, 126.0f, 0.2f, 0.35f, 328.0f,
                      TEXT_MUTED);

    C2D_SceneBegin(g_ui.bottom);
    draw_panel(18.0f, 24.0f, 284.0f, 88.0f, CARD);
    draw_panel(18.0f, 130.0f, 284.0f, 86.0f, CARD_ALT);
    draw_section_label("DETAILS", 30.0f, 36.0f);
    draw_wrapped_text(status, 30.0f, 62.0f, 0.2f, 0.34f, 260.0f,
                      TEXT_PRIMARY);
    draw_section_label("INPUT", 30.0f, 142.0f);
    draw_wrapped_text(footer_hint, 30.0f, 168.0f, 0.2f, 0.32f, 260.0f,
                      TEXT_MUTED);

    end_frame();
}

void menu_ui_draw_loading(const std::string &title, const std::string &body,
                          const std::string &status,
                          const std::string &footer_hint, int frame) {
    if (!g_ui.initialized) {
        return;
    }

    if (!begin_frame()) {
        return;
    }

    C2D_SceneBegin(g_ui.top);
    draw_panel(20.0f, 74.0f, 360.0f, 124.0f, CARD_ALT);
    draw_wrapped_text(title, 36.0f, 92.0f, 0.2f, 0.56f, 328.0f,
                      TEXT_PRIMARY);
    draw_wrapped_text(body, 36.0f, 123.0f, 0.2f, 0.35f, 328.0f,
                      TEXT_MUTED);
    draw_loading_indicator(155.0f, 168.0f, frame);

    C2D_SceneBegin(g_ui.bottom);
    draw_panel(18.0f, 24.0f, 284.0f, 88.0f, CARD);
    draw_panel(18.0f, 130.0f, 284.0f, 86.0f, CARD_ALT);
    draw_section_label("STATUS", 30.0f, 36.0f);
    draw_wrapped_text(status, 30.0f, 62.0f, 0.2f, 0.34f, 260.0f,
                      TEXT_PRIMARY);
    draw_section_label("TASK", 30.0f, 142.0f);
    draw_wrapped_text(footer_hint, 30.0f, 168.0f, 0.2f, 0.32f, 260.0f,
                      TEXT_MUTED);

    end_frame();
}

void menu_ui_draw_pairing(const std::string &title, const std::string &pin,
                          const std::string &body, const std::string &status,
                          const std::string &footer_hint, int frame) {
    if (!g_ui.initialized) {
        return;
    }

    if (!begin_frame()) {
        return;
    }

    C2D_SceneBegin(g_ui.top);
    draw_panel(18.0f, 70.0f, 364.0f, 132.0f, CARD_ALT);
    draw_wrapped_text(title, 34.0f, 88.0f, 0.2f, 0.54f, 330.0f,
                      TEXT_PRIMARY);
    draw_wrapped_text(body, 34.0f, 116.0f, 0.2f, 0.34f, 330.0f, TEXT_MUTED);
    C2D_DrawRectSolid(34.0f, 146.0f, 0.2f, 154.0f, 38.0f, TOP_ACCENT);
    draw_wrapped_text(pin, 72.0f, 154.0f, 0.3f, 0.88f, 100.0f,
                      CARD_SELECTED_TEXT);
    draw_loading_indicator(208.0f, 158.0f, frame);

    C2D_SceneBegin(g_ui.bottom);
    draw_panel(18.0f, 24.0f, 284.0f, 88.0f, CARD);
    draw_panel(18.0f, 130.0f, 284.0f, 86.0f, CARD_ALT);
    draw_section_label("STATUS", 30.0f, 36.0f);
    draw_wrapped_text(status, 30.0f, 62.0f, 0.2f, 0.34f, 260.0f,
                      TEXT_PRIMARY);
    draw_section_label("PAIRING", 30.0f, 142.0f);
    draw_wrapped_text(footer_hint, 30.0f, 168.0f, 0.2f, 0.32f, 260.0f,
                      TEXT_MUTED);

    end_frame();
}
