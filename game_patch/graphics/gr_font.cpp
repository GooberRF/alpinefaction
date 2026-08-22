#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <cstddef>
#include <cstdint>
#include <cstring>
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
#include "gr.h"
#include "../rf/gr/gr_font.h"
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

class GrNewFont
{
public:
    GrNewFont(std::string_view name);
    ~GrNewFont();
    // Owns an FT_Face and the file buffer it points into, so it cannot be copied.
    GrNewFont(const GrNewFont&) = delete;
    GrNewFont& operator=(const GrNewFont&) = delete;
    GrNewFont(GrNewFont&& other) noexcept;
    GrNewFont& operator=(GrNewFont&& other) noexcept;

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
        int page = -1;   // index into pages_, -1 when there is nothing to draw
        int bm_x = 0;
        int bm_y = 0;
        int bm_w = 0;
        int bm_h = 0;
        int x = 0;
        int y = 0;
        int advance_x = 0;
    };

    // Glyphs are rasterized on first use rather than up front. Pre-baking every
    // character a translation might need costs over 100 MB of atlas at 4K in a
    // 32 bit process, while a screen of text uses a couple of hundred distinct
    // characters. Pages are filled with a shelf allocator; a new one is created
    // when the current ones are full.
    //
    // A page keeps its own copy of the pixels. Locking a bitmap WRITE_ONLY hands
    // back staging memory that is not seeded from the current texture, and
    // unlocking copies the whole thing back, so a partial write would erase every
    // glyph already in the page. Glyphs go into pixels_ and the page is uploaded
    // as a unit, at most once per draw.
    struct AtlasPage
    {
        int bitmap;
        int next_x;
        int next_y;
        int row_h;
        bool dirty;
        std::vector<unsigned char> pixels;   // page_side_ * page_side_, FORMAT_8888_ARGB
    };

    const GlyphInfo& glyph_for(uint32_t code_point) const;
    GlyphInfo rasterize(uint32_t code_point) const;
    bool place(int w, int h, int& out_page, int& out_x, int& out_y) const;
    void flush_pages() const;

    std::string name_;
    // FT_New_Memory_Face does not copy its input, so the file has to outlive the face.
    std::vector<unsigned char> file_data_;
    FT_Face face_ = nullptr;
    bool digits_only_ = false;
    int height_ = 0;
    int baseline_y_ = 0;
    int line_spacing_ = 0;
    int page_side_ = 256;
    // Keyed by Unicode code point. Legacy Windows-1252 bytes are cached under
    // their raw byte value; rasterize() translates them before asking FreeType.
    mutable std::vector<AtlasPage> pages_;
    mutable std::unordered_map<uint32_t, GlyphInfo> glyphs_;
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
// The replacement is the same size as the bitmap font it stands in for, not a
// resolution-scaled one. These are the sizes menu_init_hook already treats as
// equivalent at scale 1, and they match the cell heights in the .vf headers once
// leading is taken off. Scaling here instead would blow up every HUD element that
// is laid out for the small bitmap font when Big HUD is off, and would also make
// the redirect depend on a scale that is not known yet during start-up -- font
// ids are handed out once and cached by the caller for good, so the same .vf would
// resolve to two different fonts and the engine would measure a string with one
// and draw it with the other.
static const char* redirect_vf_font(const char* name)
{
    if (string_iequals(name, "rfpc-large.vf")) {
        return "boldfont.ttf:15";
    }
    if (string_iequals(name, "rfpc-medium.vf")) {
        return "regularfont.ttf:9";
    }
    if (string_iequals(name, "rfpc-small.vf")) {
        return "regularfont.ttf:8";
    }
    return nullptr;
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
// hold the ~220 Windows-1252 characters, so anything outside that came out as
// per-byte mojibake. Glyphs are now keyed by Unicode code point and rasterized
// on demand, so a font covers whatever the text happens to contain.
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

GrNewFont::GrNewFont(std::string_view name) :
    name_{name}
{
    auto [filename, size_x, size_y, digits_only] = parse_font_name(name);
    digits_only_ = digits_only;
    xlog::trace("Loading font {} size {}", filename, size_y);
    if (!load_file_into_buffer(filename.c_str(), file_data_)) {
        xlog::error("load_file_into_buffer failed for {}", filename);
        throw std::runtime_error{"failed to load font"};
    }

    // The face keeps a pointer into file_data_, so both live as long as the font.
    FT_Error error = FT_New_Memory_Face(g_freetype_lib, file_data_.data(),
        static_cast<FT_Long>(file_data_.size()), 0, &face_);
    if (error) {
        xlog::error("FT_New_Memory_Face failed: {}", error);
        throw std::runtime_error{"failed to load font"};
    }

    error = FT_Set_Pixel_Sizes(face_, size_x, size_y);
    if (error) {
        FT_Done_Face(face_);
        face_ = nullptr;
        xlog::error("FT_Set_Pixel_Sizes failed: {}", error);
        throw std::runtime_error{"failed to load font"};
    }

    line_spacing_ = face_->size->metrics.height / 64;
    height_ = line_spacing_;
    baseline_y_ = face_->size->metrics.ascender / 64;
    xlog::trace("line_spacing {} height {} baseline_y {}", line_spacing_, height_, baseline_y_);

    // Size a page to hold roughly 250 glyphs of this font: small enough that a
    // font using a handful of characters does not reserve megabytes, large
    // enough that ordinary text never needs more than one or two pages.
    int side = 1;
    while (side < std::max(1, line_spacing_) * 16) {
        side *= 2;
    }
    page_side_ = std::clamp(side, 128, 1024);
}

GrNewFont::~GrNewFont()
{
    if (face_) {
        FT_Done_Face(face_);
    }
}

GrNewFont::GrNewFont(GrNewFont&& other) noexcept :
    name_{std::move(other.name_)},
    file_data_{std::move(other.file_data_)},
    face_{other.face_},
    digits_only_{other.digits_only_},
    height_{other.height_},
    baseline_y_{other.baseline_y_},
    line_spacing_{other.line_spacing_},
    page_side_{other.page_side_},
    pages_{std::move(other.pages_)},
    glyphs_{std::move(other.glyphs_)}
{
    // Moving the vector moves the heap block itself, so the pointer the face
    // holds into it stays valid.
    other.face_ = nullptr;
}

GrNewFont& GrNewFont::operator=(GrNewFont&& other) noexcept
{
    if (this != &other) {
        if (face_) {
            FT_Done_Face(face_);
        }
        name_ = std::move(other.name_);
        file_data_ = std::move(other.file_data_);
        face_ = other.face_;
        digits_only_ = other.digits_only_;
        height_ = other.height_;
        baseline_y_ = other.baseline_y_;
        line_spacing_ = other.line_spacing_;
        page_side_ = other.page_side_;
        pages_ = std::move(other.pages_);
        glyphs_ = std::move(other.glyphs_);
        other.face_ = nullptr;
    }
    return *this;
}

// Shelf allocation into the existing pages, creating another when they are full.
// Returns the page index rather than a bitmap handle so the caller can write into
// that page's pixel buffer.
bool GrNewFont::place(int w, int h, int& out_page, int& out_x, int& out_y) const
{
    constexpr int padding = 1;   // keeps filtering from bleeding between neighbours
    const int need_w = w + padding;
    const int need_h = h + padding;
    if (need_w > page_side_ || need_h > page_side_) {
        return false;
    }
    for (size_t i = 0; i < pages_.size(); ++i) {
        auto& page = pages_[i];
        if (page.next_x + need_w > page_side_) {
            page.next_x = 0;
            page.next_y += page.row_h;
            page.row_h = 0;
        }
        if (page.next_y + need_h > page_side_) {
            continue;
        }
        out_page = static_cast<int>(i);
        out_x = page.next_x;
        out_y = page.next_y;
        page.next_x += need_w;
        page.row_h = std::max(page.row_h, need_h);
        return true;
    }

    int bm_handle = rf::bm::create(rf::bm::FORMAT_8888_ARGB, page_side_, page_side_);
    if (bm_handle == -1) {
        xlog::error("bm_create failed for font atlas page {}x{}", page_side_, page_side_);
        return false;
    }
    rf::gr::tcache_add_ref(bm_handle);

    AtlasPage page;
    page.bitmap = bm_handle;
    page.next_x = need_w;
    page.next_y = 0;
    page.row_h = need_h;
    page.dirty = true;
    // Zero initialised, so anything not covered by a glyph is transparent.
    page.pixels.assign(static_cast<size_t>(page_side_) * page_side_ * 4, 0);
    pages_.push_back(std::move(page));

    out_page = static_cast<int>(pages_.size()) - 1;
    out_x = 0;
    out_y = 0;
    return true;
}

// Push every page whose pixels changed up to its texture. Called before drawing
// rather than from rasterize() so a string that introduces twenty new characters
// costs one upload per page instead of twenty.
void GrNewFont::flush_pages() const
{
    for (auto& page : pages_) {
        if (!page.dirty) {
            continue;
        }
        rf::gr::LockInfo lock;
        if (!rf::gr::lock(page.bitmap, 0, &lock, rf::gr::LOCK_WRITE_ONLY)) {
            xlog::error("gr_lock failed for font atlas page");
            continue;
        }
        bm_convert_format(lock.data, lock.format, page.pixels.data(), rf::bm::FORMAT_8888_ARGB,
            page_side_, page_side_, lock.stride_in_bytes, page_side_ * 4);
        // No mark_texture_dirty here: unlocking already copies the staging surface
        // to the texture, while marking it dirty would make the next frame reload
        // the texture from the bitmap's own pixels, which a user bitmap does not have.
        rf::gr::unlock(&lock);
        page.dirty = false;
    }
}

// Render one code point into a page's pixel buffer. A zeroed GlyphInfo (draws
// nothing, advances nothing) is returned when the character is not available,
// and that is what gets cached so the failure is not retried every frame.
GrNewFont::GlyphInfo GrNewFont::rasterize(uint32_t code_point) const
{
    GlyphInfo info;
    if (digits_only_ && (code_point < '0' || code_point > '9')) {
        return info;
    }

    // The decoder hands back bytes 0x80-0xFF unchanged because they are invalid
    // UTF-8 lead bytes. Stock, German and French tables are Windows-1252, so
    // translate those to the code point FreeType expects.
    uint32_t unicode = code_point;
    if (code_point >= 0x80 && code_point <= 0xFF) {
        char raw = static_cast<char>(code_point);
        wchar_t wide = 0;
        if (MultiByteToWideChar(1252, 0, &raw, 1, &wide, 1) == 1 && wide != 0) {
            unicode = static_cast<uint32_t>(wide);
        }
    }

    if (FT_Load_Char(face_, unicode, FT_LOAD_RENDER) != 0) {
        return info;
    }
    FT_GlyphSlot slot = face_->glyph;
    FT_Bitmap& bitmap = slot->bitmap;
    info.advance_x = slot->advance.x >> 6;
    info.x = slot->bitmap_left;
    info.y = -slot->bitmap_top;
    int glyph_w = static_cast<int>(bitmap.width);
    int glyph_h = static_cast<int>(bitmap.rows);
    if (glyph_w <= 0 || glyph_h <= 0) {
        return info;   // whitespace: advances the pen, nothing to blit
    }
    if (!place(glyph_w, glyph_h, info.page, info.bm_x, info.bm_y)) {
        info.page = -1;
        return info;
    }

    auto& page = pages_[info.page];
    const int stride = page_side_ * 4;
    auto* dst_ptr = page.pixels.data() + static_cast<size_t>(info.bm_y) * stride + info.bm_x * 4;
    bm_convert_format(dst_ptr, rf::bm::FORMAT_8888_ARGB, bitmap.buffer, rf::bm::FORMAT_8_ALPHA,
        bitmap.width, bitmap.rows, stride, bitmap.pitch);
    page.dirty = true;

    info.bm_w = glyph_w;
    info.bm_h = glyph_h;
    return info;
}

const GrNewFont::GlyphInfo& GrNewFont::glyph_for(uint32_t code_point) const
{
    auto it = glyphs_.find(code_point);
    if (it == glyphs_.end()) {
        it = glyphs_.emplace(code_point, rasterize(code_point)).first;
    }
    return it->second;
}

void GrNewFont::draw(int x, int y, std::string_view text, rf::gr::Mode state) const
{
    if (x == rf::gr::center_x) {
        draw_aligned(rf::gr::ALIGN_CENTER, rf::gr::screen.clip_width / 2, y, text, state);
        return;
    }
    // Rasterize everything this string needs first, then upload the pages that
    // changed, and only then draw. Uploading from inside the drawing loop would
    // mean one full page upload per new character.
    size_t scan_pos = 0;
    while (scan_pos < text.size()) {
        glyph_for(utf8_decode(text, scan_pos));
    }
    flush_pages();

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
        const auto& glyph_info = glyph_for(cp);
        if (glyph_info.page != -1 && glyph_info.bm_w) {
            rf::gr::bitmap_ex(pages_[glyph_info.page].bitmap, pen_x + glyph_info.x, pen_y + glyph_info.y,
                glyph_info.bm_w, glyph_info.bm_h, glyph_info.bm_x, glyph_info.bm_y, state);
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
        cur_line_w += glyph_for(cp).advance_x;
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
        if (string_ends_with(name, ".vf")) {
            const char* redirected = redirect_vf_font(name);
            if (!redirected) {
                return gr_init_font_hook.call_target(name, reserved);
            }
            xlog::info("Redirecting bitmap font {} to {}", name, redirected);
            name = redirected;
        }
        for (unsigned i = 0; i < g_fonts.size(); ++i) {
            auto& font = g_fonts[i];
            if (font.get_name() == name) {
                return static_cast<int>(i | ttf_font_flag);
            }
        }
        try {
            g_fonts.emplace_back(name);
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

// A bad font id is typically a stuck value in a HUD element, so it recurs every frame.
// The log appender flushes synchronously, so reporting it unconditionally would be
// thousands of disk writes per second. Report each distinct id once.
static bool report_bad_font_id_once(int font_num)
{
    static std::unordered_set<int> reported;
    return reported.insert(font_num).second;
}

FunHook<int(int)> gr_get_font_height_hook{
    0x0051F4D0,
    [](int font_num) {
        if (font_num == -1) {
            font_num = g_default_font_id;
        }
        if (font_num & ttf_font_flag) {
            unsigned idx = static_cast<unsigned>(font_num & ~ttf_font_flag);
            if (idx >= g_fonts.size()) {
                if (report_bad_font_id_once(font_num)) {
                    xlog::error("gr_get_font_height: bad TTF font id {:#x} (have {})",
                        font_num, g_fonts.size());
                }
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
                if (report_bad_font_id_once(font_num)) {
                    xlog::error("gr_string: bad TTF font id {:#x} (have {})",
                        font_num, g_fonts.size());
                }
                return;
            }
            g_fonts[idx].draw(x, y, text, mode);
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
                if (report_bad_font_id_once(font_num)) {
                    xlog::error("gr_get_string_size: bad TTF font id {:#x} (have {})",
                        font_num, g_fonts.size());
                }
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
            g_fonts[idx].get_size(out_width, out_height, text_sv);
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

// Byte offset where the last character of text starts, using the same rules the
// renderer decodes with. A backward scan over 0x80-0xBF would be wrong: those are
// UTF-8 continuation bytes, but they are also perfectly ordinary Windows-1252
// characters - the bullet at 0x95 and the automated chat prefix at 0xA6 among them -
// and the decoder hands such bytes back as single characters. Scanning forward means
// whatever the renderer treats as one character is what gets trimmed.
size_t gr_last_code_point_offset(std::string_view text)
{
    size_t pos = 0;
    size_t last = 0;
    while (pos < text.size()) {
        last = pos;
        utf8_decode(text, pos);
    }
    return last;
}

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
        text.erase(gr_last_code_point_offset(text));
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
