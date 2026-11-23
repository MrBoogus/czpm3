#include "fb.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <linux/fb.h>
#include <png.h>
#include <sstream>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace cpz {

namespace {

// Compact 5x7 ASCII fallback font (chars 32..126) if TTF files are missing
static const uint8_t kFont5x7[95][5] = {
    {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x5f,0x00,0x00}, {0x00,0x07,0x00,0x07,0x00},
    {0x14,0x7f,0x14,0x7f,0x14}, {0x24,0x2a,0x7f,0x2a,0x12}, {0x23,0x13,0x08,0x64,0x62},
    {0x36,0x49,0x55,0x22,0x50}, {0x00,0x05,0x03,0x00,0x00}, {0x00,0x1c,0x22,0x41,0x00},
    {0x00,0x41,0x22,0x1c,0x00}, {0x14,0x08,0x3e,0x08,0x14}, {0x08,0x08,0x3e,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00}, {0x08,0x08,0x08,0x08,0x08}, {0x00,0x60,0x60,0x00,0x00},
    {0x20,0x10,0x08,0x04,0x02}, {0x3e,0x51,0x49,0x45,0x3e}, {0x00,0x42,0x7f,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4b,0x31}, {0x18,0x14,0x12,0x7f,0x10},
    {0x27,0x45,0x45,0x45,0x39}, {0x3c,0x4a,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1e}, {0x00,0x36,0x36,0x00,0x00},
    {0x00,0x56,0x36,0x00,0x00}, {0x08,0x14,0x22,0x41,0x00}, {0x14,0x14,0x14,0x14,0x14},
    {0x00,0x41,0x22,0x14,0x08}, {0x02,0x01,0x51,0x09,0x06}, {0x32,0x49,0x79,0x41,0x3e},
    {0x7e,0x11,0x11,0x11,0x7e}, {0x7f,0x49,0x49,0x49,0x36}, {0x3e,0x41,0x41,0x41,0x22},
    {0x7f,0x41,0x41,0x22,0x1c}, {0x7f,0x49,0x49,0x49,0x41}, {0x7f,0x09,0x09,0x09,0x01},
    {0x3e,0x41,0x49,0x49,0x7a}, {0x7f,0x08,0x08,0x08,0x7f}, {0x00,0x41,0x7f,0x41,0x00},
    {0x20,0x40,0x41,0x3f,0x01}, {0x7f,0x08,0x14,0x22,0x41}, {0x7f,0x40,0x40,0x40,0x40},
    {0x7f,0x02,0x0c,0x02,0x7f}, {0x7f,0x04,0x08,0x10,0x7f}, {0x3e,0x41,0x41,0x41,0x3e},
    {0x7f,0x09,0x09,0x09,0x06}, {0x3e,0x41,0x51,0x21,0x5e}, {0x7f,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7f,0x01,0x01}, {0x3f,0x40,0x40,0x40,0x3f},
    {0x1f,0x20,0x40,0x20,0x1f}, {0x3f,0x40,0x38,0x40,0x3f}, {0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43}, {0x00,0x7f,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20}, {0x00,0x41,0x41,0x7f,0x00}, {0x04,0x02,0x01,0x02,0x04},
    {0x40,0x40,0x40,0x40,0x40}, {0x00,0x01,0x02,0x04,0x00}, {0x20,0x54,0x54,0x54,0x78},
    {0x7f,0x48,0x44,0x44,0x38}, {0x38,0x44,0x44,0x44,0x20}, {0x38,0x44,0x44,0x48,0x7f},
    {0x38,0x54,0x54,0x54,0x18}, {0x08,0x7e,0x09,0x01,0x02}, {0x0c,0x52,0x52,0x52,0x3e},
    {0x7f,0x08,0x04,0x04,0x78}, {0x00,0x44,0x7d,0x40,0x00}, {0x20,0x40,0x44,0x3d,0x00},
    {0x7f,0x10,0x28,0x44,0x00}, {0x00,0x41,0x7f,0x40,0x00}, {0x7c,0x04,0x18,0x04,0x78},
    {0x7c,0x08,0x04,0x04,0x78}, {0x38,0x44,0x44,0x44,0x38}, {0x7c,0x14,0x14,0x14,0x08},
    {0x08,0x14,0x14,0x18,0x7c}, {0x7c,0x08,0x04,0x04,0x08}, {0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3f,0x44,0x40,0x20}, {0x3c,0x40,0x40,0x20,0x7c}, {0x1c,0x20,0x40,0x20,0x1c},
    {0x3c,0x40,0x30,0x40,0x3c}, {0x44,0x28,0x10,0x28,0x44}, {0x0c,0x50,0x50,0x50,0x3c},
    {0x44,0x64,0x54,0x4c,0x44}, {0x00,0x08,0x36,0x41,0x00}, {0x00,0x00,0x7f,0x00,0x00},
    {0x00,0x41,0x36,0x08,0x00}, {0x10,0x08,0x08,0x10,0x08}
};

uint32_t next_utf8(const std::string& s, size_t& i) {
    uint8_t c = static_cast<uint8_t>(s[i]);
    if (c < 0x80) {
        ++i;
        return c;
    }
    if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
        uint32_t cp = ((c & 0x1F) << 6) | (static_cast<uint8_t>(s[i + 1]) & 0x3F);
        i += 2;
        return cp;
    }
    if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
        uint32_t cp = ((c & 0x0F) << 12) |
                      ((static_cast<uint8_t>(s[i + 1]) & 0x3F) << 6) |
                      (static_cast<uint8_t>(s[i + 2]) & 0x3F);
        i += 3;
        return cp;
    }
    if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) {
        uint32_t cp = ((c & 0x07) << 18) |
                      ((static_cast<uint8_t>(s[i + 1]) & 0x3F) << 12) |
                      ((static_cast<uint8_t>(s[i + 2]) & 0x3F) << 6) |
                      (static_cast<uint8_t>(s[i + 3]) & 0x3F);
        i += 4;
        return cp;
    }
    ++i;
    return '?';
}

uint16_t blend565(uint16_t dst, uint16_t src, uint8_t alpha) {
    if (alpha == 0) return dst;
    if (alpha == 255) return src;
    uint32_t sr = (src >> 11) & 0x1F;
    uint32_t sg = (src >> 5) & 0x3F;
    uint32_t sb = src & 0x1F;
    uint32_t dr = (dst >> 11) & 0x1F;
    uint32_t dg = (dst >> 5) & 0x3F;
    uint32_t db = dst & 0x1F;
    uint32_t a = alpha;
    uint32_t inv = 255 - a;
    uint32_t r = sr * a + dr * inv; r = (r + 1 + (r >> 8)) >> 8;
    uint32_t g = sg * a + dg * inv; g = (g + 1 + (g >> 8)) >> 8;
    uint32_t b = sb * a + db * inv; b = (b + 1 + (b >> 8)) >> 8;
    return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

bool file_exists(const char* p) {
    return ::access(p, R_OK) == 0;
}

}  // namespace

Framebuffer::~Framebuffer() {
    shutdown();
}

std::string Framebuffer::detect_fb_device() {
    if (const char* env = std::getenv("CPZ_FB_DEVICE")) {
        if (*env && file_exists(env)) return env;
    }
    if (const char* env = std::getenv("LV_LINUX_FBDEV_DEVICE")) {
        if (*env && file_exists(env)) return env;
    }
    if (file_exists("/dev/fb_lcd")) {
        return "/dev/fb_lcd";
    }
    std::ifstream pfb("/proc/fb");
    std::string line;
    while (std::getline(pfb, line)) {
        if (line.find("panel-mipi-dbid") != std::string::npos ||
            line.find("fb_st7789v") != std::string::npos) {
            std::istringstream iss(line);
            int idx = -1;
            if (iss >> idx && idx >= 0) {
                std::string cand = "/dev/fb" + std::to_string(idx);
                if (file_exists(cand.c_str())) return cand;
            }
        }
    }
    return "/dev/fb0";
}

void Framebuffer::init_fonts() {
    if (FT_Init_FreeType(&ft_lib_) != 0) {
        ft_ok_ = false;
        return;
    }
    const char* reg_candidates[] = {
        "/usr/share/APPLaunch/share/font/JetBrainsMono-Regular.ttf",
        "/usr/share/APPLaunch/share/font/DejaVuSansMono.ttf",
        "/usr/share/APPLaunch/share/font/LiberationMono-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
    };
    for (const char* p : reg_candidates) {
        if (file_exists(p) && FT_New_Face(ft_lib_, p, 0, &ft_reg_) == 0) {
            font_path_ = p;
            ft_ok_ = true;
            break;
        }
    }
    const char* bold_candidates[] = {
        "/usr/share/APPLaunch/share/font/JetBrainsMono-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf",
    };
    if (ft_ok_) {
        for (const char* p : bold_candidates) {
            if (file_exists(p) && FT_New_Face(ft_lib_, p, 0, &ft_bold_) == 0) {
                break;
            }
        }
    }
}

bool Framebuffer::init(bool headless) {
    headless_ = headless;
    clear(col::BG);
    init_fonts();
    if (headless_) {
        fb_path_ = "headless";
        return true;
    }

    fb_path_ = detect_fb_device();
    fb_fd_ = ::open(fb_path_.c_str(), O_RDWR);
    if (fb_fd_ < 0) {
        return false;
    }

    struct fb_var_screeninfo vinfo {};
    struct fb_fix_screeninfo finfo {};
    if (ioctl(fb_fd_, FBIOGET_VSCREENINFO, &vinfo) < 0 ||
        ioctl(fb_fd_, FBIOGET_FSCREENINFO, &finfo) < 0) {
        ::close(fb_fd_);
        fb_fd_ = -1;
        return false;
    }

    fb_w_ = static_cast<int>(vinfo.xres);
    fb_h_ = static_cast<int>(vinfo.yres);
    fb_bpp_ = static_cast<int>(vinfo.bits_per_pixel);
    fb_stride_ = static_cast<int>(finfo.line_length);
    fb_map_size_ = finfo.smem_len;

    void* mapped = mmap(nullptr, fb_map_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd_, 0);
    if (mapped == MAP_FAILED) {
        ::close(fb_fd_);
        fb_fd_ = -1;
        return false;
    }
    fb_map_ = static_cast<uint8_t*>(mapped);
    return true;
}

void Framebuffer::shutdown() {
    if (fb_map_ && fb_map_ != MAP_FAILED) {
        clear(col::BLACK);
        present();
        munmap(fb_map_, fb_map_size_);
        fb_map_ = nullptr;
    }
    if (fb_fd_ >= 0) {
        ::close(fb_fd_);
        fb_fd_ = -1;
    }
    std::memset(ascii_cache_, 0, sizeof(ascii_cache_));
    glyph_cache_.clear();
    if (ft_bold_) {
        FT_Done_Face(ft_bold_);
        ft_bold_ = nullptr;
    }
    if (ft_reg_) {
        FT_Done_Face(ft_reg_);
        ft_reg_ = nullptr;
    }
    if (ft_lib_) {
        FT_Done_FreeType(ft_lib_);
        ft_lib_ = nullptr;
    }
    ft_ok_ = false;
}

void Framebuffer::clear(uint16_t color) {
    std::fill(std::begin(backbuf_), std::end(backbuf_), color);
}

void Framebuffer::put_pixel(int x, int y, uint16_t color) {
    if (x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H) return;
    backbuf_[y * SCREEN_W + x] = color;
}

void Framebuffer::blend_pixel(int x, int y, uint16_t color, uint8_t alpha) {
    if (x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H) return;
    uint16_t& dst = backbuf_[y * SCREEN_W + x];
    dst = blend565(dst, color, alpha);
}

void Framebuffer::fill_rect(int x, int y, int w, int h, uint16_t color) {
    if (w <= 0 || h <= 0) return;
    int x0 = std::max(0, x);
    int y0 = std::max(0, y);
    int x1 = std::min(SCREEN_W, x + w);
    int y1 = std::min(SCREEN_H, y + h);
    for (int row = y0; row < y1; ++row) {
        std::fill(&backbuf_[row * SCREEN_W + x0], &backbuf_[row * SCREEN_W + x1], color);
    }
}

void Framebuffer::draw_hline(int x, int y, int w, uint16_t color) {
    fill_rect(x, y, w, 1, color);
}

void Framebuffer::draw_vline(int x, int y, int h, uint16_t color) {
    fill_rect(x, y, 1, h, color);
}

void Framebuffer::draw_rect(int x, int y, int w, int h, uint16_t color) {
    if (w <= 0 || h <= 0) return;
    draw_hline(x, y, w, color);
    draw_hline(x, y + h - 1, w, color);
    draw_vline(x, y, h, color);
    draw_vline(x + w - 1, y, h, color);
}

void Framebuffer::fill_round_rect(int x, int y, int w, int h, int r, uint16_t color) {
    if (w <= 0 || h <= 0) return;
    if (r <= 1) {
        fill_rect(x, y, w, h, color);
        return;
    }
    r = std::min(r, std::min(w, h) / 2);
    fill_rect(x + r, y, w - 2 * r, h, color);
    fill_rect(x, y + r, r, h - 2 * r, color);
    fill_rect(x + w - r, y + r, r, h - 2 * r, color);

    int rr = r * r;
    for (int dy = 0; dy < r; ++dy) {
        for (int dx = 0; dx < r; ++dx) {
            int ox = r - 1 - dx;
            int oy = r - 1 - dy;
            if (ox * ox + oy * oy <= rr) {
                put_pixel(x + dx, y + dy, color);
                put_pixel(x + w - 1 - dx, y + dy, color);
                put_pixel(x + dx, y + h - 1 - dy, color);
                put_pixel(x + w - 1 - dx, y + h - 1 - dy, color);
            }
        }
    }
}

void Framebuffer::draw_round_rect(int x, int y, int w, int h, int r, uint16_t color) {
    if (w <= 0 || h <= 0) return;
    if (r <= 1) {
        draw_rect(x, y, w, h, color);
        return;
    }
    r = std::min(r, std::min(w, h) / 2);
    draw_hline(x + r, y, w - 2 * r, color);
    draw_hline(x + r, y + h - 1, w - 2 * r, color);
    draw_vline(x, y + r, h - 2 * r, color);
    draw_vline(x + w - 1, y + r, h - 2 * r, color);

    int f = 1 - r;
    int ddF_x = 1;
    int ddF_y = -2 * r;
    int cx = 0;
    int cy = r;
    int lx = x + r;
    int rx = x + w - 1 - r;
    int ty = y + r;
    int by = y + h - 1 - r;

    while (cx <= cy) {
        put_pixel(lx - cx, ty - cy, color);
        put_pixel(lx - cy, ty - cx, color);
        put_pixel(rx + cx, ty - cy, color);
        put_pixel(rx + cy, ty - cx, color);
        put_pixel(lx - cx, by + cy, color);
        put_pixel(lx - cy, by + cx, color);
        put_pixel(rx + cx, by + cy, color);
        put_pixel(rx + cy, by + cx, color);
        if (f >= 0) {
            --cy;
            ddF_y += 2;
            f += ddF_y;
        }
        ++cx;
        ddF_x += 2;
        f += ddF_x;
    }
}

const Glyph* Framebuffer::get_glyph(uint32_t cp, int px_size, bool bold) {
    if (!ft_ok_) return nullptr;
    bool use_bold = (bold && ft_bold_ != nullptr);
    FT_Face face = use_bold ? ft_bold_ : ft_reg_;
    if (!face) return nullptr;

    int slot_idx = (px_size >= 8 && px_size <= 15) ? (px_size - 8) : -1;
    int bold_idx = use_bold ? 1 : 0;
    if (slot_idx >= 0 && cp < 128) {
        if (const Glyph* fast = ascii_cache_[slot_idx][bold_idx][cp]) {
            return fast;
        }
    }

    uint64_t key = (static_cast<uint64_t>(cp) << 16) |
                   (static_cast<uint64_t>(px_size & 0xFF) << 1) |
                   static_cast<uint64_t>(bold_idx);
    auto it = glyph_cache_.find(key);
    if (it != glyph_cache_.end()) {
        if (slot_idx >= 0 && cp < 128) {
            ascii_cache_[slot_idx][bold_idx][cp] = &it->second;
        }
        return &it->second;
    }

    FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(px_size));
    FT_UInt idx = FT_Get_Char_Index(face, cp);
    if (idx == 0 && cp != ' ') {
        idx = FT_Get_Char_Index(face, '?');
    }
    if (FT_Load_Glyph(face, idx, FT_LOAD_RENDER) != 0) {
        return nullptr;
    }

    const FT_GlyphSlot slot = face->glyph;
    Glyph g;
    g.width = static_cast<int>(slot->bitmap.width);
    g.height = static_cast<int>(slot->bitmap.rows);
    g.left = slot->bitmap_left;
    g.top = slot->bitmap_top;
    g.advance = static_cast<int>(slot->advance.x >> 6);
    if (g.width > 0 && g.height > 0) {
        g.alpha.resize(static_cast<size_t>(g.width * g.height));
        for (int row = 0; row < g.height; ++row) {
            const uint8_t* src = slot->bitmap.buffer + row * std::abs(slot->bitmap.pitch);
            std::memcpy(&g.alpha[row * g.width], src, static_cast<size_t>(g.width));
        }
    }
    auto [ins, _] = glyph_cache_.emplace(key, std::move(g));
    if (slot_idx >= 0 && cp < 128) {
        ascii_cache_[slot_idx][bold_idx][cp] = &ins->second;
    }
    return &ins->second;
}

int Framebuffer::char_width(int px_size, bool bold) {
    if (const Glyph* g = get_glyph('M', px_size, bold)) {
        return std::max(1, g->advance);
    }
    return 6;
}

int Framebuffer::line_height(int px_size) {
    return px_size + 3;
}

int Framebuffer::text_width(const std::string& text, int px_size, bool bold) {
    if (!ft_ok_) {
        return static_cast<int>(text.size()) * 6;
    }
    int total = 0;
    for (size_t i = 0; i < text.size();) {
        uint32_t cp = next_utf8(text, i);
        if (const Glyph* g = get_glyph(cp, px_size, bold)) {
            total += g->advance;
        }
    }
    return total;
}

void Framebuffer::draw_fallback_char(int x, int y, char c, uint16_t color, int px_size) {
    int idx = (c >= 32 && c <= 126) ? (c - 32) : ('?' - 32);
    int y_off = std::max(0, (px_size - 7) / 2);
    for (int col = 0; col < 5; ++col) {
        uint8_t bits = kFont5x7[idx][col];
        for (int row = 0; row < 7; ++row) {
            if (bits & (1 << row)) {
                put_pixel(x + col, y + y_off + row, color);
            }
        }
    }
}

void Framebuffer::draw_text(int x, int y, const std::string& text, uint16_t color,
                            int px_size, bool bold, int max_w) {
    if (!ft_ok_) {
        std::string draw_str = text;
        if (max_w > 0 && static_cast<int>(draw_str.size()) * 6 > max_w) {
            int max_chars = std::max(0, (max_w - 12) / 6);
            draw_str = draw_str.substr(0, static_cast<size_t>(max_chars)) + "..";
        }
        int cx = x;
        for (char c : draw_str) {
            draw_fallback_char(cx, y, c, color, px_size);
            cx += 6;
            if (max_w > 0 && cx - x >= max_w) break;
        }
        return;
    }

    const Glyph* stack_glyphs[96];
    std::vector<const Glyph*> heap_glyphs;
    size_t count = 0;
    int total_w = 0;

    auto push_glyph = [&](const Glyph* g) {
        if (count < 96 && heap_glyphs.empty()) {
            stack_glyphs[count++] = g;
        } else {
            if (heap_glyphs.empty()) {
                heap_glyphs.assign(stack_glyphs, stack_glyphs + count);
            }
            heap_glyphs.push_back(g);
            ++count;
        }
    };

    for (size_t i = 0; i < text.size();) {
        uint32_t cp = next_utf8(text, i);
        const Glyph* g = get_glyph(cp, px_size, bold);
        if (!g) continue;
        push_glyph(g);
        total_w += g->advance;
        if (max_w > 0 && total_w > max_w + 32) {
            // Enough to trigger ellipsization below
            break;
        }
    }

    const Glyph** glyphs = heap_glyphs.empty() ? stack_glyphs : heap_glyphs.data();
    size_t draw_count = count;

    const Glyph* dot_glyph = nullptr;
    if (max_w > 0 && total_w > max_w) {
        dot_glyph = get_glyph('.', px_size, bold);
        int ell_w = dot_glyph ? (dot_glyph->advance * 2) : 0;
        int acc = 0;
        size_t fit = 0;
        while (fit < count && acc + glyphs[fit]->advance + ell_w <= max_w) {
            acc += glyphs[fit]->advance;
            ++fit;
        }
        draw_count = fit;
    }

    int baseline = y + px_size;
    int pen_x = x;
    auto blit_glyph = [&](const Glyph* g) {
        int gx = pen_x + g->left;
        int gy = baseline - g->top;
        int r0 = std::max(0, -gy);
        int r1 = std::min(g->height, SCREEN_H - gy);
        int c0 = std::max(0, -gx);
        int c1 = std::min(g->width, SCREEN_W - gx);
        for (int row = r0; row < r1; ++row) {
            uint16_t* dst_row = &backbuf_[(gy + row) * SCREEN_W];
            const uint8_t* alpha_row = &g->alpha[row * g->width];
            for (int col = c0; col < c1; ++col) {
                uint8_t a = alpha_row[col];
                if (a > 12) {
                    dst_row[gx + col] = blend565(dst_row[gx + col], color, a);
                }
            }
        }
        pen_x += g->advance;
    };

    for (size_t i = 0; i < draw_count; ++i) {
        blit_glyph(glyphs[i]);
    }
    if (dot_glyph != nullptr) {
        blit_glyph(dot_glyph);
        blit_glyph(dot_glyph);
    }
}

int Framebuffer::draw_badge(int x, int y, const std::string& text, uint16_t bg, uint16_t fg,
                            int px_size, bool bold) {
    int tw = text_width(text, px_size, bold);
    int bw = tw + 10;
    int bh = px_size + 5;
    fill_round_rect(x, y, bw, bh, 4, bg);
    draw_text(x + 5, y + 1, text, fg, px_size, bold);
    return bw;
}

void Framebuffer::draw_icon(IconId icon, int x, int y, uint16_t color) {
    if (icon == IconId::NONE) return;

    static constexpr uint16_t kRadar[14] = {
        0b00000011000000,
        0b00011111111000,
        0b00100011000100,
        0b01000000000010,
        0b01000111100010,
        0b01001000010010,
        0b11101011010111,
        0b11101011010111,
        0b01001000010010,
        0b01000111100010,
        0b01000000000010,
        0b00100011000100,
        0b00011111111000,
        0b00000011000000,
    };
    static constexpr uint16_t kNfc[14] = {
        0b00000000000000,
        0b11111100000100,
        0b10000100000010,
        0b10110100010010,
        0b10110100001001,
        0b10000101001001,
        0b10000100101001,
        0b10000100101001,
        0b10000101001001,
        0b10000100001001,
        0b10000100010010,
        0b10000100000010,
        0b11111100000100,
        0b00000000000000,
    };
    static constexpr uint16_t kRfid[14] = {
        0b00001111110000,
        0b00110000001100,
        0b01000111100010,
        0b01001100110010,
        0b10011000011001,
        0b10010011001001,
        0b10010100101001,
        0b10010100101001,
        0b10010011001001,
        0b10011000011001,
        0b01001100110010,
        0b01000111100010,
        0b00110000001100,
        0b00001111110000,
    };
    static constexpr uint16_t kChip[14] = {
        0b00001001001000,
        0b00001001001000,
        0b00111111111100,
        0b00100000000100,
        0b11100111100111,
        0b00100100100100,
        0b00100100100100,
        0b11100100100111,
        0b00100100100100,
        0b00100111100100,
        0b11100000000111,
        0b00111111111100,
        0b00001001001000,
        0b00001001001000,
    };
    static constexpr uint16_t kFolder[14] = {
        0b00000000000000,
        0b01111100000000,
        0b11111110000000,
        0b10000011111110,
        0b10000000000010,
        0b11111111111110,
        0b10000000000010,
        0b10000000000010,
        0b10000000000010,
        0b10000000000010,
        0b10000000000010,
        0b10000000000010,
        0b11111111111110,
        0b00000000000000,
    };
    static constexpr uint16_t kTerminal[14] = {
        0b00000000000000,
        0b01111111111110,
        0b11000000000011,
        0b10000000000001,
        0b10010000000001,
        0b10001000000001,
        0b10000100000001,
        0b10001000000001,
        0b10010000000001,
        0b10000011110001,
        0b10000000000001,
        0b11000000000011,
        0b01111111111110,
        0b00000000000000,
    };
    static constexpr uint16_t kBluetooth[14] = {
        0b00000011000000,
        0b00000011100000,
        0b00000010110000,
        0b00100010011000,
        0b00010010110000,
        0b00001011100000,
        0b00000111000000,
        0b00000111000000,
        0b00001011100000,
        0b00010010110000,
        0b00100010011000,
        0b00000010110000,
        0b00000011100000,
        0b00000011000000,
    };
    static constexpr uint16_t kBlueShark[14] = {
        0b00000001100000,
        0b00000011100000,
        0b00000111100000,
        0b00001111100000,
        0b00011111100000,
        0b00111111100000,
        0b01111111110000,
        0b01111111110000,
        0b11111111111000,
        0b00000000000000,
        0b11011101110111,
        0b01100110011001,
        0b00000000000000,
        0b00000000000000,
    };
    static constexpr uint16_t kUsb[14] = {
        0b00000011000000,
        0b00000111100000,
        0b00001111110000,
        0b00000011000000,
        0b01100011000000,
        0b01100011001110,
        0b00100011001110,
        0b00111111000100,
        0b00000011111100,
        0b00000011000000,
        0b00000011000000,
        0b00000111100000,
        0b00000111100000,
        0b00000011000000,
    };
    static constexpr uint16_t kOffline[14] = {
        0b00001111110000,
        0b00111000011100,
        0b01100000001110,
        0b01000000011010,
        0b11000000110011,
        0b10000001100001,
        0b10000011000001,
        0b10000110000001,
        0b10001100000001,
        0b11011000000011,
        0b01010000000010,
        0b01110000000110,
        0b00111000011100,
        0b00001111110000,
    };
    static constexpr uint16_t kScan[14] = {
        0b00011111000000,
        0b00110001100000,
        0b01100000110000,
        0b01000000010000,
        0b01000000010000,
        0b01100000110000,
        0b00110001100000,
        0b00011111110000,
        0b00000001111000,
        0b00000000111100,
        0b00000000011110,
        0b00000000001111,
        0b00000000000110,
        0b00000000000000,
    };
    static constexpr uint16_t kPing[14] = {
        0b00000001110000,
        0b00000011100000,
        0b00000111000000,
        0b00001110000000,
        0b00011100000000,
        0b00111111111000,
        0b00111111111000,
        0b00000001110000,
        0b00000011100000,
        0b00000111000000,
        0b00001110000000,
        0b00011100000000,
        0b00111000000000,
        0b00000000000000,
    };
    static constexpr uint16_t kFileBin[14] = {
        0b00111111100000,
        0b00100000110000,
        0b00100000101000,
        0b00100000111100,
        0b00101101100100,
        0b00101101100100,
        0b00100000000100,
        0b00101011010100,
        0b00101011010100,
        0b00100000000100,
        0b00101101010100,
        0b00100000000100,
        0b00111111111100,
        0b00000000000000,
    };
    static constexpr uint16_t kFileLog[14] = {
        0b00111111100000,
        0b00100000110000,
        0b00100000101000,
        0b00100000111100,
        0b00101111110100,
        0b00100000000100,
        0b00101111110100,
        0b00100000000100,
        0b00101111110100,
        0b00100000000100,
        0b00101110000100,
        0b00100000000100,
        0b00111111111100,
        0b00000000000000,
    };
    static constexpr uint16_t kPlay[14] = {
        0b00000000000000,
        0b00110000000000,
        0b00111000000000,
        0b00111100000000,
        0b00111110000000,
        0b00111111000000,
        0b00111111100000,
        0b00111111110000,
        0b00111111100000,
        0b00111111000000,
        0b00111110000000,
        0b00111100000000,
        0b00111000000000,
        0b00110000000000,
    };
    static constexpr uint16_t kTune[14] = {
        0b00000000000000,
        0b00000000001100,
        0b00000000001100,
        0b00000000001100,
        0b00000001101100,
        0b00000001101100,
        0b00000001101100,
        0b00001101101100,
        0b00001101101100,
        0b00001101101100,
        0b01101101101100,
        0b01101101101100,
        0b01101101101100,
        0b00000000000000,
    };
    static constexpr uint16_t kKey[14] = {
        0b00000000000000,
        0b00000000000000,
        0b00111100000000,
        0b01100110000000,
        0b11000011000000,
        0b11011011111111,
        0b11011011111111,
        0b11000011000101,
        0b01100110000101,
        0b00111100000101,
        0b00000000000000,
        0b00000000000000,
        0b00000000000000,
        0b00000000000000,
    };
    static constexpr uint16_t kWifi[14] = {
        0b00000000000000,
        0b00000000000000,
        0b00111111111100,
        0b01100000000110,
        0b11000000000011,
        0b00001111110000,
        0b00011000011000,
        0b00110000001100,
        0b00000111100000,
        0b00001100110000,
        0b00000000000000,
        0b00000011000000,
        0b00000011000000,
        0b00000000000000,
    };

    const uint16_t* bmp = nullptr;
    switch (icon) {
        case IconId::RADAR:     bmp = kRadar; break;
        case IconId::NFC:       bmp = kNfc; break;
        case IconId::RFID:      bmp = kRfid; break;
        case IconId::CHIP:      bmp = kChip; break;
        case IconId::FOLDER:    bmp = kFolder; break;
        case IconId::TERMINAL:  bmp = kTerminal; break;
        case IconId::BLUETOOTH: bmp = kBluetooth; break;
        case IconId::BLUESHARK: bmp = kBlueShark; break;
        case IconId::USB:       bmp = kUsb; break;
        case IconId::OFFLINE:   bmp = kOffline; break;
        case IconId::SCAN:      bmp = kScan; break;
        case IconId::PING:      bmp = kPing; break;
        case IconId::FILE_BIN:  bmp = kFileBin; break;
        case IconId::FILE_LOG:  bmp = kFileLog; break;
        case IconId::PLAY:      bmp = kPlay; break;
        case IconId::TUNE:      bmp = kTune; break;
        case IconId::KEY:       bmp = kKey; break;
        case IconId::WIFI:      bmp = kWifi; break;
        case IconId::NONE:      return;
    }

    for (int row = 0; row < 14; ++row) {
        int py = y + row;
        if (py < 0 || py >= SCREEN_H) continue;
        uint16_t mask = bmp[row];
        if (mask == 0) continue;
        uint16_t* dst_row = &backbuf_[py * SCREEN_W];
        for (int col = 0; col < 14; ++col) {
            if (mask & (1U << (13 - col))) {
                int px = x + col;
                if (px >= 0 && px < SCREEN_W) {
                    dst_row[px] = color;
                }
            }
        }
    }
}

void Framebuffer::present() {
    if (headless_ || !fb_map_) return;
    int copy_w = std::min(SCREEN_W, fb_w_);
    int copy_h = std::min(SCREEN_H, fb_h_);

    if (fb_bpp_ == 16) {
        if (fb_stride_ == SCREEN_W * 2 && fb_w_ >= SCREEN_W && fb_h_ >= SCREEN_H) {
            std::memcpy(fb_map_, backbuf_, static_cast<size_t>(SCREEN_W * SCREEN_H * 2));
        } else {
            for (int y = 0; y < copy_h; ++y) {
                std::memcpy(fb_map_ + y * fb_stride_, &backbuf_[y * SCREEN_W],
                            static_cast<size_t>(copy_w * 2));
            }
        }
    } else if (fb_bpp_ == 32) {
        for (int y = 0; y < copy_h; ++y) {
            uint32_t* dst = reinterpret_cast<uint32_t*>(fb_map_ + y * fb_stride_);
            for (int x = 0; x < copy_w; ++x) {
                uint16_t c = backbuf_[y * SCREEN_W + x];
                uint8_t r = static_cast<uint8_t>(((c >> 11) & 0x1F) * 255 / 31);
                uint8_t g = static_cast<uint8_t>(((c >> 5) & 0x3F) * 255 / 63);
                uint8_t b = static_cast<uint8_t>((c & 0x1F) * 255 / 31);
                dst[x] = (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b;
            }
        }
    }
}

bool Framebuffer::save_png(const std::string& path) const {
    FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) return false;

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png) {
        std::fclose(fp);
        return false;
    }
    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_write_struct(&png, nullptr);
        std::fclose(fp);
        return false;
    }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        std::fclose(fp);
        return false;
    }

    png_init_io(png, fp);
    png_set_IHDR(png, info, SCREEN_W, SCREEN_H, 8, PNG_COLOR_TYPE_RGB,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    std::vector<uint8_t> row(SCREEN_W * 3);
    for (int y = 0; y < SCREEN_H; ++y) {
        for (int x = 0; x < SCREEN_W; ++x) {
            uint16_t c = backbuf_[y * SCREEN_W + x];
            row[x * 3 + 0] = static_cast<uint8_t>(((c >> 11) & 0x1F) * 255 / 31);
            row[x * 3 + 1] = static_cast<uint8_t>(((c >> 5) & 0x3F) * 255 / 63);
            row[x * 3 + 2] = static_cast<uint8_t>((c & 0x1F) * 255 / 31);
        }
        png_write_row(png, row.data());
    }

    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);
    std::fclose(fp);
    return true;
}

}  // namespace cpz
