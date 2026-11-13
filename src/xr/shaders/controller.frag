#version 450
// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
// The controllers in the room: Rift Home's own models with their own materials, opaque, in linear light (an HDR layer:
// 1.0 = the room's white, highlights go past it). Lit as a product shot that follows the head: a key light from above
// in front, a cool fill from the left, the room's warm light all round, and a rim from behind so dark graphite stands
// out from a dark room. GGX speculars (roughness and metalness from the materials); the key's specular is a small bright
// source, which is where the highlights past white come from.

layout(location = 0) in vec3 viewNormal;
layout(location = 1) in vec3 viewPosition;
layout(location = 2) in vec3 colour;
layout(location = 3) in vec2 material;

layout(location = 0) out vec4 outColour;

const float kPi = 3.14159265;

struct Light {
    vec3 direction; // towards the light, view space (x right, y up, z towards the eye)
    vec3 colour; // illuminance: a white matte surface facing it shows this
    float specular; // how much brighter its highlight is than its diffuse light (a small source)
};

vec3 shade(Light light, vec3 n, vec3 v, vec3 diffuseColour, vec3 f0, float alpha)
{
    const vec3 l = normalize(light.direction);
    const float nl = max(dot(n, l), 0.0);
    if (nl <= 0.0) {
        return vec3(0.0);
    }
    const vec3 h = normalize(l + v);
    const float nh = max(dot(n, h), 0.0);
    const float nv = max(dot(n, v), 1e-3);
    const float vh = max(dot(v, h), 0.0);
    const float a2 = alpha * alpha;
    const float d = a2 / (kPi * pow(nh * nh * (a2 - 1.0) + 1.0, 2.0));
    const vec3 f = f0 + (1.0 - f0) * pow(1.0 - vh, 5.0);
    const float k = alpha / 2.0;
    const float g = (nl / (nl * (1.0 - k) + k)) * (nv / (nv * (1.0 - k) + k));
    const vec3 specular = d * f * g / (4.0 * nl * nv + 1e-4);
    return (diffuseColour * (1.0 - f) + specular * kPi * light.specular) * nl * light.colour;
}

void main()
{
    vec3 n = normalize(viewNormal);
    if (!gl_FrontFacing) {
        n = -n; // double-sided materials
    }
    const vec3 v = normalize(-viewPosition);
    const float roughness = clamp(material.x, 0.08, 1.0);
    const float metallic = clamp(material.y, 0.0, 1.0);
    const vec3 diffuseColour = colour * (1.0 - metallic);
    const vec3 f0 = mix(vec3(0.04), colour, metallic);
    const float alpha = roughness * roughness;

    // Matte soft-touch plastic should read as dark grey, not glossy black: modest speculars, a fuller fill.
    const Light key = Light(vec3(0.35, 0.85, 0.55), vec3(1.7, 1.66, 1.6), 1.3);
    const Light fill = Light(vec3(-0.75, 0.15, 0.55), vec3(0.45, 0.50, 0.58), 0.5);
    const Light rim = Light(vec3(0.15, 0.45, -1.0), vec3(0.55, 0.60, 0.70), 0.7);
    vec3 lit = shade(key, n, v, diffuseColour, f0, alpha) + shade(fill, n, v, diffuseColour, f0, alpha)
        + shade(rim, n, v, diffuseColour, f0, alpha);
    // The room's light all round: warmer from below (the floor), cooler from above.
    const vec3 ambient = mix(vec3(0.22, 0.18, 0.14), vec3(0.22, 0.23, 0.27), n.y * 0.5 + 0.5);
    lit += ambient * diffuseColour + ambient * f0 * 0.5;
    outColour = vec4(lit, 1.0);
}
