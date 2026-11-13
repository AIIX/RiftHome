#version 450
// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
// The controllers in the room: Rift Home's own models (ControllerModel), in view space for their lighting.

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColour;
layout(location = 3) in float inRoughness;
layout(location = 4) in float inMetallic;

layout(push_constant) uniform Push {
    mat4 clip; // projection * view * model
    mat4 modelView; // view * model: positions and normals in view space
} pc;

layout(location = 0) out vec3 viewNormal;
layout(location = 1) out vec3 viewPosition;
layout(location = 2) out vec3 colour;
layout(location = 3) out vec2 material; // roughness, metalness

void main()
{
    viewPosition = (pc.modelView * vec4(inPosition, 1.0)).xyz;
    viewNormal = mat3(pc.modelView) * inNormal;
    colour = inColour;
    material = vec2(inRoughness, inMetallic);
    gl_Position = pc.clip * vec4(inPosition, 1.0);
}
