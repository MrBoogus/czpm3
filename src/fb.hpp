#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H

namespace cpz {

constexpr int SCREEN_W = 320;
constexpr int SCREEN_H = 170;

constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Dark CZPM3 palette (high contrast on 320x170 ST7789V LCD)
namespace col {
constexpr uint16_t BG          = rgb565(10, 14, 22);
constexpr uint16_t SURFACE     = rgb565(18, 25, 38);
constexpr uint16_t SURFACE_ALT = rgb565(24, 34, 52);
constexpr uint16_t HEADER      = rgb565(15, 22, 35);
constexpr uint16_t BORDER      = rgb565(42, 62, 90);
constexpr uint16_t ACCENT      = rgb565(56, 208, 248);
constexpr uint16_t ACCENT_DIM  = rgb565(20, 74, 102);
constexpr uint16_t SELECT_BG   = rgb565(24, 62, 92);
constexpr uint16_t TEXT        = rgb565(236, 242, 248);
constexpr uint16_t TEXT_DIM    = rgb565(142, 160, 182);
constexpr uint16_t GREEN       = rgb565(68, 224, 128);
constexpr uint16_t GREEN_DIM   = rgb565(18, 66, 38);
constexpr uint16_t AMBER       = rgb565(250, 192, 64);
constexpr uint16_t AMBER_DIM   = rgb565(76, 56, 16);
constexpr uint16_t RED         = rgb565(248, 88, 96);
constexpr uint16_t RED_DIM     = rgb565(76, 24, 28);
constexpr uint16_t CYAN        = rgb565(100, 232, 255);
constexpr uint16_t BLACK       = rgb565(0, 0, 0);
}  // namespace col

struct Glyph {
    int width = 0;
    int height = 0;
    int left = 0;
    int top = 0;
    int advance = 0;
    std::vector<uint8_t> alpha;
};

enum class IconId {
    NONE,
    RADAR,
    NFC,
    RFID,
    CHIP,
    FOLDER,
    TERMINAL,
    BLUETOOTH,
    BLUESHARK,
    USB,
    OFFLINE,
    SCAN,
    PING,
    FILE_BIN,
    FILE_LOG,
    PLAY,
    TUNE,
    KEY,
    WIFI,
};

class Framebuffer {
public:
    Framebuffer() = default;
    ~Framebuffer();

    Framebuffer(const Framebuffer&) = delete;
    Framebuffer& operator=(const Framebuffer&) = delete;

    bool init(bool headless = false);
    void shutdown();

    void clear(uint16_t color = col::BG);
    void put_pixel(int x, int y, uint16_t color);
    void blend_pixel(int x, int y, uint16_t color, uint8_t alpha);

    void fill_rect(int x, int y, int w, int h, uint16_t color);
    void draw_rect(int x, int y, int w, int h, uint16_t color);
    void fill_round_rect(int x, int y, int w, int h, int r, uint16_t color);
    void draw_round_rect(int x, int y, int w, int h, int r, uint16_t color);
    void draw_hline(int x, int y, int w, uint16_t color);
    void draw_vline(int x, int y, int h, uint16_t color);

    int char_width(int px_size, bool bold = false);
    int line_height(int px_size);
    int text_width(const std::string& text, int px_size, bool bold = false);

    // Draws UTF-8 text; if max_w > 0, clips/ellipsizes to fit within max_w pixels.
    void draw_text(int x, int y, const std::string& text, uint16_t color,
                   int px_size = 11, bool bold = false, int max_w = -1);

    int draw_badge(int x, int y, const std::string& text, uint16_t bg, uint16_t fg,
                   int px_size = 10, bool bold = true);
    void draw_icon(IconId icon, int x, int y, uint16_t color);

    void present();
    bool save_png(const std::string& path) const;

    const std::string& device_path() const { return fb_path_; }
    const std::string& font_path() const { return font_path_; }

private:
    std::string detect_fb_device();
    void init_fonts();
    const Glyph* get_glyph(uint32_t cp, int px_size, bool bold);
    void draw_fallback_char(int x, int y, char c, uint16_t color, int px_size);

    uint16_t backbuf_[SCREEN_W * SCREEN_H]{};
    int fb_fd_ = -1;
    uint8_t* fb_map_ = nullptr;
    size_t fb_map_size_ = 0;
    int fb_w_ = SCREEN_W;
    int fb_h_ = SCREEN_H;
    int fb_bpp_ = 16;
    int fb_stride_ = SCREEN_W * 2;
    bool headless_ = false;
    std::string fb_path_;

    FT_Library ft_lib_ = nullptr;
    FT_Face ft_reg_ = nullptr;
    FT_Face ft_bold_ = nullptr;
    bool ft_ok_ = false;
    std::string font_path_;
    std::unordered_map<uint64_t, Glyph> glyph_cache_;
    const Glyph* ascii_cache_[8][2][128]{};
};

}  // namespace cpz
