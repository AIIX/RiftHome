// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#include "xr/ViewShot.hpp"

#include <QFloat16>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <thread>

namespace RiftHome::Xr::ViewShot
{

namespace
{

using Spatial::Vec3;

constexpr float kPi = 3.14159265358979f;
constexpr float kKnee = 0.95f; //!< the display mapping passes values below this as they are

float srgbToLinear(float c)
{
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

float linearToSrgb(float c)
{
    c = std::clamp(c, 0.0f, 1.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

//! One level of the texture, with the part shown cut out.
Texture cropped(const Texture &source, int x, int y, int width, int height)
{
    Texture t;
    t.width = width;
    t.height = height;
    t.rgba.resize(size_t(width) * height * 4);
    for (int row = 0; row < height; ++row) {
        std::copy_n(source.rgba.begin() + (size_t(y + row) * source.width + x) * 4, size_t(width) * 4, t.rgba.begin() + size_t(row) * width * 4);
    }
    return t;
}

//! Half the size, each texel the mean of up to 2x2 (premultiplied values average correctly).
Texture halved(const Texture &s)
{
    Texture t;
    t.width = std::max(1, s.width / 2);
    t.height = std::max(1, s.height / 2);
    t.rgba.resize(size_t(t.width) * t.height * 4);
    for (int y = 0; y < t.height; ++y) {
        const int y0 = std::min(y * 2, s.height - 1);
        const int y1 = std::min(y * 2 + 1, s.height - 1);
        for (int x = 0; x < t.width; ++x) {
            const int x0 = std::min(x * 2, s.width - 1);
            const int x1 = std::min(x * 2 + 1, s.width - 1);
            for (int c = 0; c < 4; ++c) {
                t.rgba[(size_t(y) * t.width + x) * 4 + c] = 0.25f
                    * (s.rgba[(size_t(y0) * s.width + x0) * 4 + c] + s.rgba[(size_t(y0) * s.width + x1) * 4 + c]
                       + s.rgba[(size_t(y1) * s.width + x0) * 4 + c] + s.rgba[(size_t(y1) * s.width + x1) * 4 + c]);
            }
        }
    }
    return t;
}

//! Bilinear sample at @p u, @p v (0..1 over the level), clamped at the edges, or wrapped horizontally.
void bilinear(const Texture &t, float u, float v, bool wrapX, float out[4])
{
    const float fx = u * t.width - 0.5f;
    const float fy = v * t.height - 0.5f;
    const float flx = std::floor(fx);
    const float fly = std::floor(fy);
    const float ax = fx - flx;
    const float ay = fy - fly;
    int x0 = int(flx);
    int x1 = x0 + 1;
    const int y0 = std::clamp(int(fly), 0, t.height - 1);
    const int y1 = std::clamp(int(fly) + 1, 0, t.height - 1);
    if (wrapX) {
        x0 = ((x0 % t.width) + t.width) % t.width;
        x1 = ((x1 % t.width) + t.width) % t.width;
    } else {
        x0 = std::clamp(x0, 0, t.width - 1);
        x1 = std::clamp(x1, 0, t.width - 1);
    }
    const float *p00 = &t.rgba[(size_t(y0) * t.width + x0) * 4];
    const float *p10 = &t.rgba[(size_t(y0) * t.width + x1) * 4];
    const float *p01 = &t.rgba[(size_t(y1) * t.width + x0) * 4];
    const float *p11 = &t.rgba[(size_t(y1) * t.width + x1) * 4];
    for (int c = 0; c < 4; ++c) {
        const float top = p00[c] + (p10[c] - p00[c]) * ax;
        const float bottom = p01[c] + (p11[c] - p01[c]) * ax;
        out[c] = top + (bottom - top) * ay;
    }
}

//! A layer ready to be hit by rays: its pose inverted, its texture's levels made.
struct Prepared {
    const Layer *layer = nullptr;
    Vec3 origin; //!< the camera in the layer's space
    Vec3 rows[3]; //!< world directions into the layer's space
    Texture cut; //!< the part shown, when it is not all of the texture
    const Texture *base = nullptr; //!< the first level
    std::vector<Texture> rest; //!< the smaller levels
    bool wrapX = false;

    size_t levelCount() const
    {
        return rest.size() + 1;
    }
    const Texture &level(size_t i) const
    {
        return i == 0 ? *base : rest[i - 1];
    }

    Vec3 toLocal(Vec3 d) const
    {
        return {rows[0].dot(d), rows[1].dot(d), rows[2].dot(d)};
    }

    /*!
     * The layer's colour (premultiplied, colour scale applied) where the world ray @p d (unit) from the camera meets it;
     * @p pixelAngle: what one output pixel spans, in radians (which level of the picture to sample).
     */
    bool sample(Vec3 d, float pixelAngle, float out[4]) const
    {
        const Vec3 ld = toLocal(d);
        float u = 0.0f;
        float v = 0.0f;
        float ratio = 1.0f; //!< source texels an output pixel spans
        const int rw = base->width;
        const int rh = base->height;
        switch (layer->kind) {
        case Kind::Quad: {
            if (ld.z > -1e-6f) {
                return false;
            }
            const float t = -origin.z / ld.z;
            if (t <= 0.0f) {
                return false;
            }
            const float x = origin.x + t * ld.x;
            const float y = origin.y + t * ld.y;
            if (std::abs(x) > layer->width * 0.5f || std::abs(y) > layer->height * 0.5f) {
                return false;
            }
            u = x / layer->width + 0.5f;
            v = 0.5f - y / layer->height;
            const float texel = 0.5f * (layer->width / float(rw) + layer->height / float(rh));
            ratio = t * pixelAngle / texel;
            break;
        }
        case Kind::Cylinder: {
            const float a = ld.x * ld.x + ld.z * ld.z;
            if (a < 1e-9f || layer->radius <= 0.0f || layer->centralAngle <= 0.0f) {
                return false;
            }
            const float b = origin.x * ld.x + origin.z * ld.z;
            const float c = origin.x * origin.x + origin.z * origin.z - layer->radius * layer->radius;
            const float disc = b * b - a * c;
            if (disc < 0.0f) {
                return false;
            }
            const float t = (-b + std::sqrt(disc)) / a; // the far side of the circle: the inside surface
            if (t <= 0.0f) {
                return false;
            }
            const float x = origin.x + t * ld.x;
            const float y = origin.y + t * ld.y;
            const float z = origin.z + t * ld.z;
            const float theta = std::atan2(x, -z);
            const float height = layer->radius * layer->centralAngle / layer->aspectRatio;
            if (std::abs(theta) > layer->centralAngle * 0.5f || std::abs(y) > height * 0.5f) {
                return false;
            }
            u = theta / layer->centralAngle + 0.5f;
            v = 0.5f - y / height;
            const float texel = 0.5f * (layer->radius * layer->centralAngle / float(rw) + height / float(rh));
            ratio = t * pixelAngle / texel;
            break;
        }
        case Kind::Equirect: {
            Vec3 p = ld;
            float t = 1.0f;
            if (layer->radius > 0.0f) {
                const float b = origin.dot(ld);
                const float c = origin.dot(origin) - layer->radius * layer->radius;
                const float disc = b * b - c;
                if (disc < 0.0f) {
                    return false;
                }
                t = -b + std::sqrt(disc);
                if (t <= 0.0f) {
                    return false;
                }
                p = origin + ld * t;
            }
            const float length = p.length();
            if (length < 1e-6f) {
                return false;
            }
            const float theta = std::atan2(p.x, -p.z);
            const float phi = std::asin(std::clamp(p.y / length, -1.0f, 1.0f));
            const float span = layer->upperAngle - layer->lowerAngle;
            if (span <= 0.0f || layer->centralAngle <= 0.0f || (!wrapX && std::abs(theta) > layer->centralAngle * 0.5f)) {
                return false;
            }
            u = theta / layer->centralAngle + 0.5f;
            v = (layer->upperAngle - phi) / span;
            if (v < 0.0f || v > 1.0f) {
                return false;
            }
            // Texels an output pixel spans: its angle over a texel's.
            ratio = pixelAngle * 0.5f * (float(rw) / layer->centralAngle + float(rh) / span);
            break;
        }
        }
        const float wanted = std::clamp(std::log2(std::max(ratio, 1e-6f)), 0.0f, float(levelCount() - 1));
        const int l0 = int(wanted);
        const int l1 = std::min(l0 + 1, int(levelCount()) - 1);
        const float f = wanted - float(l0);
        bilinear(level(size_t(l0)), u, v, wrapX, out);
        if (f > 0.001f && l1 != l0) {
            float other[4];
            bilinear(level(size_t(l1)), u, v, wrapX, other);
            for (int c = 0; c < 4; ++c) {
                out[c] += (other[c] - out[c]) * f;
            }
        }
        if (!layer->blend) {
            out[3] = 1.0f;
        }
        for (int c = 0; c < 4; ++c) {
            out[c] = std::max(0.0f, out[c] * layer->colorScale[c] + layer->colorBias[c]);
        }
        if (!layer->blend) {
            out[3] = 1.0f;
        }
        return true;
    }
};

Spatial::Quat identityIfZero(Spatial::Quat q)
{
    const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return n > 1e-6f ? Spatial::Quat{q.x / n, q.y / n, q.z / n, q.w / n} : Spatial::Quat{};
}

} // namespace

Texture render(const Camera &camera, const std::vector<Layer> &layers, int threads)
{
    Texture out;
    out.width = camera.width;
    out.height = camera.height;
    out.rgba.assign(size_t(out.width) * out.height * 4, 0.0f);
    for (size_t i = 3; i < out.rgba.size(); i += 4) {
        out.rgba[i] = 1.0f;
    }
    const Spatial::Pose eye{identityIfZero(camera.pose.orientation), camera.pose.position};

    std::vector<Prepared> prepared;
    prepared.reserve(layers.size());
    for (const Layer &layer : layers) {
        if (!layer.texture || layer.texture->width <= 0 || layer.texture->height <= 0) {
            continue;
        }
        const Texture &texture = *layer.texture;
        int x = std::clamp(layer.rectX, 0, texture.width - 1);
        int y = std::clamp(layer.rectY, 0, texture.height - 1);
        int w = layer.rectWidth > 0 ? layer.rectWidth : texture.width;
        int h = layer.rectHeight > 0 ? layer.rectHeight : texture.height;
        w = std::clamp(w, 1, texture.width - x);
        h = std::clamp(h, 1, texture.height - y);
        prepared.emplace_back(); // in place: the reserve keeps the addresses the levels point to
        Prepared &p = prepared.back();
        p.layer = &layer;
        const Spatial::Pose world = layer.headLocked ? eye * layer.pose : layer.pose;
        const Spatial::Pose inverse = world.inverse();
        p.origin = inverse.transform(eye.position);
        // The rows of the rotation to the layer's space: its columns are the images of the world axes.
        const Vec3 ex = inverse.orientation.rotate({1.0f, 0.0f, 0.0f});
        const Vec3 ey = inverse.orientation.rotate({0.0f, 1.0f, 0.0f});
        const Vec3 ez = inverse.orientation.rotate({0.0f, 0.0f, 1.0f});
        p.rows[0] = {ex.x, ey.x, ez.x};
        p.rows[1] = {ex.y, ey.y, ez.y};
        p.rows[2] = {ex.z, ey.z, ez.z};
        p.wrapX = layer.kind == Kind::Equirect && layer.centralAngle >= 2.0f * kPi - 1e-3f;
        if (w == texture.width && h == texture.height) {
            p.base = &texture;
        } else {
            p.cut = cropped(texture, x, y, w, h);
            p.base = &p.cut;
        }
        const Texture *last = p.base;
        while (last->width > 1 || last->height > 1) {
            p.rest.push_back(halved(*last));
            last = &p.rest.back();
        }
    }

    const float tanHalf = std::tan(camera.horizontalFovDegrees * kPi / 360.0f);
    const float pixelAngle = 2.0f * tanHalf / float(camera.width);
    const float aspect = float(camera.height) / float(camera.width);
    const int count = std::max(1, threads > 0 ? threads : int(std::thread::hardware_concurrency()));
    //! @p body(i) for i in [0, n), spread over the threads.
    const auto parallel = [count](int n, const auto &body) {
        std::atomic<int> next{0};
        const auto work = [&] {
            for (int i = next++; i < n; i = next++) {
                body(i);
            }
        };
        std::vector<std::thread> pool;
        for (int t = 1; t < std::min(count, n); ++t) {
            pool.emplace_back(work);
        }
        work();
        for (std::thread &t : pool) {
            t.join();
        }
    };
    const auto rayAt = [&](int px, int py) {
        const float ny = 1.0f - (float(py) + 0.5f) / float(camera.height) * 2.0f;
        const float nx = (float(px) + 0.5f) / float(camera.width) * 2.0f - 1.0f;
        return eye.orientation.rotate(Vec3{nx * tanHalf, ny * tanHalf * aspect, -1.0f}.normalized());
    };
    // Premultiplied "over": the layer's colour plus what is behind it, through the layer's alpha.
    const auto blendOver = [](const Prepared &p, const float c[4], float *pixel) {
        const float behind = p.layer->blend ? 1.0f - std::clamp(c[3], 0.0f, 1.0f) : 0.0f;
        for (int k = 0; k < 3; ++k) {
            pixel[k] = c[k] + pixel[k] * behind;
        }
    };

    // Back to front, a layer at a time (the blur of a glass layer needs everything behind it done).
    std::vector<float> layerPixels; //!< a glass layer's colours
    std::vector<uchar> covered; //!< and where it covers the picture
    std::vector<float> blurred; //!< the region being blurred, 3 floats a pixel
    std::vector<float> scratch;
    for (const Prepared &p : prepared) {
        // The runtime blurs at a quarter of the resolution: the standard deviation (the angle at most 0.06, in pixels
        // across the view) there is between half a pixel and 16, so 2 to 64 pixels here.
        const float sigma = std::clamp(std::min(p.layer->blurRadians, 0.06f) / pixelAngle / 4.0f, 0.5f, 16.0f) * 4.0f;
        if (p.layer->blurRadians <= 0.0f) {
            parallel(camera.height, [&](int py) {
                for (int px = 0; px < camera.width; ++px) {
                    float c[4];
                    if (p.sample(rayAt(px, py), pixelAngle, c)) {
                        blendOver(p, c, &out.rgba[(size_t(py) * camera.width + px) * 4]);
                    }
                }
            });
            continue;
        }
        // Where the layer covers the picture, and its colours there.
        layerPixels.assign(size_t(camera.width) * camera.height * 4, 0.0f);
        covered.assign(size_t(camera.width) * camera.height, 0);
        std::vector<int> rowFirst(size_t(camera.height), camera.width);
        std::vector<int> rowLast(size_t(camera.height), -1);
        parallel(camera.height, [&](int py) {
            for (int px = 0; px < camera.width; ++px) {
                const size_t i = size_t(py) * camera.width + px;
                if (p.sample(rayAt(px, py), pixelAngle, &layerPixels[i * 4])) {
                    covered[i] = 1;
                    rowFirst[size_t(py)] = std::min(rowFirst[size_t(py)], px);
                    rowLast[size_t(py)] = std::max(rowLast[size_t(py)], px);
                }
            }
        });
        int x0 = camera.width, x1 = -1, y0 = camera.height, y1 = -1;
        for (int py = 0; py < camera.height; ++py) {
            if (rowLast[size_t(py)] >= 0) {
                x0 = std::min(x0, rowFirst[size_t(py)]);
                x1 = std::max(x1, rowLast[size_t(py)]);
                y0 = std::min(y0, py);
                y1 = py;
            }
        }
        if (x1 >= 0) {
            // The region to blur: the layer's box and three deviations round it (what the blur there takes in).
            const int margin = int(std::ceil(3.0f * sigma));
            const int rx0 = std::max(0, x0 - margin);
            const int ry0 = std::max(0, y0 - margin);
            const int rx1 = std::min(camera.width - 1, x1 + margin);
            const int ry1 = std::min(camera.height - 1, y1 + margin);
            const int rw = rx1 - rx0 + 1;
            const int rh = ry1 - ry0 + 1;
            blurred.resize(size_t(rw) * rh * 3);
            scratch.resize(blurred.size());
            for (int y = 0; y < rh; ++y) {
                for (int x = 0; x < rw; ++x) {
                    const float *s = &out.rgba[(size_t(ry0 + y) * camera.width + rx0 + x) * 4];
                    std::copy_n(s, 3, &blurred[(size_t(y) * rw + x) * 3]);
                }
            }
            // A Gaussian as three box blurs a direction, running sums. A box of odd width w has variance (w^2 - 1) / 12:
            // the first m boxes take the odd width just under the ideal one, the rest the one above it, which together
            // come to sigma^2 (the standard deviation to within a hundredth of a pixel).
            const float ideal = std::sqrt(4.0f * sigma * sigma + 1.0f);
            int narrow = int(std::floor(ideal));
            narrow -= (narrow % 2 == 0) ? 1 : 0;
            const float m = (12.0f * sigma * sigma - 3.0f * float(narrow * narrow) - 12.0f * float(narrow) - 9.0f) / (-4.0f * float(narrow) - 4.0f);
            const int narrowPasses = std::clamp(int(std::lround(m)), 0, 3);
            const auto radiusOf = [&](int pass) { return ((pass < narrowPasses ? narrow : narrow + 2) - 1) / 2; };
            const auto box = [](int radius, const float *in, float *dest, int length, size_t step) {
                const float norm = 1.0f / float(2 * radius + 1);
                for (int k = 0; k < 3; ++k) {
                    float sum = 0.0f;
                    for (int i = -radius; i <= radius; ++i) {
                        sum += in[size_t(std::clamp(i, 0, length - 1)) * step + k];
                    }
                    for (int i = 0; i < length; ++i) {
                        dest[size_t(i) * step + k] = sum * norm;
                        sum += in[size_t(std::min(i + radius + 1, length - 1)) * step + k] - in[size_t(std::max(i - radius, 0)) * step + k];
                    }
                }
            };
            for (int pass = 0; pass < 3; ++pass) {
                parallel(rh, [&](int y) { // along the rows, through scratch and back
                    box(radiusOf(pass), &blurred[size_t(y) * rw * 3], &scratch[size_t(y) * rw * 3], rw, 3);
                });
                blurred.swap(scratch);
            }
            for (int pass = 0; pass < 3; ++pass) {
                parallel(rw, [&](int x) { // down the columns
                    box(radiusOf(pass), &blurred[size_t(x) * 3], &scratch[size_t(x) * 3], rh, size_t(rw) * 3);
                });
                blurred.swap(scratch);
            }
            parallel(rh, [&](int y) {
                for (int x = 0; x < rw; ++x) {
                    const size_t i = size_t(ry0 + y) * camera.width + rx0 + x;
                    if (!covered[i]) {
                        continue;
                    }
                    float *pixel = &out.rgba[i * 4];
                    // The layer's alpha over the mask alpha: how much of the frosted backdrop replaces the picture.
                    const float weight = std::clamp(layerPixels[i * 4 + 3] / std::max(p.layer->blurMaskAlpha, 1e-6f), 0.0f, 1.0f);
                    if (weight > 0.0f) {
                        const float *b = &blurred[(size_t(y) * rw + x) * 3];
                        const float luma = 0.2126f * b[0] + 0.7152f * b[1] + 0.0722f * b[2];
                        const float tint = p.layer->blurTint[3];
                        for (int k = 0; k < 3; ++k) {
                            const float saturated = std::max(luma + (b[k] - luma) * p.layer->blurSaturation, 0.0f);
                            const float frosted = saturated + (p.layer->blurTint[k] - saturated) * tint;
                            pixel[k] += (frosted - pixel[k]) * weight;
                        }
                    }
                    blendOver(p, &layerPixels[i * 4], pixel);
                }
            });
        }
    }
    return out;
}

float rollOff(float value)
{
    if (value <= kKnee) {
        return value;
    }
    // Reinhard's curve from the knee on, with the knee's slope (1): never above 1, no corner where it starts.
    const float x = (value - kKnee) / (1.0f - kKnee);
    return kKnee + (1.0f - kKnee) * x / (1.0f + x);
}

QImage toSrgb8(const Texture &image, bool dither)
{
    QImage out(image.width, image.height, QImage::Format_RGB888);
    for (int y = 0; y < image.height; ++y) {
        uchar *row = out.scanLine(y);
        for (int x = 0; x < image.width; ++x) {
            const float *p = &image.rgba[(size_t(y) * image.width + x) * 4];
            float c[3] = {std::max(p[0], 0.0f), std::max(p[1], 0.0f), std::max(p[2], 0.0f)};
            const float peak = std::max({c[0], c[1], c[2]});
            if (peak > kKnee) {
                const float scale = rollOff(peak) / peak;
                for (float &v : c) {
                    v *= scale;
                }
            }
            // A fixed hash of the position: a third of a step either way, so smooth gradients do not band.
            float noise = 0.0f;
            if (dither) {
                uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u;
                h = (h ^ (h >> 13)) * 1274126177u;
                noise = (float((h ^ (h >> 16)) & 0xFFFF) / 65535.0f - 0.5f);
            }
            for (int k = 0; k < 3; ++k) {
                row[x * 3 + k] = uchar(std::clamp(int(std::lround(linearToSrgb(c[k]) * 255.0f + noise)), 0, 255));
            }
        }
    }
    return out;
}

float pqToNits(float encoded)
{
    constexpr float m1 = 2610.0f / 16384.0f;
    constexpr float m2 = 2523.0f / 4096.0f * 128.0f;
    constexpr float c1 = 3424.0f / 4096.0f;
    constexpr float c2 = 2413.0f / 4096.0f * 32.0f;
    constexpr float c3 = 2392.0f / 4096.0f * 32.0f;
    const float e = std::pow(std::clamp(encoded, 0.0f, 1.0f), 1.0f / m2);
    const float numerator = std::max(e - c1, 0.0f);
    const float denominator = c2 - c3 * e;
    return denominator > 0.0f ? 10000.0f * std::pow(numerator / denominator, 1.0f / m1) : 0.0f;
}

Texture decodePixels(const void *data, int width, int height, const Decode &decode, long long maxPixels)
{
    Texture t;
    if (!data || width <= 0 || height <= 0) {
        return t;
    }
    int factor = 1;
    while (static_cast<long long>(width / factor) * (height / factor) > maxPixels && width / (factor + 1) > 0 && height / (factor + 1) > 0) {
        ++factor;
    }
    t.width = width / factor;
    t.height = height / factor;
    t.rgba.assign(size_t(t.width) * t.height * 4, 0.0f);
    const float white = decode.referenceWhiteNits > 0.0f ? decode.referenceWhiteNits : 203.0f;
    // BT.2020 to BT.709 (linear light), clipped at 0.
    static constexpr float m[3][3] = {{1.6605f, -0.5876f, -0.0728f}, {-0.1246f, 1.1329f, -0.0083f}, {-0.0182f, -0.1006f, 1.1187f}};
    const bool half = decode.encoding == Encoding::HalfLinear || decode.encoding == Encoding::HalfPq;
    // The 8-bit curve once for all 256 values.
    float srgbTable[256];
    for (int i = 0; i < 256; ++i) {
        srgbTable[i] = decode.encoding == Encoding::Srgb8 ? srgbToLinear(float(i) / 255.0f) : float(i) / 255.0f;
    }
    const auto pixel = [&](int x, int y, float out[4]) {
        const size_t index = (size_t(y) * width + x) * 4;
        if (half) {
            const qfloat16 *h = static_cast<const qfloat16 *>(data) + index;
            const float a = std::clamp(float(h[3]), 0.0f, 1.0f);
            float rgb[3] = {float(h[0]), float(h[1]), float(h[2])};
            if (decode.encoding == Encoding::HalfPq) {
                for (float &v : rgb) {
                    v = a > 0.0f ? pqToNits(v / a) / white * a : 0.0f; // the encoded colour, then back under the alpha
                }
            }
            if (decode.bt2020) {
                const float in[3] = {rgb[0], rgb[1], rgb[2]};
                for (int r = 0; r < 3; ++r) {
                    rgb[r] = std::max(0.0f, m[r][0] * in[0] + m[r][1] * in[1] + m[r][2] * in[2]);
                }
            }
            out[0] = rgb[0];
            out[1] = rgb[1];
            out[2] = rgb[2];
            out[3] = a;
        } else {
            const uchar *b = static_cast<const uchar *>(data) + index;
            out[0] = srgbTable[b[0]];
            out[1] = srgbTable[b[1]];
            out[2] = srgbTable[b[2]];
            out[3] = float(b[3]) / 255.0f;
            if (decode.bt2020) {
                const float in[3] = {out[0], out[1], out[2]};
                for (int r = 0; r < 3; ++r) {
                    out[r] = std::max(0.0f, m[r][0] * in[0] + m[r][1] * in[1] + m[r][2] * in[2]);
                }
            }
        }
        // Not finite (a half float's overflow): the brightest a layer can be.
        for (int c = 0; c < 4; ++c) {
            if (!std::isfinite(out[c])) {
                out[c] = c == 3 ? 1.0f : 65504.0f;
            }
        }
    };
    const float weight = 1.0f / float(factor * factor);
    for (int y = 0; y < t.height; ++y) {
        for (int x = 0; x < t.width; ++x) {
            float sum[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            for (int dy = 0; dy < factor; ++dy) {
                for (int dx = 0; dx < factor; ++dx) {
                    float c[4];
                    pixel(x * factor + dx, y * factor + dy, c);
                    for (int k = 0; k < 4; ++k) {
                        sum[k] += c[k];
                    }
                }
            }
            for (int k = 0; k < 4; ++k) {
                t.rgba[(size_t(y) * t.width + x) * 4 + k] = sum[k] * weight;
            }
        }
    }
    return t;
}

} // namespace RiftHome::Xr::ViewShot
