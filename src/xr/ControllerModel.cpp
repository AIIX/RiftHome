// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#include "xr/ControllerModel.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

Q_DECLARE_LOGGING_CATEGORY(lcXr)

namespace RiftHome::Xr
{

namespace
{

constexpr float kHeight = 0.094f; //!< metres from the handle's foot to the top of the head (a CV1 Touch without its ring)
constexpr float kGripBelowTop = 0.31f; //!< of the height: where RiftLinux's grip sits on the handle
constexpr float kAxisLow = 0.25f; //!< of the height above the foot: the handle's lower cross-section (above its rounded end)
//! The model's front (its trigger side, square to the handle) along the laser (else the rays left 20-30
//! degrees off it): RiftLinux's aim ray is 30 degrees from the grip's -Y, towards the handle's top.
constexpr float kFrontToAim = 30.0f;

using V3 = std::array<float, 3>;

V3 sub(V3 a, V3 b)
{
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
float dot(V3 a, V3 b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
V3 cross(V3 a, V3 b)
{
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
V3 normalized(V3 a)
{
    const float l = std::sqrt(dot(a, a));
    return l > 0.0f ? V3{a[0] / l, a[1] / l, a[2] / l} : a;
}

//! An accessor's elements as floats (VEC3 positions and normals) or indices (SCALAR, any unsigned type).
bool readAccessor(const QJsonObject &gltf, const QByteArray &bin, int index, int components, std::vector<float> *floats, std::vector<uint32_t> *ints)
{
    const QJsonArray accessors = gltf.value(QStringLiteral("accessors")).toArray();
    const QJsonArray views = gltf.value(QStringLiteral("bufferViews")).toArray();
    if (index < 0 || index >= accessors.size()) {
        return false;
    }
    const QJsonObject accessor = accessors.at(index).toObject();
    const int viewIndex = accessor.value(QStringLiteral("bufferView")).toInt(-1);
    if (viewIndex < 0 || viewIndex >= views.size()) {
        return false;
    }
    const QJsonObject view = views.at(viewIndex).toObject();
    const qint64 count = accessor.value(QStringLiteral("count")).toInteger();
    const int type = accessor.value(QStringLiteral("componentType")).toInt();
    const int size = type == 5126 || type == 5125 ? 4 : type == 5123 ? 2 : type == 5121 ? 1 : 0;
    const qint64 start = view.value(QStringLiteral("byteOffset")).toInteger() + accessor.value(QStringLiteral("byteOffset")).toInteger();
    const qint64 stride = view.value(QStringLiteral("byteStride")).toInteger(size * components);
    if (size == 0 || count <= 0 || start < 0 || start + stride * (count - 1) + size * components > bin.size()) {
        return false;
    }
    for (qint64 i = 0; i < count; ++i) {
        const char *element = bin.constData() + start + i * stride;
        for (int c = 0; c < components; ++c) {
            const char *p = element + c * size;
            if (floats && type == 5126) {
                float f;
                std::memcpy(&f, p, 4);
                floats->push_back(f);
            } else if (ints) {
                uint32_t v = 0;
                std::memcpy(&v, p, size_t(size)); // little-endian, as glTF
                ints->push_back(v);
            } else {
                return false;
            }
        }
    }
    return true;
}

struct Part {
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<uint32_t> indices;
    V3 colour{0.8f, 0.8f, 0.8f};
    float roughness = 1.0f; // glTF's defaults
    float metallic = 1.0f;
};

//! The middle of where the plane at height @p y cuts the part's triangles (x and z), or nothing.
std::optional<V3> crossSection(const Part &part, float y)
{
    float minX = 1e30f, maxX = -1e30f, minZ = 1e30f, maxZ = -1e30f;
    bool any = false;
    const auto at = [&part](uint32_t i) {
        return V3{part.positions[i * 3], part.positions[i * 3 + 1], part.positions[i * 3 + 2]};
    };
    for (size_t t = 0; t + 2 < part.indices.size(); t += 3) {
        const V3 v[3] = {at(part.indices[t]), at(part.indices[t + 1]), at(part.indices[t + 2])};
        for (int e = 0; e < 3; ++e) {
            const V3 &a = v[e];
            const V3 &b = v[(e + 1) % 3];
            if ((a[1] - y) * (b[1] - y) > 0.0f || a[1] == b[1]) {
                continue;
            }
            const float k = (y - a[1]) / (b[1] - a[1]);
            const float x = a[0] + k * (b[0] - a[0]);
            const float z = a[2] + k * (b[2] - a[2]);
            minX = std::min(minX, x), maxX = std::max(maxX, x), minZ = std::min(minZ, z), maxZ = std::max(maxZ, z);
            any = true;
        }
    }
    if (!any) {
        return std::nullopt;
    }
    return V3{(minX + maxX) / 2.0f, y, (minZ + maxZ) / 2.0f};
}

} // namespace

std::optional<ControllerMesh> loadControllerModel(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    const QByteArray data = file.readAll();
    const auto word = [&data](qint64 at) {
        uint32_t v = 0;
        if (at >= 0 && at + 4 <= data.size()) {
            std::memcpy(&v, data.constData() + at, 4);
        }
        return v;
    };
    if (data.size() < 28 || std::memcmp(data.constData(), "glTF", 4) != 0 || word(4) != 2) {
        qCWarning(lcXr).noquote() << "controllers:" << path << "is not a binary glTF 2.0 file";
        return std::nullopt;
    }
    const uint32_t jsonLength = word(12);
    if (word(16) != 0x4E4F534Au /* JSON */ || 20 + qint64(jsonLength) + 8 > data.size()) {
        return std::nullopt;
    }
    const QJsonObject gltf = QJsonDocument::fromJson(data.mid(20, jsonLength)).object();
    const qint64 binAt = 20 + qint64(jsonLength);
    const QByteArray bin = data.mid(binAt + 8, word(binAt));
    const QJsonArray materials = gltf.value(QStringLiteral("materials")).toArray();

    // Every triangle list with its material.
    std::vector<Part> parts;
    for (const QJsonValue &m : gltf.value(QStringLiteral("meshes")).toArray()) {
        for (const QJsonValue &p : m.toObject().value(QStringLiteral("primitives")).toArray()) {
            const QJsonObject primitive = p.toObject();
            if (primitive.value(QStringLiteral("mode")).toInt(4) != 4) {
                continue; // triangles only
            }
            const QJsonObject attributes = primitive.value(QStringLiteral("attributes")).toObject();
            Part part;
            if (!readAccessor(gltf, bin, attributes.value(QStringLiteral("POSITION")).toInt(-1), 3, &part.positions, nullptr)
                || !readAccessor(gltf, bin, attributes.value(QStringLiteral("NORMAL")).toInt(-1), 3, &part.normals, nullptr)
                || !readAccessor(gltf, bin, primitive.value(QStringLiteral("indices")).toInt(-1), 1, nullptr, &part.indices)
                || part.normals.size() != part.positions.size()) {
                continue;
            }
            const int material = primitive.value(QStringLiteral("material")).toInt(-1);
            if (material >= 0 && material < materials.size()) {
                const QJsonObject pbr = materials.at(material).toObject().value(QStringLiteral("pbrMetallicRoughness")).toObject();
                const QJsonArray base = pbr.value(QStringLiteral("baseColorFactor")).toArray();
                if (base.size() >= 3) {
                    part.colour = {float(base.at(0).toDouble()), float(base.at(1).toDouble()), float(base.at(2).toDouble())};
                }
                part.roughness = float(pbr.value(QStringLiteral("roughnessFactor")).toDouble(1.0));
                part.metallic = float(pbr.value(QStringLiteral("metallicFactor")).toDouble(1.0));
            }
            parts.push_back(std::move(part));
        }
    }
    if (parts.empty()) {
        qCWarning(lcXr).noquote() << "controllers:" << path << "has no triangles Rift Home can draw";
        return std::nullopt;
    }

    // The fit: height from the bounds; the handle's centre line from the shell's (the largest part's) cross-sections.
    float minY = 1e30f, maxY = -1e30f;
    const Part *shell = &parts.front();
    for (const Part &part : parts) {
        for (size_t i = 1; i < part.positions.size(); i += 3) {
            minY = std::min(minY, part.positions[i]);
            maxY = std::max(maxY, part.positions[i]);
        }
        if (part.indices.size() > shell->indices.size()) {
            shell = &part;
        }
    }
    const float height = maxY - minY;
    const std::optional<V3> grip = crossSection(*shell, maxY - kGripBelowTop * height);
    const std::optional<V3> low = crossSection(*shell, minY + kAxisLow * height);
    if (height <= 0.0f || !grip || !low) {
        qCWarning(lcXr).noquote() << "controllers:" << path << "has no handle to fit (its +Y is not up the handle?)";
        return std::nullopt;
    }
    const V3 up = normalized(sub(*grip, *low));
    const V3 minusX{-1.0f, 0.0f, 0.0f};
    const V3 front = normalized(sub(minusX, {up[0] * dot(minusX, up), up[1] * dot(minusX, up), up[2] * dot(minusX, up)}));
    const V3 right = cross(front, up);
    const float scale = kHeight / height;
    // Grip space: +X right, +Y behind (-Y ahead), +Z down the handle; then turned about +X so the front meets the aim.
    const float ca = std::cos(kFrontToAim * float(M_PI) / 180.0f);
    const float sa = std::sin(kFrontToAim * float(M_PI) / 180.0f);
    const auto toGrip = [&](V3 v) {
        const V3 g{dot(right, v), -dot(front, v), -dot(up, v)};
        return V3{g[0], g[1] * ca - g[2] * sa, g[1] * sa + g[2] * ca};
    };

    ControllerMesh mesh;
    for (const Part &part : parts) {
        const uint32_t first = uint32_t(mesh.vertices.size());
        for (size_t i = 0; i + 2 < part.positions.size(); i += 3) {
            const V3 p = toGrip(sub({part.positions[i], part.positions[i + 1], part.positions[i + 2]}, *grip));
            const V3 n = normalized(toGrip({part.normals[i], part.normals[i + 1], part.normals[i + 2]}));
            mesh.vertices.push_back({{p[0] * scale, p[1] * scale, p[2] * scale},
                                     {n[0], n[1], n[2]},
                                     {part.colour[0], part.colour[1], part.colour[2]},
                                     part.roughness,
                                     part.metallic});
        }
        for (uint32_t index : part.indices) {
            if (index < part.positions.size() / 3) {
                mesh.indices.push_back(first + index);
            }
        }
    }
    const float tilt = std::acos(std::clamp(up[1], -1.0f, 1.0f)) * 180.0f / float(M_PI);
    mesh.summary = QStringLiteral("%1 parts, %2 triangles, %3 units tall at %4 mm a unit, handle tilted %5 degrees")
                       .arg(parts.size())
                       .arg(mesh.indices.size() / 3)
                       .arg(height, 0, 'f', 2)
                       .arg(scale * 1000.0f, 0, 'f', 1)
                       .arg(tilt, 0, 'f', 1);
    return mesh;
}

} // namespace RiftHome::Xr
