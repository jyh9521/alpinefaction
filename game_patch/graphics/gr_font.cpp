#include <memory>
#include <unordered_map>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <exception>
#include <cctype>
#include <stdexcept>
#include <windows.h>
#include <patch_common/FunHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/AsmWriter.h>
#include <format>
#include <algorithm>
#include <cmath>
#include <common/utils/string-utils.h>
#include "../rf/gr/gr_font.h"
#include "../rf/ui.h"
#include "../rf/bmpman.h"
#include "../rf/multi.h"
#include "../rf/file/file.h"
#include "../bmpman/bmpman.h"

#include <ft2build.h>
#include FT_FREETYPE_H

struct ParsedFontName
{
    std::string name;
    int size_x;
    int size_y;
    bool digits_only;
};

template<typename T = int>
class TextureAtlasPacker {
public:
    void add(int w, int h, T userdata);
    void pack();
    [[nodiscard]] std::pair<int, int> get_size() const;
    [[nodiscard]] std::pair<int, int> get_pos(T userdata) const;

private:
    struct Item
    {
        int w, h, area;
        T userdata;
    };
    int total_pixels_ = 0;
    std::pair<int, int> atlas_size_;
    std::vector<Item> items_;
    std::unordered_map<T, std::pair<int, int>> packed_pos_map_;

    void update_size();
    bool try_pack();
};

class GrNewFont
{
public:
    GrNewFont(std::string_view name);
    void draw(int x, int y, std::string_view text, rf::gr::Mode state) const;
    void draw_aligned(rf::gr::TextAlignment align, int x, int y, std::string_view text, rf::gr::Mode state) const;
    void get_size(int* w, int* h, std::string_view text) const;

    [[nodiscard]] const std::string& get_name() const
    {
        return name_;
    }

    [[nodiscard]] int get_height() const
    {
        return height_;
    }

private:
    struct GlyphInfo
    {
        int bm_x;
        int bm_y;
        int bm_w;
        int bm_h;
        int x;
        int y;
        int advance_x;
    };

    std::string name_;
    int bitmap_;
    int height_;
    int baseline_y_;
    int line_spacing_;
    std::vector<GlyphInfo> glyphs_;
    // Keyed by Unicode code point. High bytes of legacy Windows-1252 text are
    // additionally registered under their raw byte value so both encodings
    // resolve to the same glyph (see the constructor).
    std::unordered_map<uint32_t, int> char_map_;
};

constexpr int ttf_font_flag = 0x1000;

// Stock .vf bitmap fonts index glyphs by a single byte, so any non-Latin text
// drawn with one comes out as per-byte mojibake. Alpine already swaps the four
// rf::ui globals for TrueType, but some widgets (the weapon priority list and
// the control binding list among them) hold a separate font handle and still
// ask for a .vf. Redirect the three general-purpose text fonts to their
// TrueType equivalents, using the same scale factors menu_init_hook uses, so
// those widgets get a font that can actually render the text.
//
// bigfont.vf / smallfont.vf / biggerfont.vf are deliberately left alone: they
// hold 32-69 glyphs with a non-standard first_ascii and are special-purpose
// glyph sets (HUD counters), not general text.
static std::string redirect_vf_font(const char* name)
{
    const float scale = rf::ui::scale_y;
    if (!(scale > 1.0f)) {
        return {};   // UI scale not known yet - keep the bitmap font
    }
    auto px = [scale](float factor) {
        return std::min(128, static_cast<int>(std::round(scale * factor)));
    };
    if (string_iequals(name, "rfpc-large.vf")) {
        return std::format("boldfont.ttf:{}", px(14.5f));
    }
    if (string_iequals(name, "rfpc-medium.vf")) {
        return std::format("regularfont.ttf:{}", px(9.0f));
    }
    if (string_iequals(name, "rfpc-small.vf")) {
        return std::format("regularfont.ttf:{}", px(7.5f));
    }
    return {};
}

FT_Library g_freetype_lib = nullptr;
int g_default_font_id = 0;
std::vector<GrNewFont> g_fonts;

static inline ParsedFontName parse_font_name(std::string_view name)
{
    auto name_splitted = string_split(name, ':');
    auto file_name_sv = name_splitted[0];
    auto size_y_sv = name_splitted.size() > 1 ? name_splitted[1] : "12";
    auto size_x_sv = name_splitted.size() > 2 ? name_splitted[2] : "0";
    auto flags_sv = name_splitted.size() > 3 ? name_splitted[3] : "";
    std::string file_name_str{file_name_sv};
    std::string size_y_str{size_y_sv};
    int size_y = std::stoi(size_y_str);
    std::string size_x_str{size_x_sv};
    int size_x = std::stoi(size_x_str);
    bool digits_only = string_contains(flags_sv, 'd');
    return {file_name_str, size_x, size_y, digits_only};
}

static bool load_file_into_buffer(const char* name, std::vector<unsigned char>& buffer)
{
    rf::File file;
    if (file.open(name) != 0) {
        xlog::error("Failed to open file {}", name);
        return false;
    }
    auto len = file.size();
    buffer.resize(len);
    int total_bytes_read = 0;
    while (len - total_bytes_read > 0) {
        int num_bytes_read = file.read(buffer.data() + total_bytes_read, len - total_bytes_read);
        if (num_bytes_read <= 0) {
            break;
        }
        total_bytes_read += num_bytes_read;
    }
    file.close();
    if (total_bytes_read != len) {
        xlog::error("Cannot read all file bytes");
        return false;
    }
    return true;
}

// ============================================================================
// Support for non-Latin scripts (CJK and others)
// ----------------------------------------------------------------------------
// Glyphs used to be indexed by a single byte (char_map_[256]), which can only
// hold the ~220 Windows-1252 characters. These two helpers extend that to
// arbitrary Unicode code points:
//   utf8_decode       - pull one code point out of a byte stream
//   get_extra_charset - read the list of extra glyphs to rasterize, from a
//                       packfile entry supplied by a translation mod
// ============================================================================

// Decode one UTF-8 code point at pos and advance pos.
// On an invalid byte it falls back to returning that single byte. That both
// guarantees forward progress and keeps legacy Windows-1252 text working:
// bytes 0x80-0xFF are invalid UTF-8 lead bytes, so they come back unchanged
// and can still be looked up by their byte value.
static uint32_t utf8_decode(std::string_view text, size_t& pos)
{
    auto byte_at = [&](size_t i) { return static_cast<unsigned char>(text[i]); };
    unsigned char lead = byte_at(pos);
    if (lead < 0x80) {
        ++pos;
        return lead;
    }
    uint32_t cp;
    int extra;
    if ((lead & 0xE0) == 0xC0) { cp = lead & 0x1Fu; extra = 1; }
    else if ((lead & 0xF0) == 0xE0) { cp = lead & 0x0Fu; extra = 2; }
    else if ((lead & 0xF8) == 0xF0) { cp = lead & 0x07u; extra = 3; }
    else { ++pos; return lead; }
    if (pos + extra >= text.size()) {
        ++pos;
        return lead;
    }
    for (int i = 1; i <= extra; ++i) {
        unsigned char cont = byte_at(pos + i);
        if ((cont & 0xC0) != 0x80) {
            ++pos;
            return lead;
        }
        cp = (cp << 6) | (cont & 0x3Fu);
    }
    pos += extra + 1;
    return cp;
}

// List of extra characters to rasterize. Lives in a packfile (e.g. a
// translation mod's vpp) as plain UTF-8 text; only the set of characters that
// appear in it matters, formatting is irrelevant. If the file is absent this
// does nothing and behaviour is identical to stock.
constexpr const char* extra_charset_filename = "alpine_extra_charset.txt";

static const std::vector<uint32_t>& get_extra_charset()
{
    static std::vector<uint32_t> charset;
    static bool loaded = false;
    if (loaded) {
        return charset;
    }
    loaded = true;
    if (!rf::File{}.find(extra_charset_filename)) {
        return charset;
    }
    std::vector<unsigned char> buffer;
    if (!load_file_into_buffer(extra_charset_filename, buffer)) {
        return charset;
    }
    std::string_view sv{reinterpret_cast<const char*>(buffer.data()), buffer.size()};
    std::unordered_map<uint32_t, bool> seen;
    size_t pos = 0;
    while (pos < sv.size()) {
        uint32_t cp = utf8_decode(sv, pos);
        if (cp < 0x80 || cp == 0xFEFF) {
            continue;  // ASCII is already in the base set; FEFF is a BOM
        }
        if (seen.emplace(cp, true).second) {
            charset.push_back(cp);
        }
    }
    xlog::info("Loaded {} extra glyphs from {}", charset.size(), extra_charset_filename);
    return charset;
}

template<typename T>
inline void TextureAtlasPacker<T>::add(int w, int h, T userdata)
{
    int area = w * h;
    items_.push_back({w, h, area, userdata});
    total_pixels_ += area;
}

template<typename T>
inline void TextureAtlasPacker<T>::pack()
{
    update_size();
    xlog::trace("Texture atlas usage: {:.2f}", total_pixels_ * 100.0f / (atlas_size_.first * atlas_size_.second));
    // Sort by area (from largest to smallest)
    std::sort(items_.begin(), items_.end(), [](Item& a, Item& b) {
        return a.area > b.area;
    });
    // Set positions
    if (!try_pack()) {
        atlas_size_ = {2 * atlas_size_.first, 2 * atlas_size_.second};
        if (!try_pack()) {
            throw std::runtime_error{"unable to pack texture atlas"};
        }
    }
}

template<typename T>
inline bool TextureAtlasPacker<T>::try_pack()
{
    // Set positions
    int cur_x = 0;
    int cur_y = 0;
    int current_row_h = 0;
    for (auto& item : items_) {
        if (cur_x + item.w > atlas_size_.first) {
            cur_x = 0;
            cur_y += current_row_h;
            current_row_h = 0;
        }
        if (cur_y + item.h > atlas_size_.second) {
            return false;
        }
        packed_pos_map_[item.userdata] = std::pair{cur_x, cur_y};
        cur_x += item.w;
        current_row_h = std::max(current_row_h, item.h);
    }
    return true;
}

template<typename T>
std::pair<int, int> TextureAtlasPacker<T>::get_size() const
{
    return atlas_size_;
}

template<typename T>
inline std::pair<int, int> TextureAtlasPacker<T>::get_pos(T userdata) const
{
    auto it = packed_pos_map_.find(userdata);
    if (it == packed_pos_map_.end()) {
        throw std::runtime_error{"element not found in texture atlas"};
    }
    return it->second;
}

template<typename T>
inline void TextureAtlasPacker<T>::update_size()
{
    // calculate area + some margin for place that we don't use
    int total_pixels_adjusted = total_pixels_ * 9 / 8;
    // find squere root of calculated area (atlas is a squere)
    auto size_before_rounding = std::sqrt(total_pixels_adjusted);
    // round to next power of two
    auto exp = std::ceil(std::log(size_before_rounding) / std::log(2));
    int size = static_cast<int>(std::pow(2, exp));
    atlas_size_ = std::pair{size, size};
}

GrNewFont::GrNewFont(std::string_view name) :
    name_{name}
{
    auto [filename, size_x, size_y, digits_only] = parse_font_name(name);
    std::vector<unsigned char> buffer;
    xlog::trace("Loading font {} size {}", filename, size_y);
    if (!load_file_into_buffer(filename.c_str(), buffer)) {
        xlog::error("load_file_into_buffer failed for {}", filename);
        throw std::runtime_error{"failed to load font"};
    }

    FT_Face face;
    FT_Error error = FT_New_Memory_Face(g_freetype_lib, buffer.data(), buffer.size(), 0, &face);
    if (error) {
        xlog::error("FT_New_Memory_Face failed: {}", error);
        throw std::runtime_error{"failed to load font"};
    }

    error = FT_Set_Pixel_Sizes(face, size_x, size_y);
    if (error) {
        xlog::error("FT_Set_Pixel_Sizes failed: {}", error);
        throw std::runtime_error{"failed to load font"};
    }

    xlog::trace("raw height {} ascender {} descender {}", face->height, face->ascender, face->descender);
    xlog::trace("scaled height {} ascender {} descender {}", face->size->metrics.height / 64,
        face->size->metrics.ascender / 64, face->size->metrics.descender / 64);
    line_spacing_ = face->size->metrics.height / 64;
    height_ = line_spacing_; //(face->size->metrics.ascender - face->size->metrics.descender) / 64;
    baseline_y_ = face->size->metrics.ascender / 64;
    xlog::trace("line_spacing {} height {} baseline_y {}", line_spacing_, height_, baseline_y_);

    // Prepare lookup table for translating Windows 1252 characters (encoding used by RF) into Unicode codepoints

    static std::pair<int, int> win_1252_char_ranges[]{
        {0x20, 0x7E},
        {0x8C, 0x8C},
        {0x95, 0x95},
        {0x99, 0x99},
        {0x9C, 0x9C},
        {0x9F, 0x9F},
        {0xA6, 0xA7},
        {0xA9, 0xAB},
        {0xAE, 0xAE},
        {0xB0, 0xB0},
        {0xC0, 0xC2},
        {0xC4, 0xCB},
        {0xCE, 0xCF},
        {0xD2, 0xD4},
        {0xD6, 0xD6},
        {0xD9, 0xDC},
        {0xDF, 0xE2},
        {0xE4, 0xEB},
        {0xEE, 0xEF},
        {0xF2, 0xF4},
        {0xF6, 0xF6},
        {0xF9, 0xFC},
    };

    // Base character set: Windows-1252, same as stock
    char_map_.clear();
    std::vector<int> unicode_code_points;
    for (auto& range : win_1252_char_ranges) {
        for (auto c = range.first; c < range.second + 1; ++c) {
            if (digits_only && !std::isdigit(c)) {
                continue;
            }
            char windows_1252_char = static_cast<char>(c);
            wchar_t unicode_char = 0;
            MultiByteToWideChar(1252, 0, &windows_1252_char, 1, &unicode_char, 1);
            unicode_code_points.push_back(unicode_char);
            int glyph_idx = static_cast<int>(unicode_code_points.size()) - 1;
            // Register both keys: stock tables are Windows-1252 (single byte),
            // translated tables are UTF-8. Bytes 0x80-0xFF are invalid UTF-8 lead
            // bytes, so the decoder returns them unchanged and the two encodings
            // never collide. This keeps the German and French editions working.
            char_map_[static_cast<uint32_t>(static_cast<unsigned char>(windows_1252_char))] = glyph_idx;
            char_map_[static_cast<uint32_t>(unicode_char)] = glyph_idx;
        }
    }
    // Extra character set (CJK etc.), registered by code point
    if (!digits_only) {
        for (uint32_t cp : get_extra_charset()) {
            if (char_map_.find(cp) != char_map_.end()) {
                continue;
            }
            unicode_code_points.push_back(static_cast<int>(cp));
            char_map_[cp] = static_cast<int>(unicode_code_points.size()) - 1;
        }
    }

    TextureAtlasPacker atlas_packer;

    for (auto codepoint : unicode_code_points) {
        error = FT_Load_Char(face, codepoint, FT_LOAD_BITMAP_METRICS_ONLY);
        if (error) {
            xlog::error("FT_Load_Char failed: {}", error);
            continue;
        }
        FT_GlyphSlot slot = face->glyph;
        atlas_packer.add(slot->bitmap.width, slot->bitmap.rows, codepoint);
    }

    atlas_packer.pack();
    auto [atlas_w, atlas_h] = atlas_packer.get_size();

    xlog::trace("Creating font texture atlas {}x{}", atlas_w, atlas_h);
    bitmap_ = rf::bm::create(rf::bm::FORMAT_8888_ARGB, atlas_w, atlas_h);
    if (bitmap_ == -1) {
        xlog::error("bm_create failed for font texture");
        throw std::runtime_error{"failed to load font"};
    }
    rf::gr::LockInfo lock;
    if (!rf::gr::lock(bitmap_, 0, &lock, rf::gr::LOCK_WRITE_ONLY)) {
        xlog::error("gr_lock failed for font texture");
        throw std::runtime_error{"failed to load font"};
    }

    auto* bitmap_bits = lock.data;
    glyphs_.reserve(unicode_code_points.size());

    for (auto codepoint : unicode_code_points) {
        error = FT_Load_Char(face, codepoint, FT_LOAD_RENDER);
        if (error) {
            xlog::error("FT_Load_Char failed: {}", error);
            continue;
        }
        FT_GlyphSlot slot = face->glyph;
        FT_Bitmap& bitmap = slot->bitmap;
        int glyph_bm_w = static_cast<int>(bitmap.width);
        int glyph_bm_h = static_cast<int>(bitmap.rows);

        auto [glyph_bm_x, glyph_bm_y] = atlas_packer.get_pos(codepoint);

        xlog::trace("glyph {:x} bitmap x {} y {} w {} h {} left {} top {} advance {}", codepoint, glyph_bm_x, glyph_bm_y,
            glyph_bm_w, glyph_bm_h, slot->bitmap_left, slot->bitmap_top, slot->advance.x >> 6);

        GlyphInfo glyph_info;
        glyph_info.advance_x = slot->advance.x >> 6;
        glyph_info.bm_x = glyph_bm_x;
        glyph_info.bm_y = glyph_bm_y;
        glyph_info.bm_w = glyph_bm_w;
        glyph_info.bm_h = glyph_bm_h;
        glyph_info.x = slot->bitmap_left;
        glyph_info.y = -slot->bitmap_top;

        int pixel_size = bm_bytes_per_pixel(lock.format);
        auto* dst_ptr = bitmap_bits + glyph_bm_y * lock.stride_in_bytes + glyph_bm_x * pixel_size;
        bm_convert_format(dst_ptr, lock.format, bitmap.buffer, rf::bm::FORMAT_8_ALPHA, bitmap.width, bitmap.rows, lock.stride_in_bytes, bitmap.pitch);

        glyphs_.push_back(glyph_info);
    }

    rf::gr::unlock(&lock);
    rf::gr::tcache_add_ref(bitmap_);
}

void GrNewFont::draw(int x, int y, std::string_view text, rf::gr::Mode state) const
{
    if (x == rf::gr::center_x) {
        draw_aligned(rf::gr::ALIGN_CENTER, rf::gr::screen.clip_width / 2, y, text, state);
        return;
    }
    int pen_x = x;
    int pen_y = y + baseline_y_;
    size_t char_pos = 0;
    while (char_pos < text.size()) {
        uint32_t cp = utf8_decode(text, char_pos);
        if (cp == static_cast<uint32_t>('\n')) {
            pen_x = x;
            pen_y += line_spacing_;
            continue;
        }
        auto it = char_map_.find(cp);
        if (it == char_map_.end()) {
            continue;
        }
        const auto& glyph_info = glyphs_[it->second];
        if (glyph_info.bm_w) {
            rf::gr::bitmap_ex(bitmap_, pen_x + glyph_info.x, pen_y + glyph_info.y, glyph_info.bm_w, glyph_info.bm_h, glyph_info.bm_x, glyph_info.bm_y, state);
        }
        pen_x += glyph_info.advance_x;
    }
    rf::gr::current_string_x = pen_x;
    rf::gr::current_string_y = y;
}

void GrNewFont::draw_aligned(rf::gr::TextAlignment alignment, int x, int y, std::string_view text, rf::gr::Mode state) const
{
    size_t cur_pos = 0;
    while (cur_pos < text.size()) {
        auto line_end_pos = text.find('\n', cur_pos);
        if (line_end_pos == std::string_view::npos) {
            line_end_pos = text.size();
        }
        size_t line_len = line_end_pos - cur_pos;
        auto line = text.substr(cur_pos, line_len);
        cur_pos += line_len + 1;

        int w, h;
        get_size(&w, &h, line);

        int line_x = x;
        if (alignment == rf::gr::ALIGN_CENTER) {
            line_x -= w / 2;
        }
        else if (alignment == rf::gr::ALIGN_RIGHT) {
            line_x -= w;
        }
        draw(line_x, y, line, state);
        y += line_spacing_;
    }
}

void GrNewFont::get_size(int* w, int* h, std::string_view text) const
{
    *w = 0;
    *h = line_spacing_;
    int cur_line_w = 0;
    size_t char_pos = 0;
    while (char_pos < text.size()) {
        uint32_t cp = utf8_decode(text, char_pos);
        if (cp == static_cast<uint32_t>('\n')) {
            *w = std::max(*w, cur_line_w);
            cur_line_w = 0;
            *h += line_spacing_;
            continue;
        }
        auto it = char_map_.find(cp);
        if (it != char_map_.end()) {
            cur_line_w += glyphs_[it->second].advance_x;
        }
    }
    *w = std::max(*w, cur_line_w);
}

CodeInjection gr_load_font_internal_fix_texture_ref{
    0x0051F429,
    [](auto& regs) {
        auto gr_tcache_add_ref = addr_as_ref<void(int bm_handle)>(0x0050E850);
        gr_tcache_add_ref(regs.eax);
    },
};

void init_freetype_lib()
{
    FT_Error error = FT_Init_FreeType(&g_freetype_lib);
    if (error) {
        xlog::error("FT_Init_FreeType failed: {}", error);
    }
}

int gr_font_get_default()
{
    return g_default_font_id;
}

void gr_font_set_default(int font_id)
{
    g_default_font_id = font_id;
}

FunHook<int(const char*, int)> gr_init_font_hook{
    0x0051F6E0,
    [](const char *name, int reserved) {
        if (rf::is_dedicated_server) {
           return -1;
        }
        std::string redirected;
        if (string_ends_with(name, ".vf")) {
            redirected = redirect_vf_font(name);
            if (redirected.empty()) {
                return gr_init_font_hook.call_target(name, reserved);
            }
            xlog::info("Redirecting bitmap font {} to {}", name, redirected);
            name = redirected.c_str();   // outlives every use below
        }
        for (unsigned i = 0; i < g_fonts.size(); ++i) {
            auto& font = g_fonts[i];
            if (font.get_name() == name) {
                return static_cast<int>(i | ttf_font_flag);
            }
        }
        try {
            GrNewFont font{name};
            g_fonts.push_back(font);
            return static_cast<int>((g_fonts.size() - 1) | ttf_font_flag);
        }
        catch (std::exception& e) {
            xlog::error("Failed to load font {}: {}", name, e.what());
            return -1;
        }
    },
};

FunHook<bool(const char*)> gr_set_default_font_hook{
    0x0051FE20,
    [](const char* name) {
        int font = rf::gr::load_font(name, -1);
        if (font >= 0) {
            g_default_font_id = font;
            return true;
        }
        return false;
    },
};

FunHook<int(int)> gr_get_font_height_hook{
    0x0051F4D0,
    [](int font_num) {
        if (font_num == -1) {
            font_num = g_default_font_id;
        }
        if (font_num & ttf_font_flag) {
            unsigned idx = static_cast<unsigned>(font_num & ~ttf_font_flag);
            if (idx >= g_fonts.size()) {
                xlog::error("gr_get_font_height: bad TTF font id {:#x} (have {})",
                    font_num, g_fonts.size());
                return 0;
            }
            return g_fonts[idx].get_height();
        }
        return gr_get_font_height_hook.call_target(font_num);
    },
};

FunHook<void(int, int, const char*, int, rf::gr::Mode)> gr_string_hook{
    0x0051FEB0,
    [](int x, int y, const char *text, int font_num, rf::gr::Mode mode) {
        if (font_num == -1) {
            font_num = g_default_font_id;
        }
        if (font_num & ttf_font_flag) {
            unsigned idx = static_cast<unsigned>(font_num & ~ttf_font_flag);
            if (idx >= g_fonts.size()) {
                xlog::error("gr_string: bad TTF font id {:#x} (have {}) text={}",
                    font_num, g_fonts.size(), text ? text : "(null)");
                return;
            }
            try {
                g_fonts[idx].draw(x, y, text, mode);
            }
            catch (const std::exception& e) {
                xlog::error("gr_string threw: {} | font={:#x} text={}",
                    e.what(), font_num, text ? text : "(null)");
            }
        }
        else {
            gr_string_hook.call_target(x, y, text, font_num, mode);
        }
    },
};

FunHook<void(int*, int*, const char*, int, int)> gr_get_string_size_hook{
    0x0051F530,
    [](int *out_width, int *out_height, const char *text, int text_len, int font_num) {
        if (font_num == -1) {
            font_num = g_default_font_id;
        }
        if (font_num & ttf_font_flag) {
            unsigned idx = static_cast<unsigned>(font_num & ~ttf_font_flag);
            if (idx >= g_fonts.size()) {
                xlog::error("gr_get_string_size: bad TTF font id {:#x} (have {})",
                    font_num, g_fonts.size());
                *out_width = 0;
                *out_height = 0;
                return;
            }
            std::string_view text_sv;
            if (text_len < 0) {
                text_sv = std::string_view{text};
            }
            else {
                text_sv = std::string_view{text, static_cast<size_t>(text_len)};
            }
            try {
                g_fonts[idx].get_size(out_width, out_height, text_sv);
            }
            catch (const std::exception& e) {
                xlog::error("gr_get_string_size threw: {} | font={:#x} len={}",
                    e.what(), font_num, text_len);
                *out_width = 0;
                *out_height = 0;
            }
        }
        else {
            gr_get_string_size_hook.call_target(out_width, out_height, text, text_len, font_num);
        }
    },
};

CodeInjection gr_create_font_increment_number_injection{
    0x0051F800,
    []() {
        rf::gr::num_fonts++;
    },
};

int gr_fit_string(
    std::string& text,
    const int max_width,
    const int font_id,
    const std::string_view suffix
) {
    auto [text_w, text_h] = rf::gr::get_string_size(text, font_id);
    if (text_w <= max_width) {
        return text_w;
    }

    const auto [suffix_w, suffix_h] = rf::gr::get_string_size(suffix, font_id);
    if (suffix_w > max_width) {
        return text_w;
    }

    while (text_w + suffix_w > max_width && !text.empty()) {
        // Trim whole code points, otherwise a multi-byte character gets cut in half
        size_t cut = text.size() - 1;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
            --cut;
        }
        text.erase(cut);
        const auto [new_w, new_h] = rf::gr::get_string_size(text, font_id);
        text_w = new_w;
    }

    text.append(suffix);
    return text_w + suffix_w;
};


// ---- CJK aware line breaking ----
// See gr_split_str_hook below. Break opportunities follow the usual CJK rules: a line
// may break between two ideographs, but never before closing punctuation nor after
// opening punctuation.
static bool cjk_breakable(uint32_t cp)
{
    return (cp >= 0x2E80 && cp <= 0x9FFF)      // radicals, kana, CJK ideographs
        || (cp >= 0xF900 && cp <= 0xFAFF)      // compatibility ideographs
        || (cp >= 0x3000 && cp <= 0x303F)      // CJK punctuation
        || (cp >= 0xFF00 && cp <= 0xFF60);     // fullwidth forms
}

static bool cjk_no_break_before(uint32_t cp)
{
    switch (cp) {
    case 0x3001: case 0x3002: case 0xFF0C: case 0xFF0E:
    case 0xFF1A: case 0xFF1B: case 0xFF01: case 0xFF1F:
    case 0x300D: case 0x300F: case 0x3011: case 0x300B:
    case 0xFF09: case 0x2019: case 0x201D:
        return true;
    default:
        return false;
    }
}

static bool cjk_no_break_after(uint32_t cp)
{
    switch (cp) {
    case 0x300C: case 0x300E: case 0x3010: case 0x300A:
    case 0xFF08: case 0x2018: case 0x201C:
        return true;
    default:
        return false;
    }
}

// Replacement for the engine's gr_split_str. Fills offset_array/len_array with byte
// offsets and lengths of each line and returns the line count, same contract as the
// original. unk_char is ignored: its meaning in the original is unclear and every
// caller we can see passes 0. A newline always breaks, as it did before.
FunHook<int(int*, int*, char*, int, int, char, int)> gr_split_str_hook{
    0x00520810,
    [](int* len_array, int* offset_array, char* text, int max_w, int max_lines, char unk_char,
       int font_num) -> int {
        static_cast<void>(unk_char);
        if (!text || !len_array || !offset_array || max_lines <= 0 || max_w <= 0) {
            return 0;
        }
        const std::string_view sv{text};
        int n_lines = 0;
        size_t line_start = 0;
        size_t pos = 0;
        int line_w = 0;
        size_t break_end = 0;   // where the line would end if we break here
        size_t break_next = 0;  // where the following line would start
        uint32_t prev_cp = 0;
        size_t prev_pos = 0;

        auto emit = [&](size_t end, size_t next) {
            offset_array[n_lines] = static_cast<int>(line_start);
            len_array[n_lines] = static_cast<int>(end - line_start);
            ++n_lines;
            line_start = next;
            pos = next;
            line_w = 0;
            break_end = 0;
            break_next = 0;
            prev_cp = 0;
            prev_pos = 0;
        };

        while (pos < sv.size() && n_lines < max_lines) {
            size_t next = pos;
            const uint32_t cp = utf8_decode(sv, next);

            if (cp == static_cast<uint32_t>('\n')) {
                emit(pos, next);
                continue;
            }

            // Record a break opportunity for the pair (prev_cp, cp) before placing cp,
            // so that when cp turns out not to fit we already know where to cut.
            if (prev_cp == static_cast<uint32_t>(' ')) {
                break_end = prev_pos;   // the space itself is dropped
                break_next = pos;
            }
            else if (prev_cp && !cjk_no_break_after(prev_cp) && !cjk_no_break_before(cp)
                     && (cjk_breakable(prev_cp) || cjk_breakable(cp))) {
                break_end = pos;
                break_next = pos;
            }

            const int cw = rf::gr::get_string_size(sv.substr(pos, next - pos), font_num).first;
            if (line_w + cw > max_w && pos > line_start) {
                if (break_end > line_start) {
                    emit(break_end, break_next);
                }
                else {
                    emit(pos, pos);   // one word wider than the whole line: hard cut
                }
                continue;
            }

            line_w += cw;
            prev_cp = cp;
            prev_pos = pos;
            pos = next;
        }

        if (line_start < sv.size() && n_lines < max_lines) {
            offset_array[n_lines] = static_cast<int>(line_start);
            len_array[n_lines] = static_cast<int>(sv.size() - line_start);
            ++n_lines;
        }
        return n_lines;
    },
};

void gr_font_apply_patch()
{
    // Fix font texture leak
    // Original code sets bitmap handle in all fonts to -1 on level unload. On next font usage the font bitmap is reloaded.
    // Note: font bitmaps are dynamic (USERBMAP) so they cannot be found by name unlike normal bitmaps.
    AsmWriter(0x0051F3E0).ret();
    gr_load_font_internal_fix_texture_ref.install();

    // Support TrueType fonts
    gr_init_font_hook.install();
    gr_set_default_font_hook.install();
    gr_get_font_height_hook.install();
    gr_string_hook.install();
    gr_get_string_size_hook.install();
    gr_split_str_hook.install();
    init_freetype_lib();

    // Do not increament number of loaded fonts before a successful load
    // Fixes slots being exhausted if font cannot be loaded
    gr_create_font_increment_number_injection.install();
    AsmWriter{0x0051F7D8, 0x0051F7E3}.nop();
}
