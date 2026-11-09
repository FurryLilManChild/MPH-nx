#version 460
// MPH-nx native16 (Top only + HUD): blend a piece of the bottom screen over
// the composed 1280x720 frame. Bilinear from the DS bottom screen
// (0xAARRGGBB words, 256 per row) into the pitch-linear RGBA8 frame copy.
// Alpha follows source brightness: dark panel backgrounds let the game show
// through, bright radar lines and text stay solid.
layout (local_size_x = 8, local_size_y = 8) in;

layout (std140, binding = 0) uniform Params
{
    uint DstX, DstY, DstW, DstH;   // frame pixels
    uint SrcX, SrcY, SrcW, SrcH;   // DS bottom-screen pixels
    uint AlphaDark, AlphaBright;   // 0..255
    uint Pitch;                    // frame row length in pixels
};

layout (std430, binding = 0) readonly buffer SrcBuf { uint Src[]; };
layout (std430, binding = 1) buffer FrameBuf { uint Frame[]; };

vec3 texel(int x, int y)
{
    x = clamp(x, int(SrcX), int(SrcX + SrcW) - 1);
    y = clamp(y, int(SrcY), int(SrcY + SrcH) - 1);
    uint c = Src[uint(y) * 256u + uint(x)];
    return vec3(float((c >> 16) & 255u), float((c >> 8) & 255u), float(c & 255u));
}

void main()
{
    uint x = gl_GlobalInvocationID.x, y = gl_GlobalInvocationID.y;
    if (x >= DstW || y >= DstH) return;
    float fx = (float(x) + 0.5) * float(SrcW) / float(DstW) - 0.5 + float(SrcX);
    float fy = (float(y) + 0.5) * float(SrcH) / float(DstH) - 0.5 + float(SrcY);
    int ix = int(floor(fx)), iy = int(floor(fy));
    float tx = fx - float(ix), ty = fy - float(iy);
    vec3 c = mix(mix(texel(ix, iy), texel(ix + 1, iy), tx),
                 mix(texel(ix, iy + 1), texel(ix + 1, iy + 1), tx), ty);
    float l = max(c.r, max(c.g, c.b));
    float a = mix(float(AlphaDark), float(AlphaBright), smoothstep(80.0, 160.0, l)) / 255.0;
    uint o = (DstY + y) * Pitch + DstX + x;
    uint d = Frame[o];
    vec3 dc = vec3(float(d & 255u), float((d >> 8) & 255u), float((d >> 16) & 255u));
    vec3 r = clamp(c * a + dc * (1.0 - a), 0.0, 255.0);
    Frame[o] = uint(r.r + 0.5) | (uint(r.g + 0.5) << 8) | (uint(r.b + 0.5) << 16) | 0xFF000000u;
}
