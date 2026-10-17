// 描画の共通部品の実装。
#include "draw.h"

#include <cairo-ft.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

/**
 * cairo の決まりに沿って、フォントが要らなくなったら FT_Face を閉じるための後始末。
 * @param face FT_Face
 */
void destroyFtFace(void* face) {
    FT_Done_Face(static_cast<FT_Face>(face));
}

const cairo_user_data_key_t kFtFaceKey {};

}  // namespace

FontSet::FontSet() {
    FT_Library library = nullptr;
    if (FT_Init_FreeType(&library) == 0) ftLibrary_ = library;
}

FontSet::~FontSet() {
    release();
    // FT_Face は cairo が持っている間は生きている必要があるので、FT_Library は閉じない（終了時に OS が片付ける）
}

cairo_font_face_t* FontSet::createFace(const std::string& path, bool bold) {
    FT_Face face = nullptr;
    if (ftLibrary_ != nullptr && FT_New_Face(static_cast<FT_Library>(ftLibrary_), path.c_str(), 0, &face) == 0) {
        cairo_font_face_t* cairoFace = cairo_ft_font_face_create_for_ft_face(face, 0);
        if (cairo_font_face_set_user_data(cairoFace, &kFtFaceKey, face, destroyFtFace) == CAIRO_STATUS_SUCCESS) {
            return cairoFace;
        }
        cairo_font_face_destroy(cairoFace);
        FT_Done_Face(face);
    }
    std::fprintf(stderr, "[描画] フォント %s を読めないので Noto Sans CJK JP を探して使います\n", path.c_str());
    return cairo_toy_font_face_create("Noto Sans CJK JP", CAIRO_FONT_SLANT_NORMAL,
                                      bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
}

void FontSet::release() {
    // cairo_t が参照を持っていれば、そちらが手放すまでフォントは生きている
    if (regular_ != nullptr) cairo_font_face_destroy(regular_);
    if (bold_ != nullptr) cairo_font_face_destroy(bold_);
    regular_ = nullptr;
    bold_ = nullptr;
}

void FontSet::load(const std::string& regularPath, const std::string& boldPath) {
    if (regular_ != nullptr && regularPath == regularPath_ && boldPath == boldPath_) return;
    release();
    regular_ = createFace(regularPath, false);
    bold_ = createFace(boldPath, true);
    regularPath_ = regularPath;
    boldPath_ = boldPath;
}

double Pen::measure(const char* text, double size, bool isBold) const {
    cairo_set_font_face(cr, isBold ? fonts->bold() : fonts->regular());
    cairo_set_font_size(cr, size);
    cairo_text_extents_t extents;
    cairo_text_extents(cr, text, &extents);
    return extents.x_advance;
}

double Pen::text(double x, double y, const char* text, double size, Color c, bool isBold,
                 bool alignRight) const {
    color(c);
    if (alignRight) {
        const double width = measure(text, size, isBold);
        cairo_move_to(cr, x - width, y);
        cairo_show_text(cr, text);
        return width;
    }
    // 左揃えは先に幅を測らず、描いたあとの現在位置から幅を出す（文字の組み立てを 1 回で済ませる）
    cairo_set_font_face(cr, isBold ? fonts->bold() : fonts->regular());
    cairo_set_font_size(cr, size);
    cairo_move_to(cr, x, y);
    cairo_show_text(cr, text);
    double endX = x;
    double endY = y;
    cairo_get_current_point(cr, &endX, &endY);
    return endX - x;
}

void Pen::roundedRect(double x, double y, double w, double h, double r) const {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

void Pen::dot(double x, double y, Color c) const {
    color(c);
    cairo_arc(cr, x, y, 4.0, 0, 2 * M_PI);
    cairo_fill(cr);
}

double fitSize(const Pen& pen, const char* text, double size, double minSize, double maxWidth, bool bold) {
    while (size > minSize && pen.measure(text, size, bold) > maxWidth) size -= 1;
    return size;
}

double centerBaseline(double top, double h, double size) {
    return top + h / 2 + size * 0.36;
}

void textCentered(const Pen& pen, double cx, double baseline, const char* text, double size, Color c, bool bold) {
    pen.text(cx - pen.measure(text, size, bold) / 2, baseline, text, size, c, bold);
}

void drawCard(const Pen& pen, double x, double y, double w, double h, double r, Color fill, Color border,
              double borderWidth) {
    cairo_t* cr = pen.cr;
    // 影（下にずらした黒を 2 段重ねる）
    cairo_set_source_rgba(cr, 0, 0, 0, 0.22);
    pen.roundedRect(x - 2, y + 5, w + 4, h + 4, r + 2);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.30);
    pen.roundedRect(x, y + 2, w, h + 1, r);
    cairo_fill(cr);
    pen.color(fill);
    pen.roundedRect(x, y, w, h, r);
    cairo_fill(cr);
    if (borderWidth > 0) strokeRounded(pen, x, y, w, h, r, border, borderWidth);
    // 内側のハイライト（上の縁だけ少し明るく）
    cairo_set_source_rgba(cr, 1, 1, 1, 0.07);
    cairo_set_line_width(cr, 1);
    const double inset = borderWidth + 0.5;
    cairo_move_to(cr, x + r, y + inset);
    cairo_line_to(cr, x + w - r, y + inset);
    cairo_stroke(cr);
}

void strokeRounded(const Pen& pen, double x, double y, double w, double h, double r, Color c, double width) {
    pen.color(c);
    cairo_set_line_width(pen.cr, width);
    pen.roundedRect(x + width / 2, y + width / 2, w - width, h - width, std::max(0.0, r - width / 2));
    cairo_stroke(pen.cr);
}

void drawCheck(cairo_t* cr, double cx, double cy, double s, Color c) {
    cairo_set_source_rgb(cr, c.r, c.g, c.b);
    cairo_set_line_width(cr, s * 0.16);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_move_to(cr, cx - s * 0.36, cy + s * 0.02);
    cairo_line_to(cr, cx - s * 0.10, cy + s * 0.28);
    cairo_line_to(cr, cx + s * 0.38, cy - s * 0.26);
    cairo_stroke(cr);
}

void drawDisc(cairo_t* cr, double cx, double cy, double r, Color c) {
    cairo_set_source_rgb(cr, c.r, c.g, c.b);
    cairo_new_sub_path(cr);
    cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
    cairo_fill(cr);
}

void surfaceToRgba(cairo_surface_t* surface, std::vector<uint8_t>& out) {
    // Generate once in working memory, rather than storing 64 KiB in the executable.
    // Lookup keeps fractional-alpha pixels fast; opaque/transparent interiors skip it.
    static const auto channels = [] {
        std::vector<uint8_t> table(256 * 256);
        for (unsigned a = 1; a < 256; ++a) {
            for (unsigned c = 0; c < 256; ++c) {
                table[a * 256 + c] = static_cast<uint8_t>(std::min(255u, (c * 255 + a / 2) / a));
            }
        }
        return table;
    }();

    cairo_surface_flush(surface);
    const int width = cairo_image_surface_get_width(surface);
    const int height = cairo_image_surface_get_height(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    const uint8_t* data = cairo_image_surface_get_data(surface);
    out.resize(static_cast<size_t>(width) * height * 4);
    uint8_t* dst = out.data();
    for (int y = 0; y < height; ++y) {
        const auto* row = reinterpret_cast<const uint32_t*>(data + static_cast<size_t>(y) * stride);
        for (int x = 0; x < width; ++x) {
            // cairo は乗算済みの ARGB（ネイティブエンディアンの 32bit）なので、戻して RGBA に並べ替える
            const uint32_t p = row[x];
            const uint32_t a = p >> 24;
            if (a == 255) {
                dst[0] = static_cast<uint8_t>(p >> 16);
                dst[1] = static_cast<uint8_t>(p >> 8);
                dst[2] = static_cast<uint8_t>(p);
            } else if (a == 0) {
                dst[0] = dst[1] = dst[2] = 0;
            } else {
                const uint8_t* line = channels.data() + a * 256;
                dst[0] = line[(p >> 16) & 0xFF];
                dst[1] = line[(p >> 8) & 0xFF];
                dst[2] = line[p & 0xFF];
            }
            dst[3] = static_cast<uint8_t>(a);
            dst += 4;
        }
    }
}
