#version 460
// MPH-nx resolution scaler: compose the top screen at render size.
// The 2D engine gives, per native pixel, its top two layers without 3D
// (NdsGpu2dHdFrame: RG32 pairs); the 3D layer comes from the render-size
// 3D image and is inserted by (priority, order). Blending and master
// brightness follow ndsrecomp's presenter (melonds_compute/PresentShaders.h),
// which mirrors compose() in gpu2d.cpp. The 2D layers stay at native
// density; only the 3D layer gains resolution.
layout (local_size_x = 8, local_size_y = 8) in;

layout (std140, binding = 0) uniform Params
{
    uint OutW, OutH;          // render size
    uint Bldcnt, Eva, Evb, Evy;
    uint MasterBright, Priority3d, Order3d;
    uint RenderXPos, OutputWidth;
    uint StepX, StepY;        // ceil(native / render * 65536): no integer divide on the GPU
};

layout (std430, binding = 0) readonly buffer HdTopBuf { uvec2 HdTop[]; };
layout (std430, binding = 1) readonly buffer HdBelowBuf { uvec2 HdBelow[]; };
layout (std430, binding = 2) readonly buffer Hi3dBuf { uint Hi3d[]; };
layout (std430, binding = 3) writeonly buffer OutBuf { uint Out[]; };

uvec3 blend16(uvec3 a, uvec3 b, uint ca, uint cb)
{
    return min((a * ca + b * cb + uvec3(8u)) >> 4, uvec3(63u));
}

uvec3 blend5(uvec3 a, uvec3 b, uint alpha)
{
    uint ca = alpha + 1u;
    if (ca == 32u) return a;
    return min((a * ca + b * (32u - ca) + uvec3(16u)) >> 5, uvec3(63u));
}

uvec3 brighten(uvec3 c, uint factor, uint bias)
{
    return min(c + (((uvec3(63u) - c) * factor + uvec3(bias)) >> 4), uvec3(63u));
}

uvec3 darken(uvec3 c, uint factor, uint bias)
{
    return c - ((c * factor + uvec3(bias)) >> 4);
}

struct Layer
{
    uvec3 color;
    uint target;
    bool effects;
    uint alpha;
    uint alpha5;
    uint priority;
    uint order;
};

Layer unpackLayer(uvec2 t)
{
    Layer l;
    l.color = uvec3(t.x & 63u, (t.x >> 8) & 63u, (t.x >> 16) & 63u);
    l.target = t.y & 63u;
    l.effects = (t.y & 128u) != 0u;
    l.alpha = (t.y >> 8) & 255u;
    l.alpha5 = 0u;
    l.priority = (t.y >> 16) & 255u;
    l.order = (t.y >> 24) & 255u;
    return l;
}

bool ahead(Layer a, Layer b)
{
    return a.priority < b.priority || (a.priority == b.priority && a.order < b.order);
}

uvec3 composeLayers(Layer top, Layer below)
{
    uint target2 = below.target << 8;
    if (top.alpha5 != 0u) {
        if ((Bldcnt & target2) != 0u)
            return blend5(top.color, below.color, top.alpha5);
        if (top.effects && (Bldcnt & top.target) != 0u) {
            uint mode = (Bldcnt >> 6) & 3u;
            if (mode == 2u) return brighten(top.color, Evy, 8u);
            if (mode == 3u) return darken(top.color, Evy, 7u);
        }
        return top.color;
    }
    if (top.alpha != 0u && (Bldcnt & target2) != 0u) {
        uint eva1 = top.alpha == 255u ? Eva : top.alpha;
        uint evb1 = top.alpha == 255u ? Evb : 16u - eva1;
        return blend16(top.color, below.color, eva1, evb1);
    }
    if (top.effects && (Bldcnt & top.target) != 0u) {
        uint mode = (Bldcnt >> 6) & 3u;
        if (mode == 1u && (Bldcnt & target2) != 0u)
            return blend16(top.color, below.color, Eva, Evb);
        if (mode == 2u) return brighten(top.color, Evy, 8u);
        if (mode == 3u) return darken(top.color, Evy, 7u);
    }
    return top.color;
}

void main()
{
    uint x = gl_GlobalInvocationID.x, y = gl_GlobalInvocationID.y;
    if (x >= OutW || y >= OutH) return;
    // the native pixel this render-size pixel lies in
    uint nx = min((x * StepX) >> 16, 255u);
    uint ny = min((y * StepY) >> 16, 191u);
    uint n = ny * 256u + nx;

    // RenderXPos is a native-pixel scroll; the 3D image is unscrolled.
    int sx = int(x);
    bool off = false;
    if (RenderXPos != 0u) {
        int sc = int(OutW);
        if ((RenderXPos & 0x100u) != 0u) {
            int shift = (min(int(OutputWidth), 512 - int(RenderXPos)) * sc) >> 8;
            off = int(x) < shift;
            sx = int(x) - shift;
        } else {
            sx = int(x) + ((int(RenderXPos) * sc) >> 8);
            off = sx >= sc;
        }
    }

    uvec2 topTexel = HdTop[n];
    Layer top = unpackLayer(topTexel);
    Layer below = unpackLayer(HdBelow[n]);
    if (!off && (topTexel.y & 64u) == 0u) {
        uint c = Hi3d[y * OutW + uint(sx)];
        uint a3 = (c >> 24) & 31u;
        if (a3 != 0u) {
            Layer l3;
            l3.color = uvec3(c & 63u, (c >> 8) & 63u, (c >> 16) & 63u);
            l3.target = 1u;
            l3.effects = top.effects;
            l3.alpha = 0u;
            l3.alpha5 = a3;
            l3.priority = Priority3d;
            l3.order = Order3d;
            if (ahead(l3, top)) { below = top; top = l3; }
            else if (ahead(l3, below)) { below = l3; }
        }
    }
    uvec3 col = composeLayers(top, below);
    uint bm = MasterBright >> 14;
    uint bf = min(MasterBright & 31u, 16u);
    if (bm == 1u) col = brighten(col, bf, 0u);
    else if (bm == 2u) col = darken(col, bf, 15u);
    uvec3 c8 = (col << 2) | (col >> 4);
    // BGRA8 in memory: 0xAARRGGBB
    Out[y * OutW + x] = 0xFF000000u | (c8.r << 16) | (c8.g << 8) | c8.b;
}
