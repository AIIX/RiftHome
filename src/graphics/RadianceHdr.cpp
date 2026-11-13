// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#include "graphics/RadianceHdr.hpp"

#include <QFile>
#include <QFloat16>

#include <algorithm>
#include <cmath>

namespace RiftHome::Graphics
{

namespace
{

float component(uint8_t value, int exponent)
{
    return exponent == 0 ? 0.0f : std::ldexp(float(value) + 0.5f, exponent - (128 + 8));
}

} // namespace

std::optional<HdrImage> parseRadianceHdr(const QByteArray &data)
{
    // Header: "#?RADIANCE" (or "#?RGBE"), lines up to an empty one; the format must be RGBE.
    if (!data.startsWith("#?")) {
        return std::nullopt;
    }
    qsizetype pos = 0;
    bool rgbe = false;
    for (;;) {
        const qsizetype end = data.indexOf('\n', pos);
        if (end < 0) {
            return std::nullopt;
        }
        const QByteArray line = data.mid(pos, end - pos).trimmed();
        pos = end + 1;
        if (line.isEmpty()) {
            break;
        }
        if (line.startsWith("FORMAT=")) {
            rgbe = line == "FORMAT=32-bit_rle_rgbe";
        }
    }
    if (!rgbe) {
        return std::nullopt;
    }
    // Resolution: the usual orientation only (top to bottom, left to right).
    const qsizetype end = data.indexOf('\n', pos);
    if (end < 0) {
        return std::nullopt;
    }
    const QList<QByteArray> words = data.mid(pos, end - pos).trimmed().split(' ');
    pos = end + 1;
    if (words.size() != 4 || words[0] != "-Y" || words[2] != "+X") {
        return std::nullopt;
    }
    HdrImage image;
    image.height = words[1].toInt();
    image.width = words[3].toInt();
    if (image.width <= 0 || image.height <= 0 || image.width > 32768 || image.height > 32768) {
        return std::nullopt;
    }
    image.rgb.resize(size_t(image.width) * size_t(image.height) * 3);
    const auto *bytes = reinterpret_cast<const uint8_t *>(data.constData());
    const qsizetype size = data.size();
    std::vector<uint8_t> scan(size_t(image.width) * 4);
    for (int y = 0; y < image.height; ++y) {
        if (pos + 4 > size) {
            return std::nullopt;
        }
        const bool runLength = image.width >= 8 && image.width < 32768 && bytes[pos] == 2 && bytes[pos + 1] == 2
            && ((bytes[pos + 2] << 8) | bytes[pos + 3]) == image.width;
        if (runLength) {
            // New-style scanline: each of the four components on its own, as runs and literals.
            pos += 4;
            for (int c = 0; c < 4; ++c) {
                int x = 0;
                while (x < image.width) {
                    if (pos >= size) {
                        return std::nullopt;
                    }
                    int count = bytes[pos++];
                    if (count > 128) {
                        count -= 128;
                        if (pos >= size || x + count > image.width) {
                            return std::nullopt;
                        }
                        const uint8_t value = bytes[pos++];
                        for (int i = 0; i < count; ++i) {
                            scan[size_t(x++) * 4 + size_t(c)] = value;
                        }
                    } else {
                        if (count == 0 || pos + count > size || x + count > image.width) {
                            return std::nullopt;
                        }
                        for (int i = 0; i < count; ++i) {
                            scan[size_t(x++) * 4 + size_t(c)] = bytes[pos++];
                        }
                    }
                }
            }
        } else {
            // Flat scanline (old files): four bytes a pixel.
            if (pos + qsizetype(image.width) * 4 > size) {
                return std::nullopt;
            }
            std::copy(bytes + pos, bytes + pos + image.width * 4, scan.begin());
            pos += image.width * 4;
        }
        float *row = image.rgb.data() + size_t(y) * size_t(image.width) * 3;
        for (int x = 0; x < image.width; ++x) {
            const uint8_t *p = &scan[size_t(x) * 4];
            row[x * 3 + 0] = component(p[0], p[3]);
            row[x * 3 + 1] = component(p[1], p[3]);
            row[x * 3 + 2] = component(p[2], p[3]);
        }
    }
    return image;
}

std::optional<HdrImage> loadRadianceHdr(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    return parseRadianceHdr(file.readAll());
}

float exposureFor(const HdrImage &image, float key)
{
    // The log-average (geometric mean) luminance, every 7th pixel (enough for a panorama).
    double sum = 0.0;
    size_t count = 0;
    const size_t pixels = image.rgb.size() / 3;
    for (size_t i = 0; i < pixels; i += 7) {
        const float *p = &image.rgb[i * 3];
        const double luminance = 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
        sum += std::log(1e-4 + luminance);
        ++count;
    }
    if (count == 0) {
        return 1.0f;
    }
    const double average = std::exp(sum / double(count));
    return float(std::clamp(key / average, 1e-3, 1e3));
}

QByteArray toHalfRgba(const HdrImage &image, float scale, int maxWidth, int &width, int &height, float highlightLimit)
{
    // Above the knee, v -> knee + (v - knee) / (1 + (v - knee) / (limit - knee)): smooth, never above the limit.
    constexpr float knee = 4.0f;
    const auto shoulder = [highlightLimit](float v) {
        if (highlightLimit <= knee || v <= knee) {
            return std::min(v, 65000.0f);
        }
        const float over = v - knee;
        return knee + over / (1.0f + over / (highlightLimit - knee));
    };
    int factor = 1;
    while (image.width / factor > maxWidth && image.height / factor > 1) {
        factor *= 2;
    }
    width = image.width / factor;
    height = image.height / factor;
    QByteArray out(qsizetype(width) * height * 8, Qt::Uninitialized);
    auto *halves = reinterpret_cast<qfloat16 *>(out.data());
    const float weight = scale / float(factor * factor);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float r = 0.0f, g = 0.0f, b = 0.0f;
            for (int dy = 0; dy < factor; ++dy) {
                const float *row = image.rgb.data() + (size_t(y * factor + dy) * size_t(image.width) + size_t(x * factor)) * 3;
                for (int dx = 0; dx < factor; ++dx) {
                    r += row[dx * 3 + 0];
                    g += row[dx * 3 + 1];
                    b += row[dx * 3 + 2];
                }
            }
            qfloat16 *px = halves + (size_t(y) * size_t(width) + size_t(x)) * 4;
            // A half's largest value is 65504: brighter than that (the sun) is clipped there.
            px[0] = qfloat16(shoulder(r * weight));
            px[1] = qfloat16(shoulder(g * weight));
            px[2] = qfloat16(shoulder(b * weight));
            px[3] = qfloat16(1.0f);
        }
    }
    return out;
}

} // namespace RiftHome::Graphics
