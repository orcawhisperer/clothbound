#pragma once

#include <QColor>
#include <QImage>
#include <QRectF>
#include <algorithm>
#include <cstdint>

class PdfBook;

// Pure QImage PDF theme-recoloring and margin-crop algorithms, free of Qt6::Quick
// so headless core builds (macOS SwiftUI bridge, CLI, benchmarks) can reuse them
// without linking QQuickAsyncImageProvider.
namespace PdfRecolor {

struct RecolorLut {
    QRgb bgRgb;
    int br, bgG, bb;
    int fr, fgG, fb;
    int bgLuma;
    bool darkBg;
    QRgb gray[256];
    uint8_t tintR[256];
    uint8_t tintG[256];
    uint8_t tintB[256];

    RecolorLut(const QColor &bg, const QColor &fg);

    inline QRgb mapRgb(int r, int g, int b) const {
        if (r == g && g == b)
            return gray[r];
        const int high = (r > g) ? (r > b ? r : b) : (g > b ? g : b);
        const int low = (r < g) ? (r < b ? r : b) : (g < b ? g : b);
        const int chroma = high - low;
        const int light = (r * 19595 + g * 38470 + b * 7471 + 32768) >> 16;
        const QRgb gpx = gray[light];
        if (chroma <= 16)
            return gpx;
        int cr, cg, cb;
        if (darkBg) {
            // Hue-preserving lightness inversion (HSL L' = 255 - L) mapped cleanly
            // into [bg, fg]: dark navy links, burgundy headings, and warm scans
            // invert to crisp, vivid light hues instead of disappearing into dark bg,
            // while pastel paper highlight boxes invert to dark tinted surfaces.
            const int shift = 255 - high - low;
            const int ir = r + shift;
            const int ig = g + shift;
            const int ib = b + shift;
            cr = br + (fr - br) * ir / 255;
            cg = bgG + (fgG - bgG) * ig / 255;
            cb = bb + (fb - bb) * ib / 255;
            // Deeply saturated primaries (e.g. pure #0000FF blue links) have shift ~= 0
            // and low perceptual luminance on dark ground; lift dark-ink pixels cleanly
            // toward fg so WCAG contrast stays comfortably legible.
            if (light < 140) {
                const int outLuma = (cr * 19595 + cg * 38470 + cb * 7471 + 32768) >> 16;
                const int minLuma = std::min(220, bgLuma + 95);
                if (outLuma < minLuma) {
                    const int lift = minLuma - outLuma;
                    cr = std::min(255, cr + lift);
                    cg = std::min(255, cg + lift);
                    cb = std::min(255, cb + lift);
                }
            }
        } else {
            cr = tintR[r];
            cg = tintG[g];
            cb = tintB[b];
        }
        if (chroma >= 64)
            return qRgb(cr, cg, cb);
        const int w = chroma - 16;  // 1..47 over 48
        const int iw = 48 - w;
        const int gr = qRed(gpx), gg = qGreen(gpx), gb = qBlue(gpx);
        // Exact division by 48 for non-negative values up to 255 * 48 = 12240:
        // floor(n / 48) == (n * 2731) >> 17.
        return qRgb(((gr * iw + cr * w) * 2731) >> 17, ((gg * iw + cg * w) * 2731) >> 17,
                    ((gb * iw + cb * w) * 2731) >> 17);
    }
};

// Ink to `fg` and paper to `bg`; colored pixels (pictures) mostly kept.
void recolor(QImage &image, const QColor &bg, const QColor &fg);
// Fused single-pass white-compositing + theme recoloring over a raw PDFium
// render (avoids allocating a second full-size QImage or scanning twice).
void compositeAndRecolor(QImage &image, const QColor &bg, const QColor &fg);
// The part of a page with anything on it (0 to 1 across and down).
QRectF contentBox(const QImage &page);
// One crop for a whole book, from some of its pages: margins around the
// text are cut, the same on every page so nothing jumps.
QRectF bookCrop(PdfBook &book);

}  // namespace PdfRecolor
