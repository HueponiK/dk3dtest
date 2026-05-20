#version 460

layout (location = 0) in vec3 inPos;
layout (location = 1) in vec3 inColor;

layout (binding = 0, std140) uniform Transform {
    mat4 mvp;
} u;

layout (location = 0) out vec3 vColor;

void main()
{
    gl_Position = u.mvp * vec4(inPos, 1.0);
    vColor = inColor;
}
