// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#include "xr/ViewShot.hpp"

#include <QtTest>

#include <cmath>

using namespace RiftHome::Xr::ViewShot;
using RiftHome::Spatial::Pose;
using RiftHome::Spatial::Quat;
using RiftHome::Spatial::Vec3;

namespace
{

constexpr float kPi = 3.14159265358979f;

std::shared_ptr<const Texture> solid(float r, float g, float b, float a = 1.0f, int size = 8)
{
    auto t = std::make_shared<Texture>();
    t->width = size;
    t->height = size;
    for (int i = 0; i < size * size; ++i) {
        t->rgba.insert(t->rgba.end(), {r * a, g * a, b * a, a}); // premultiplied
    }
    return t;
}

//! Left half red, right half blue; top row green: tells orientation.
std::shared_ptr<const Texture> halves()
{
    auto t = std::make_shared<Texture>();
    t->width = 16;
    t->height = 16;
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            if (y < 2) {
                t->rgba.insert(t->rgba.end(), {0.0f, 1.0f, 0.0f, 1.0f});
            } else if (x < 8) {
                t->rgba.insert(t->rgba.end(), {1.0f, 0.0f, 0.0f, 1.0f});
            } else {
                t->rgba.insert(t->rgba.end(), {0.0f, 0.0f, 1.0f, 1.0f});
            }
        }
    }
    return t;
}

Layer quad(float width, float height, Vec3 at, std::shared_ptr<const Texture> texture)
{
    Layer l;
    l.kind = Kind::Quad;
    l.pose = {Quat{}, at};
    l.width = width;
    l.height = height;
    l.texture = std::move(texture);
    return l;
}

Camera camera(int width = 400, int height = 250, float fov = 100.0f)
{
    Camera c;
    c.horizontalFovDegrees = fov;
    c.width = width;
    c.height = height;
    return c;
}

const float *at(const Texture &t, int x, int y)
{
    return &t.rgba[(size_t(y) * t.width + x) * 4];
}

bool lit(const Texture &t, int x, int y)
{
    const float *p = at(t, x, y);
    return p[0] + p[1] + p[2] > 0.5f;
}

} // namespace

class TestViewShot : public QObject
{
    Q_OBJECT
private slots:
    void quadAheadIsCentredAndSized()
    {
        // 2 m wide, 1 m high, 1 m ahead: at 100 degrees (tan 50 = 1.19175) it spans 2 / (2 * 1.19175) of the width.
        const Texture shot = render(camera(), {quad(2.0f, 1.0f, {0, 0, -1}, solid(1, 1, 1))});
        int left = shot.width, right = -1, top = shot.height, bottom = -1;
        for (int y = 0; y < shot.height; ++y) {
            for (int x = 0; x < shot.width; ++x) {
                if (lit(shot, x, y)) {
                    left = std::min(left, x);
                    right = std::max(right, x);
                    top = std::min(top, y);
                    bottom = std::max(bottom, y);
                }
            }
        }
        const float expectedWidth = shot.width * 2.0f / (2.0f * std::tan(50.0f * kPi / 180.0f));
        QVERIFY(std::abs((right - left + 1) - expectedWidth) <= 2.0f);
        QVERIFY(std::abs((right + left) / 2.0f - (shot.width - 1) / 2.0f) <= 1.0f);
        QVERIFY(std::abs((bottom + top) / 2.0f - (shot.height - 1) / 2.0f) <= 1.0f);
        // 1 m high at 1 m: its height in pixels is the width's half (one pixel is the same angle either way, near the centre).
        QVERIFY(std::abs((bottom - top + 1) - expectedWidth / 2.0f) <= 3.0f);
    }

    void quadOrientation()
    {
        const Texture shot = render(camera(), {quad(1.0f, 1.0f, {0, 0, -1}, halves())});
        const int cy = shot.height / 2;
        QVERIFY(at(shot, shot.width / 2 - 20, cy)[0] > 0.9f); // left: red
        QVERIFY(at(shot, shot.width / 2 + 20, cy)[2] > 0.9f); // right: blue
        QVERIFY(at(shot, shot.width / 2, shot.height / 2 - 75)[1] > 0.9f); // the top edge: green (top rows)
        QVERIFY(!lit(shot, 5, 5)); // nothing around it
    }

    void quadSeenFromBehindIsNotDrawn()
    {
        Layer l = quad(2.0f, 2.0f, {0, 0, -1}, solid(1, 1, 1));
        l.pose.orientation = Quat::fromYaw(kPi); // its front faces away
        const Texture shot = render(camera(), {l});
        QVERIFY(!lit(shot, shot.width / 2, shot.height / 2));
    }

    void cameraMovesAndTurns()
    {
        Camera c = camera();
        c.pose = {Quat::fromYaw(-kPi / 2.0f), {1, 0, 0}}; // at x = 1, looking towards +X
        Layer l = quad(1.0f, 1.0f, {3, 0, 0}, solid(1, 1, 1));
        l.pose.orientation = Quat::fromYaw(-kPi / 2.0f); // facing -X, back at the camera
        const Texture shot = render(c, {l});
        QVERIFY(lit(shot, shot.width / 2, shot.height / 2));
        QVERIFY(!lit(shot, 10, shot.height / 2));
    }

    void headLockedLayerFollowsTheCamera()
    {
        Camera c = camera();
        c.pose = {Quat::fromYaw(0.7f), {5, 1, 5}};
        Layer l = quad(1.0f, 1.0f, {0, 0, -1}, solid(1, 1, 1));
        l.headLocked = true;
        const Texture shot = render(c, {l});
        QVERIFY(lit(shot, shot.width / 2, shot.height / 2));
    }

    void cylinderCentreAndEdges()
    {
        // 90 degrees of a 2 m cylinder around the camera, aspect 2: its height is r * angle / 2.
        Layer l;
        l.kind = Kind::Cylinder;
        l.pose = {Quat{}, {0, 0, 0}};
        l.radius = 2.0f;
        l.centralAngle = kPi / 2.0f;
        l.aspectRatio = 2.0f;
        l.texture = halves();
        const Camera c = camera(800, 500, 150.0f);
        const Texture shot = render(c, {l});
        // The picture's centre column is straight ahead: the seam between red and blue.
        const int cy = c.height / 2;
        QVERIFY(at(shot, c.width / 2 - 20, cy)[0] > 0.9f);
        QVERIFY(at(shot, c.width / 2 + 20, cy)[2] > 0.9f);
        // The edges at +-45 degrees: x = tan(45) / tan(75) of the half width off centre.
        const float edge = std::tan(kPi / 4.0f) / std::tan(75.0f * kPi / 180.0f) * (c.width / 2.0f);
        int left = c.width, right = -1;
        for (int x = 0; x < c.width; ++x) {
            if (lit(shot, x, cy)) {
                left = std::min(left, x);
                right = std::max(right, x);
            }
        }
        QVERIFY(std::abs((c.width / 2.0f - left) - edge) <= 2.0f);
        QVERIFY(std::abs((right + 1 - c.width / 2.0f) - edge) <= 2.0f);
        // Its height: r * angle / aspect = 1.5708 m on the 2 m wall: +-0.7854 m, the top edge at 0.7854 / 2 on the picture plane.
        int top = c.height;
        for (int y = 0; y < c.height; ++y) {
            if (lit(shot, c.width / 2, y)) {
                top = y;
                break;
            }
        }
        const float tanHalf = std::tan(75.0f * kPi / 180.0f);
        const float expectedTop = c.height / 2.0f - (0.7854f / 2.0f) / tanHalf * (c.width / 2.0f);
        QVERIFY(std::abs(top - expectedTop) <= 2.0f);
    }

    void cylinderEdgesRiseTowardsTheSides()
    {
        // Seen from its axis, the wall's top edge is a constant height: on the picture it rises by 1 / cos(angle) at the
        // sides (screen y = h / (r cos a)), where a flat quad's would not.
        Layer l;
        l.kind = Kind::Cylinder;
        l.pose = {Quat{}, {0, 0, 0}};
        l.radius = 2.0f;
        l.centralAngle = kPi / 2.0f;
        l.aspectRatio = 2.0f;
        l.texture = solid(1, 1, 1);
        const Camera c = camera(800, 500, 150.0f);
        const Texture shot = render(c, {l});
        const float tanHalf = std::tan(75.0f * kPi / 180.0f);
        const auto topOf = [&](int x) {
            for (int y = 0; y < c.height; ++y) {
                if (lit(shot, x, y)) {
                    return y;
                }
            }
            return c.height;
        };
        const int column = c.width / 2 - 100;
        const float angle = std::atan((c.width / 2.0f - column - 0.5f) / (c.width / 2.0f) * tanHalf);
        const float expected = c.height / 2.0f - (0.7854f / (2.0f * std::cos(angle))) / tanHalf * (c.width / 2.0f);
        QVERIFY(topOf(column) < topOf(c.width / 2));
        QVERIFY(std::abs(topOf(column) - expected) <= 2.0f);
    }

    void cylinderOffAxisPlacement()
    {
        // A window's arc: the cylinder's axis one radius in front of the window's surface, the pose turned to x.
        const float radius = 1.6f;
        Layer l;
        l.kind = Kind::Cylinder;
        l.pose = {Quat{}, {0, 0, -1.6f + radius}}; // the axis at the camera: the arc's middle 1.6 m ahead... on the axis here
        l.radius = radius;
        l.centralAngle = 1.0f;
        l.aspectRatio = 2.0f;
        l.texture = solid(1, 1, 1);
        const Texture shot = render(camera(), {l});
        QVERIFY(lit(shot, shot.width / 2, shot.height / 2));
        // Turned a quarter turn to the right (about +Y by -90 degrees): the arc is on the right-hand side.
        l.pose.orientation = Quat::fromYaw(-kPi / 2.0f);
        const Texture right = render(camera(400, 250, 170.0f), {l});
        QVERIFY(lit(right, right.width - 60, right.height / 2));
        QVERIFY(!lit(right, 30, right.height / 2));
    }

    void equirectForwardIsTheCentre()
    {
        auto t = std::make_shared<Texture>();
        t->width = 64;
        t->height = 32;
        for (int y = 0; y < 32; ++y) {
            for (int x = 0; x < 64; ++x) {
                // red grows to the right, green grows downwards
                t->rgba.insert(t->rgba.end(), {float(x) / 63.0f, float(y) / 31.0f, 0.0f, 1.0f});
            }
        }
        Layer l;
        l.kind = Kind::Equirect;
        l.pose = {Quat{}, {0, 0, 0}};
        l.radius = 0.0f;
        l.centralAngle = 2.0f * kPi;
        l.upperAngle = kPi / 2.0f;
        l.lowerAngle = -kPi / 2.0f;
        l.texture = t;
        const Camera c = camera(200, 125, 90.0f);
        const Texture shot = render(c, {l});
        const float *centre = at(shot, c.width / 2, c.height / 2);
        QVERIFY(std::abs(centre[0] - 0.5f) < 0.03f);
        QVERIFY(std::abs(centre[1] - 0.5f) < 0.03f);
        // Looking right (yaw -90): the image's three-quarter column.
        Camera right = c;
        right.pose.orientation = Quat::fromYaw(-kPi / 2.0f);
        QVERIFY(std::abs(at(render(right, {l}), c.width / 2, c.height / 2)[0] - 0.75f) < 0.03f);
        // Looking up: the top of the picture.
        Camera up = c;
        up.pose.orientation = Quat::fromPitch(1.2f);
        QVERIFY(at(render(up, {l}), c.width / 2, c.height / 2)[1] < 0.15f);
        // Behind: the seam, wrapping round: the picture's left edge on one side, its right on the other.
        Camera back = c;
        back.pose.orientation = Quat::fromYaw(kPi - 0.2f);
        QVERIFY(at(render(back, {l}), c.width / 2, c.height / 2)[0] < 0.1f);
        back.pose.orientation = Quat::fromYaw(kPi + 0.2f);
        QVERIFY(at(render(back, {l}), c.width / 2, c.height / 2)[0] > 0.9f);
    }

    void equirectWithARadiusSeesTheCameraOffCentre()
    {
        Layer l;
        l.kind = Kind::Equirect;
        l.pose = {Quat{}, {0, 0, 0}};
        l.radius = 3.0f;
        l.centralAngle = 2.0f * kPi;
        l.upperAngle = kPi / 2.0f;
        l.lowerAngle = -kPi / 2.0f;
        l.texture = solid(0.2f, 0.4f, 0.6f);
        Camera c = camera(50, 30);
        c.pose.position = {1.0f, 0.5f, 0.0f};
        const Texture shot = render(c, {l});
        QVERIFY(std::abs(at(shot, 25, 15)[2] - 0.6f) < 0.01f);
    }

    void alphaBlendsInOrder()
    {
        // Opaque red behind; half-transparent blue (premultiplied: 0, 0, 0.5, 0.5) over it: 0.5 red, 0.5 blue.
        Layer back = quad(2.0f, 2.0f, {0, 0, -2}, solid(1, 0, 0));
        Layer front = quad(2.0f, 2.0f, {0, 0, -1}, solid(0, 0, 1, 0.5f));
        front.blend = true;
        const Texture shot = render(camera(), {back, front});
        const float *p = at(shot, shot.width / 2, shot.height / 2);
        QVERIFY(std::abs(p[0] - 0.5f) < 0.01f);
        QVERIFY(std::abs(p[2] - 0.5f) < 0.01f);
        // The other way: the red is opaque and covers the blue.
        const Texture covered = render(camera(), {front, back});
        const float *q = at(covered, covered.width / 2, covered.height / 2);
        QVERIFY(std::abs(q[0] - 1.0f) < 0.01f);
        QVERIFY(q[2] < 0.01f);
        // Without the blend flag, alpha is ignored: the layer is opaque.
        front.blend = false;
        const Texture opaque = render(camera(), {back, front});
        QVERIFY(std::abs(at(opaque, opaque.width / 2, opaque.height / 2)[2] - 0.5f) < 0.01f);
        QVERIFY(at(opaque, opaque.width / 2, opaque.height / 2)[0] < 0.01f);
    }

    void colourScaleDims()
    {
        Layer l = quad(2.0f, 2.0f, {0, 0, -1}, solid(0.8f, 0.8f, 0.8f));
        l.colorScale[0] = l.colorScale[1] = l.colorScale[2] = 0.25f;
        const Texture shot = render(camera(), {l});
        QVERIFY(std::abs(at(shot, shot.width / 2, shot.height / 2)[1] - 0.2f) < 0.005f);
    }

    void subRectangleShowsPartOfTheTexture()
    {
        Layer l = quad(1.0f, 1.0f, {0, 0, -1}, halves());
        l.rectX = 8;
        l.rectY = 4;
        l.rectWidth = 8;
        l.rectHeight = 8; // the blue part
        const Texture shot = render(camera(), {l});
        const float *p = at(shot, shot.width / 2 - 10, shot.height / 2);
        QVERIFY(p[2] > 0.95f && p[0] < 0.05f);
    }

    //! A black and white vertical stripe pattern (32 texels a stripe on a 512 texel picture: edges sharp at the picture's scale) behind the glass.
    static std::shared_ptr<const Texture> stripes()
    {
        auto t = std::make_shared<Texture>();
        t->width = 512;
        t->height = 8;
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 512; ++x) {
                const float v = (x / 32) % 2 ? 1.0f : 0.0f;
                t->rgba.insert(t->rgba.end(), {v, v, v, 1.0f});
            }
        }
        return t;
    }

    //! The largest step between neighbouring pixels along a row, over [from, to).
    static float sharpest(const Texture &t, int y, int from, int to)
    {
        float most = 0.0f;
        for (int x = from; x + 1 < to; ++x) {
            most = std::max(most, std::abs(at(t, x + 1, y)[0] - at(t, x, y)[0]));
        }
        return most;
    }

    void glassBlursWhatIsBehindItWhereItCovers()
    {
        Layer back = quad(4.0f, 2.0f, {0, 0, -2}, stripes());
        Layer glass = quad(1.0f, 1.0f, {0, 0, -1}, solid(1, 1, 1, 0.01f)); // all but clear, but covering (the runtime blurs by the layer's alpha)
        glass.blend = true;
        glass.blurMaskAlpha = 0.01f;
        glass.blurRadians = 0.02f;
        const Camera c = camera(800, 500, 100.0f);
        const Texture shot = render(c, {back, glass});
        const Texture sharp = render(c, {back});
        const int y = c.height / 2;
        // The glass spans 1 m at 1 m: about 335 px wide, centred. Inside it the stripes are blurred, outside as before.
        QVERIFY(sharpest(sharp, y, 300, 500) > 0.4f);
        QVERIFY(sharpest(shot, y, 300, 500) < 0.15f);
        for (int x = 0; x < 200; ++x) {
            QCOMPARE(at(shot, x, y)[0], at(sharp, x, y)[0]);
        }
    }

    void glassBlurWidthFollowsTheRadians()
    {
        // A black / white edge behind the glass: the 10-90 % spread of a Gaussian edge is 2.56 standard deviations.
        auto edge = std::make_shared<Texture>();
        edge->width = 2048;
        edge->height = 4;
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 2048; ++x) {
                const float v = x < 1024 ? 0.0f : 1.0f;
                edge->rgba.insert(edge->rgba.end(), {v, v, v, 1.0f});
            }
        }
        const Camera c = camera(1000, 600, 100.0f);
        const float pixelAngle = 2.0f * std::tan(50.0f * kPi / 180.0f) / 1000.0f;
        for (float sigmaPixels : {6.0f, 12.0f}) {
            Layer back = quad(8.0f, 4.0f, {0, 0, -2}, edge);
            Layer glass = quad(3.0f, 2.0f, {0, 0, -1}, solid(1, 1, 1, 0.01f));
            glass.blend = true;
            glass.blurMaskAlpha = 0.01f;
            glass.blurRadians = sigmaPixels * pixelAngle;
            const Texture shot = render(c, {back, glass});
            const int y = c.height / 2;
            float lo = -1.0f, hi = -1.0f;
            for (int x = 300; x < 700; ++x) {
                const float v = at(shot, x, y)[0];
                if (lo < 0.0f && v > 0.1f) {
                    lo = float(x);
                }
                if (hi < 0.0f && v > 0.9f) {
                    hi = float(x);
                }
            }
            QVERIFY(lo > 0.0f && hi > 0.0f);
            QVERIFY(std::abs((hi - lo) / 2.56f - sigmaPixels) < 0.15f * sigmaPixels);
        }
    }

    void glassSaturationTintAndMask()
    {
        // A uniform backdrop (the blur leaves it as it is), RiftLinux's arithmetic by hand: luma 0.32756 of (0.8, 0.2, 0.2);
        // saturation 0.5 about the luma: (0.5638, 0.2638, 0.2638); the tint (0.1, 0.1, 0.3) over it by 0.25:
        // (0.4478, 0.2228, 0.2728); mixed in by alpha 0.2 over the mask alpha 0.4 = 0.5: (0.6239, 0.2114, 0.2364); then the
        // layer (black, alpha 0.2, premultiplied) over that: times 0.8.
        Layer back = quad(6.0f, 4.0f, {0, 0, -2}, solid(0.8f, 0.2f, 0.2f));
        Layer glass = quad(1.0f, 1.0f, {0, 0, -1}, solid(0, 0, 0, 0.2f));
        glass.blend = true;
        glass.blurRadians = 0.02f;
        glass.blurSaturation = 0.5f;
        glass.blurTint[0] = 0.1f;
        glass.blurTint[1] = 0.1f;
        glass.blurTint[2] = 0.3f;
        glass.blurTint[3] = 0.25f;
        glass.blurMaskAlpha = 0.4f;
        const Camera c = camera(400, 250, 100.0f);
        const Texture shot = render(c, {back, glass});
        const float *in = at(shot, c.width / 2, c.height / 2);
        QVERIFY(std::abs(in[0] - 0.6239f * 0.8f) < 0.003f);
        QVERIFY(std::abs(in[1] - 0.2114f * 0.8f) < 0.003f);
        QVERIFY(std::abs(in[2] - 0.2364f * 0.8f) < 0.003f);
        const float *out = at(shot, 20, 20); // outside the glass
        QVERIFY(std::abs(out[0] - 0.8f) < 1e-5f && std::abs(out[1] - 0.2f) < 1e-5f && std::abs(out[2] - 0.2f) < 1e-5f);
        // From the mask alpha up the frosting is full: (0.4478, 0.2228, 0.2728) under the layer's 0.6.
        glass.blurMaskAlpha = 0.2f;
        const float *full = at(render(c, {back, glass}), c.width / 2, c.height / 2);
        QVERIFY(std::abs(full[0] - 0.4478f * 0.8f) < 0.003f);
        // Where the layer is transparent nothing is frosted at all (corners stay sharp, as the picture behind).
        Layer clear = glass;
        clear.texture = solid(0, 0, 0, 0.0f);
        const float *none = at(render(c, {back, clear}), c.width / 2, c.height / 2);
        QVERIFY(std::abs(none[0] - 0.8f) < 1e-5f && std::abs(none[1] - 0.2f) < 1e-5f);
    }

    void clearLayersLeaveWhatIsBehindSharp()
    {
        Layer back = quad(4.0f, 2.0f, {0, 0, -2}, stripes());
        Layer front = quad(1.0f, 1.0f, {0, 0, -1}, solid(1, 1, 1, 0.0f));
        front.blend = true; // no blurRadians: not glass
        const Camera c = camera(800, 500, 100.0f);
        const Texture shot = render(c, {back, front});
        const Texture sharp = render(c, {back});
        QCOMPARE(shot.rgba, sharp.rgba);
    }

    void aSceneWithoutGlassIsDrawnPixelForPixelAsBefore()
    {
        // The same pixel computed by hand in the order given: opaque red, half blue, quarter green over.
        Layer a = quad(2.0f, 2.0f, {0, 0, -3}, solid(1, 0, 0));
        Layer b = quad(2.0f, 2.0f, {0, 0, -2}, solid(0, 0, 1, 0.5f));
        b.blend = true;
        Layer d = quad(2.0f, 2.0f, {0, 0, -1}, solid(0, 1, 0, 0.25f));
        d.blend = true;
        const Texture shot = render(camera(), {a, b, d}, 3);
        const float *p = at(shot, shot.width / 2, shot.height / 2);
        QVERIFY(std::abs(p[0] - 0.5f * 0.75f) < 1e-5f);
        QVERIFY(std::abs(p[1] - 0.25f) < 1e-5f);
        QVERIFY(std::abs(p[2] - 0.5f * 0.75f) < 1e-5f);
        QCOMPARE(render(camera(), {a, b, d}, 1).rgba, shot.rgba); // threads do not change it
    }

    void pqDecoding()
    {
        // The PQ code for 100 nits is 0.5081 (ST 2084).
        QVERIFY(std::abs(pqToNits(0.508078f) - 100.0f) < 0.5f);
        QVERIFY(std::abs(pqToNits(1.0f) - 10000.0f) < 1.0f);
        QCOMPARE(pqToNits(0.0f), 0.0f);
        // As a layer: half floats of that code, white 203: 100 / 203.
        const qfloat16 code(0.508078f);
        const qfloat16 pixel[4] = {code, code, code, qfloat16(1.0f)};
        const Texture t = decodePixels(pixel, 1, 1, {Encoding::HalfPq, 0.0f, false});
        QVERIFY(std::abs(t.rgba[0] - 100.0f / 203.0f) < 0.005f);
        // A layer that names its reference white.
        const Texture other = decodePixels(pixel, 1, 1, {Encoding::HalfPq, 100.0f, false});
        QVERIFY(std::abs(other.rgba[1] - 1.0f) < 0.01f);
    }

    void decodingTheOtherEncodings()
    {
        const uchar bytes[4] = {255, 128, 0, 255};
        const Texture srgb = decodePixels(bytes, 1, 1, {Encoding::Srgb8, 0.0f, false});
        QVERIFY(std::abs(srgb.rgba[0] - 1.0f) < 1e-4f);
        QVERIFY(std::abs(srgb.rgba[1] - 0.2158f) < 0.002f); // sRGB 128 is 21.6 % linear
        QCOMPARE(srgb.rgba[2], 0.0f);
        const qfloat16 half[4] = {qfloat16(3.0f), qfloat16(0.5f), qfloat16(0.0f), qfloat16(1.0f)};
        const Texture linear = decodePixels(half, 1, 1, {Encoding::HalfLinear, 0.0f, false});
        QCOMPARE(linear.rgba[0], 3.0f); // brighter than white stays
        QCOMPARE(linear.rgba[1], 0.5f);
        // Box filtered by a whole factor above the limit.
        const std::vector<uchar> big(8 * 8 * 4, 255);
        const Texture small = decodePixels(big.data(), 8, 8, {Encoding::Srgb8, 0.0f, false}, 15);
        QCOMPARE(small.width, 2);
        QCOMPARE(small.height, 2);
        QVERIFY(std::abs(small.rgba[0] - 1.0f) < 1e-4f);
    }

    void displayMappingKeepsAndRollsOff()
    {
        // Below the knee: unchanged. White stays within 3 % of white. Above: monotonic, below 1, no cliff.
        for (float v : {0.0f, 0.01f, 0.18f, 0.5f, 0.9f}) {
            QCOMPARE(rollOff(v), v);
        }
        QVERIFY(rollOff(1.0f) > 0.95f && rollOff(1.0f) < 1.0f);
        float previous = rollOff(0.9f);
        for (float v = 0.91f; v < 200.0f; v *= 1.05f) {
            const float m = rollOff(v);
            QVERIFY(m >= previous);
            QVERIFY(m <= 1.0f);
            previous = m;
        }
        // The step just over the knee is as small as just under it (no corner).
        QVERIFY(std::abs((rollOff(0.9501f) - rollOff(0.95f)) - 0.0001f) < 0.00002f);
        // As pixels: 18 % grey stays sRGB 118; 1.0 is near white; 50x is 255 at most; hue kept.
        Texture t;
        t.width = 3;
        t.height = 1;
        t.rgba = {0.18f, 0.18f, 0.18f, 1, 1.0f, 1.0f, 1.0f, 1, 50.0f, 10.0f, 5.0f, 1};
        const QImage image = toSrgb8(t, false);
        QCOMPARE(image.format(), QImage::Format_RGB888);
        QCOMPARE(qRed(image.pixel(0, 0)), 118);
        QVERIFY(qRed(image.pixel(1, 0)) >= 250 && qRed(image.pixel(1, 0)) <= 255);
        const QRgb hot = image.pixel(2, 0);
        QVERIFY(qRed(hot) <= 255 && qRed(hot) > qGreen(hot) && qGreen(hot) > qBlue(hot));
        QVERIFY(qRed(hot) >= 250);
    }
};

QTEST_APPLESS_MAIN(TestViewShot)
#include "tst_viewshot.moc"
