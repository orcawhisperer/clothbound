#include "formats/pdfrecolor.h"

#include "formats/pdfbook.h"

#include <QList>
#include <QSize>
#include <QSizeF>
#include <algorithm>
#include <cmath>

namespace PdfRecolor {

RecolorLut::RecolorLut(const QColor &bg, const QColor &fg) {
    br = bg.red();
    bgG = bg.green();
    bb = bg.blue();
    fr = fg.red();
    fgG = fg.green();
    fb = fg.blue();
    bgRgb = qRgb(br, bgG, bb);
    bgLuma = (br * 19595 + bgG * 38470 + bb * 7471 + 32768) >> 16;
    const int fgLuma = (fr * 19595 + fgG * 38470 + fb * 7471 + 32768) >> 16;
    darkBg = bgLuma < fgLuma;
    for (int l = 0; l < 256; ++l) {
        gray[l] = qRgb(fr + (br - fr) * l / 255, fgG + (bgG - fgG) * l / 255, fb + (bb - fb) * l / 255);
        tintR[l] = uint8_t((l * 4 + br) / 5);
        tintG[l] = uint8_t((l * 4 + bgG) / 5);
        tintB[l] = uint8_t((l * 4 + bb) / 5);
    }
}

void recolor(QImage &image, const QColor &bg, const QColor &fg) {
    if (image.format() != QImage::Format_RGB32 && image.format() != QImage::Format_ARGB32)
        image = image.convertToFormat(QImage::Format_RGB32);
    // Pure white paper and black ink stay untouched.
    if (bg == Qt::white && fg == Qt::black)
        return;
    const RecolorLut lut(bg, fg);
    const int w = image.width();
    const int h = image.height();
    for (int y = 0; y < h; ++y) {
        auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const QRgb pixel = line[x];
            if ((pixel & 0x00FFFFFFu) == 0x00FFFFFFu) {
                line[x] = lut.bgRgb;
                continue;
            }
            const int r = int((pixel >> 16) & 0xFFu);
            const int g = int((pixel >> 8) & 0xFFu);
            const int b = int(pixel & 0xFFu);
            line[x] = lut.mapRgb(r, g, b);
        }
    }
}

void compositeAndRecolor(QImage &image, const QColor &bg, const QColor &fg) {
    if (image.isNull())
        return;
    const bool applyTheme = bg.isValid() && fg.isValid() && !(bg == Qt::white && fg == Qt::black);
    const QImage::Format fmt = image.format();
    if (fmt == QImage::Format_RGB32) {
        if (applyTheme)
            recolor(image, bg, fg);
        return;
    }
    if (fmt != QImage::Format_ARGB32_Premultiplied && fmt != QImage::Format_ARGB32) {
        image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }
    const bool premul = (image.format() == QImage::Format_ARGB32_Premultiplied);
    const int w = image.width();
    const int h = image.height();

    if (!applyTheme) {
        for (int y = 0; y < h; ++y) {
            auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
            for (int x = 0; x < w; ++x) {
                const QRgb px = line[x];
                const uint32_t a = px >> 24;
                if (a == 0) {
                    line[x] = 0xFFFFFFFFu;
                } else if (a == 255) {
                    line[x] = px | 0xFF000000u;
                } else {
                    const uint32_t inv = 255u - a;
                    uint32_t r = (px >> 16) & 0xFFu;
                    uint32_t g = (px >> 8) & 0xFFu;
                    uint32_t b = px & 0xFFu;
                    if (premul) {
                        r = std::min(255u, r + inv);
                        g = std::min(255u, g + inv);
                        b = std::min(255u, b + inv);
                    } else {
                        r = (r * a + 255u * inv + 127u) / 255u;
                        g = (g * a + 255u * inv + 127u) / 255u;
                        b = (b * a + 255u * inv + 127u) / 255u;
                    }
                    line[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
                }
            }
        }
        image.reinterpretAsFormat(QImage::Format_RGB32);
        return;
    }

    const RecolorLut lut(bg, fg);
    for (int y = 0; y < h; ++y) {
        auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const QRgb px = line[x];
            const uint32_t a = px >> 24;
            if (a == 0) {
                line[x] = lut.bgRgb;
                continue;
            }
            if (a == 255) {
                if ((px & 0x00FFFFFFu) == 0x00FFFFFFu) {
                    line[x] = lut.bgRgb;
                } else {
                    line[x] = lut.mapRgb(int((px >> 16) & 0xFFu), int((px >> 8) & 0xFFu), int(px & 0xFFu));
                }
                continue;
            }
            const uint32_t inv = 255u - a;
            uint32_t r = (px >> 16) & 0xFFu;
            uint32_t g = (px >> 8) & 0xFFu;
            uint32_t b = px & 0xFFu;
            if (premul) {
                r = std::min(255u, r + inv);
                g = std::min(255u, g + inv);
                b = std::min(255u, b + inv);
            } else {
                r = (r * a + 255u * inv + 127u) / 255u;
                g = (g * a + 255u * inv + 127u) / 255u;
                b = (b * a + 255u * inv + 127u) / 255u;
            }
            line[x] = lut.mapRgb(int(r), int(g), int(b));
        }
    }
    image.reinterpretAsFormat(QImage::Format_RGB32);
}

QRectF contentBox(const QImage &page) {
    if (page.isNull() || page.width() <= 0 || page.height() <= 0)
        return {};
    const int w = page.width();
    const int h = page.height();
    int left = w, right = -1, top = h, bottom = -1;
    const QImage::Format fmt = page.format();
    if (fmt == QImage::Format_RGB32 || fmt == QImage::Format_ARGB32 || fmt == QImage::Format_ARGB32_Premultiplied) {
        const bool checkAlpha = (fmt != QImage::Format_RGB32);
        const bool premul = (fmt == QImage::Format_ARGB32_Premultiplied);
        auto isInk = [checkAlpha, premul](QRgb px) -> bool {
            if ((px & 0x00FFFFFFu) == 0x00FFFFFFu)
                return false;
            if (checkAlpha) {
                const uint32_t a = px >> 24;
                if (a == 0)
                    return false;
                const uint32_t inv = 255u - a;
                uint32_t r = (px >> 16) & 0xFFu;
                uint32_t g = (px >> 8) & 0xFFu;
                uint32_t b = px & 0xFFu;
                if (a < 255) {
                    if (premul) {
                        r = std::min(255u, r + inv);
                        g = std::min(255u, g + inv);
                        b = std::min(255u, b + inv);
                    } else {
                        r = (r * a + 255u * inv) / 255u;
                        g = (g * a + 255u * inv) / 255u;
                        b = (b * a + 255u * inv) / 255u;
                    }
                }
                return (r * 77u + g * 150u + b * 29u) < (200u << 8);
            }
            const uint32_t r = (px >> 16) & 0xFFu;
            const uint32_t g = (px >> 8) & 0xFFu;
            const uint32_t b = px & 0xFFu;
            return (r * 77u + g * 150u + b * 29u) < (200u << 8);
        };
        for (int y = 0; y < h; ++y) {
            const auto *line = reinterpret_cast<const QRgb *>(page.constScanLine(y));
            int rowLeft = -1;
            for (int x = 0; x < w; ++x) {
                if (isInk(line[x])) {
                    rowLeft = x;
                    break;
                }
            }
            if (rowLeft < 0)
                continue;
            if (y < top)
                top = y;
            bottom = y;
            if (rowLeft < left)
                left = rowLeft;
            for (int x = w - 1; x >= right + 1 && x >= rowLeft; --x) {
                if (isInk(line[x])) {
                    right = x;
                    break;
                }
            }
            if (rowLeft > right)
                right = rowLeft;
        }
    } else {
        const QImage gray = page.convertToFormat(QImage::Format_Grayscale8);
        for (int y = 0; y < gray.height(); ++y) {
            const uchar *line = gray.constScanLine(y);
            for (int x = 0; x < gray.width(); ++x)
                if (line[x] < 200) {
                    left = qMin(left, x);
                    right = qMax(right, x);
                    top = qMin(top, y);
                    bottom = qMax(bottom, y);
                }
        }
    }
    if (right < left || bottom < top)
        return {};
    return QRectF(double(left) / w, double(top) / h, double(right - left + 1) / w, double(bottom - top + 1) / h);
}

QRectF bookCrop(PdfBook &book) {
    const int pages = book.pageCount();
    if (pages <= 0)
        return QRectF(0, 0, 1, 1);
    // Pages through the middle of the book: covers and front matter differ.
    QList<double> lefts, tops, rights, bottoms;
    const int from = pages > 8 ? 3 : 0;
    const int step = qMax(1, (pages - from) / 24);
    for (int page = from; page < pages && lefts.size() < 24; page += step) {
        const QSizeF size = book.pageSize(page);
        if (size.width() <= 0 || size.height() <= 0 || !std::isfinite(size.width()) || !std::isfinite(size.height()))
            continue;
        const double ratio = std::clamp(size.height() / size.width(), 0.2, 5.0);
        const int height = qBound(1, qRound(240.0 * ratio), 1200);
        const QRectF box = contentBox(book.renderPageRaw(page, QSize(240, height)));
        if (box.isEmpty() || box.width() < 0.2 || box.height() < 0.2)
            continue;
        lefts.append(box.left());
        tops.append(box.top());
        rights.append(box.right());
        bottoms.append(box.bottom());
    }
    if (lefts.size() < 2)
        return QRectF(0, 0, 1, 1);
    std::sort(lefts.begin(), lefts.end());
    std::sort(tops.begin(), tops.end());
    std::sort(rights.begin(), rights.end());
    std::sort(bottoms.begin(), bottoms.end());
    // The widest the text goes on nearly every page, with a little room.
    const auto near = [](const QList<double> &values, double share) {
        return values[qBound(qsizetype(0), qsizetype(qRound(share * (values.size() - 1))), values.size() - 1)];
    };
    const double margin = 0.025;
    const double left = qMax(0.0, near(lefts, 0.1) - margin);
    const double top = qMax(0.0, near(tops, 0.1) - margin);
    const double right = qMin(1.0, near(rights, 0.9) + margin);
    const double bottom = qMin(1.0, near(bottoms, 0.9) + margin);
    if (right - left < 0.4 || bottom - top < 0.4)
        return QRectF(0, 0, 1, 1);
    return QRectF(left, top, right - left, bottom - top);
}

}  // namespace PdfRecolor
