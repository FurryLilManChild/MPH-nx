// MPH-nx: deko3d GPU 3D renderer for the Switch, ported from RSDuck's
// melonDS Switch build (GPU3D_Deko.cpp, GPL-3.0) to the object-based melonDS
// API that ndsrecomp vendors. The final image is read back for the software
// 2D compositor instead of feeding a deko3d 2D renderer.
#include "GPU3D_Deko.h"
#include "GPU.h"
#include "nx_deko.h"

#include <assert.h>
#include <string>
#include <switch.h>

#define XXH_STATIC_LINKING_ONLY
#define XXH_INLINE_ALL
#include "xxhash/xxhash.h"

#include <arm_neon.h>

using NxDeko::EmuCmdBuf;
using NxDeko::EmuQueue;

int visiblePolygon;

u32 stupidTextureNum = 0;

uint16_t nds_powercontrol9();  // runner io.cpp: ARM9 POWCNT1
namespace melonDS
{

DekoRenderer::DekoRenderer()
    : Renderer3D(false),
    CmdMem(*NxDeko::DataHeap, 1024*128)
{}

DekoRenderer::~DekoRenderer()
{}

bool DekoRenderer::Init()
{
    for (int i = 0; i < 2; i++)
    {
        YSpanSetupMemory[i] = NxDeko::DataHeap->Alloc(sizeof(SpanSetupY)*MaxYSpanSetups, 4);

        RenderPolygonMemory[i] = NxDeko::DataHeap->Alloc(sizeof(RenderPolygon)*2048, 4);
    }

    TileMemory = NxDeko::DataHeap->Alloc(sizeof(Tiles), alignof(Tiles));

    XSpanSetupMemory = NxDeko::DataHeap->Alloc(sizeof(SpanSetupX)*MaxYSpanIndices, alignof(SpanSetupX));

    BinResultMemory = NxDeko::DataHeap->Alloc(sizeof(BinResult), alignof(BinResult));
    memset(NxDeko::DataHeap->CpuAddr<void>(BinResultMemory), 0, sizeof(BinResult));

    FinalTileMemory = NxDeko::DataHeap->Alloc(sizeof(FinalTiles), alignof(FinalTiles));

    dk::ImageLayout yspanIndicesLayout;
    dk::ImageLayoutMaker{NxDeko::Device}
        .setType(DkImageType_Buffer)
        .setDimensions(MaxYSpanIndices)
        .setFormat(DkImageFormat_RGBA16_Uint)
        .initialize(yspanIndicesLayout);
    YSpanIndicesTextureMemory = NxDeko::TextureHeap->Alloc(yspanIndicesLayout.getSize(), yspanIndicesLayout.getAlignment());
    YSpanIndicesTexture.initialize(yspanIndicesLayout, NxDeko::TextureHeap->MemBlock, YSpanIndicesTextureMemory.Offset);

    static const char* const kSuffix[5] = {"", "_S15", "_S2", "_S3", "_S4"};
    for (int si = 0; si < 3; si++)
    {
        ShaderSet& set = Sets[si];
        const char* sfx = kSuffix[si];
        if (!NxDeko::LoadShader((std::string("InterpXSpansZBuffer") + sfx).c_str(), set.ShaderInterpXSpans[0])) return false;
        if (!NxDeko::LoadShader((std::string("InterpXSpansWBuffer") + sfx).c_str(), set.ShaderInterpXSpans[1])) return false;
        if (!NxDeko::LoadShader((std::string("BinCombined") + sfx).c_str(), set.ShaderBinCombined)) return false;
        if (!NxDeko::LoadShader((std::string("DepthBlendZBuffer") + sfx).c_str(), set.ShaderDepthBlend[0])) return false;
        if (!NxDeko::LoadShader((std::string("DepthBlendWBuffer") + sfx).c_str(), set.ShaderDepthBlend[1])) return false;
        if (!NxDeko::LoadShader((std::string("RasteriseNoTextureZBuffer") + sfx).c_str(), set.ShaderRasteriseNoTexture[0])) return false;
        if (!NxDeko::LoadShader((std::string("RasteriseNoTextureZBufferToon") + sfx).c_str(), set.ShaderRasteriseNoTextureToon[0])) return false;
        if (!NxDeko::LoadShader((std::string("RasteriseNoTextureZBufferHighlight") + sfx).c_str(), set.ShaderRasteriseNoTextureHighlight[0])) return false;
        if (!NxDeko::LoadShader((std::string("RasteriseUseTextureDecalZBuffer") + sfx).c_str(), set.ShaderRasteriseUseTextureDecal[0])) return false;
        if (!NxDeko::LoadShader((std::string("RasteriseUseTextureModulateZBuffer") + sfx).c_str(), set.ShaderRasteriseUseTextureModulate[0])) return false;
        if (!NxDeko::LoadShader((std::string("RasteriseUseTextureToonZBuffer") + sfx).c_str(), set.ShaderRasteriseUseTextureToon[0])) return false;
        if (!NxDeko::LoadShader((std::string("RasteriseUseTextureHighlightZBuffer") + sfx).c_str(), set.ShaderRasteriseUseTextureHighlight[0])) return false;
        if (!NxDeko::LoadShader((std::string("RasteriseShadowMaskZBuffer") + sfx).c_str(), set.ShaderRasteriseShadowMask[0])) return false;
        if (!NxDeko::LoadShader((std::string("RasteriseNoTextureWBuffer") + sfx).c_str(), set.ShaderRasteriseNoTexture[1])) return false;
        if (!NxDeko::LoadShader((std::string("RasteriseNoTextureWBufferToon") + sfx).c_str(), set.ShaderRasteriseNoTextureToon[1])) return false;
        if (!NxDeko::LoadShader((std::string("RasteriseNoTextureWBufferHighlight") + sfx).c_str(), set.ShaderRasteriseNoTextureHighlight[1])) return false;
        if (!NxDeko::LoadShader((std::string("RasteriseUseTextureDecalWBuffer") + sfx).c_str(), set.ShaderRasteriseUseTextureDecal[1])) return false;
        if (!NxDeko::LoadShader((std::string("RasteriseUseTextureModulateWBuffer") + sfx).c_str(), set.ShaderRasteriseUseTextureModulate[1])) return false;
        if (!NxDeko::LoadShader((std::string("RasteriseUseTextureToonWBuffer") + sfx).c_str(), set.ShaderRasteriseUseTextureToon[1])) return false;
        if (!NxDeko::LoadShader((std::string("RasteriseUseTextureHighlightWBuffer") + sfx).c_str(), set.ShaderRasteriseUseTextureHighlight[1])) return false;
        if (!NxDeko::LoadShader((std::string("RasteriseShadowMaskWBuffer") + sfx).c_str(), set.ShaderRasteriseShadowMask[1])) return false;
        if (!NxDeko::LoadShader((std::string("ClearCoarseBinMask") + sfx).c_str(), set.ShaderClearCoarseBinMask)) return false;
        if (!NxDeko::LoadShader((std::string("ClearIndirectWorkCount") + sfx).c_str(), set.ShaderClearIndirectWorkCount)) return false;
        if (!NxDeko::LoadShader((std::string("CalculateWorkOffsets") + sfx).c_str(), set.ShaderCalculateWorkListOffset)) return false;
        if (!NxDeko::LoadShader((std::string("SortWork") + sfx).c_str(), set.ShaderSortWork)) return false;
        if (!NxDeko::LoadShader((std::string("FinalPass") + sfx).c_str(), set.ShaderFinalPass[0])) return false;
        if (!NxDeko::LoadShader((std::string("FinalPassEdge") + sfx).c_str(), set.ShaderFinalPass[1])) return false;
        if (!NxDeko::LoadShader((std::string("FinalPassFog") + sfx).c_str(), set.ShaderFinalPass[2])) return false;
        if (!NxDeko::LoadShader((std::string("FinalPassEdgeFog") + sfx).c_str(), set.ShaderFinalPass[3])) return false;
        if (!NxDeko::LoadShader((std::string("FinalPassAA") + sfx).c_str(), set.ShaderFinalPass[4])) return false;
        if (!NxDeko::LoadShader((std::string("FinalPassEdgeAA") + sfx).c_str(), set.ShaderFinalPass[5])) return false;
        if (!NxDeko::LoadShader((std::string("FinalPassFogAA") + sfx).c_str(), set.ShaderFinalPass[6])) return false;
        if (!NxDeko::LoadShader((std::string("FinalPassEdgeFogAA") + sfx).c_str(), set.ShaderFinalPass[7])) return false;
        if (si > 0 && !NxDeko::LoadShader((std::string("Downsample") + sfx).c_str(), set.ShaderDownsample)) return false;
    }
    S = &Sets[0];

    {
        ImageDescriptors = NxDeko::DataHeap->Alloc(sizeof(dk::ImageDescriptor)*descriptorOffset_Count, DK_IMAGE_DESCRIPTOR_ALIGNMENT);
        dk::ImageDescriptor* descriptors = NxDeko::DataHeap->CpuAddr<dk::ImageDescriptor>(ImageDescriptors);
        descriptors[descriptorOffset_YSpanIndices].initialize(YSpanIndicesTexture, true);
    }

    {
        SamplerDescriptors = NxDeko::DataHeap->Alloc(sizeof(dk::SamplerDescriptor)*9, DK_SAMPLER_DESCRIPTOR_ALIGNMENT);
        dk::SamplerDescriptor* descriptors = NxDeko::DataHeap->CpuAddr<dk::SamplerDescriptor>(SamplerDescriptors);
        for (u32 j = 0; j < 3; j++)
        {
            for (u32 i = 0; i < 3; i++)
            {
                const DkWrapMode translateWrapMode[3] = {DkWrapMode_ClampToEdge, DkWrapMode_Repeat, DkWrapMode_MirroredRepeat};
                descriptors[i+j*3].initialize(dk::Sampler{}.setWrapMode(translateWrapMode[i], translateWrapMode[j]));
            }
        }
    }

    MetaUniformMemory = NxDeko::DataHeap->Alloc(MetaUniformSize, DK_UNIFORM_BUF_ALIGNMENT);

    for (int i = 0; i < 2; i++)
    {
        FinalOutputMemory[i] = NxDeko::ReadbackHeap->Alloc(256*192*4, 256);
        memset(NxDeko::ReadbackHeap->CpuAddr<void>(FinalOutputMemory[i]), 0, 256*192*4);
        armDCacheFlush(NxDeko::ReadbackHeap->CpuAddr<void>(FinalOutputMemory[i]), 256*192*4);
        HiresOutputMemory[i] = NxDeko::DataHeap->Alloc(1024*768*4, 256);
    }

    return true;
}

void DekoRenderer::Reset(GPU& gpu)
{
    for (u32 i = 0; i < 8; i++)
    {
        for (u32 j = 0; j < 8; j++)
        {
            for (u32 k = 0; k < TexArrays[i][j].size(); k++)
                NxDeko::TextureHeap->Free(TexArrays[i][j][k].Memory);
            TexArrays[i][j].clear();
            FreeTextures[i][j].clear();
        }
    }
    TexCache.clear();

    FreeImageDescriptorsCount = TexCacheMaxImages;
    for (int i = 0; i < TexCacheMaxImages; i++)
    {
        FreeImageDescriptors[i] = i;
    }
}

void DekoRenderer::VCount144(GPU& gpu)
{

}

void DekoRenderer::SetupAttrs(SpanSetupY* span, Polygon* poly, int from, int to)
{
    span->Z0 = poly->FinalZ[from];
    span->W0 = poly->FinalW[from];
    span->Z1 = poly->FinalZ[to];
    span->W1 = poly->FinalW[to];
    span->ColorR0 = poly->Vertices[from]->FinalColor[0];
    span->ColorG0 = poly->Vertices[from]->FinalColor[1];
    span->ColorB0 = poly->Vertices[from]->FinalColor[2];
    span->ColorR1 = poly->Vertices[to]->FinalColor[0];
    span->ColorG1 = poly->Vertices[to]->FinalColor[1];
    span->ColorB1 = poly->Vertices[to]->FinalColor[2];
    span->TexcoordU0 = poly->Vertices[from]->TexCoords[0];
    span->TexcoordV0 = poly->Vertices[from]->TexCoords[1];
    span->TexcoordU1 = poly->Vertices[to]->TexCoords[0];
    span->TexcoordV1 = poly->Vertices[to]->TexCoords[1];
}

void DekoRenderer::SetupYSpanDummy(SpanSetupY* span, Polygon* poly, int vertex, int side)
{
    s32 x0 = Pos[vertex][0];
    if (side)
    {
        span->DxInitial = -0x40000;
        x0--;
    }
    else
    {
        span->DxInitial = 0;
    }

    span->X0 = span->X1 = x0;
    span->XMin = x0;
    span->XMax = x0;
    span->Y0 = span->Y1 = Pos[vertex][1];

    span->Increment = 0;

    span->I0 = span->I1 = span->IRecip = 0;
    span->Linear = true;

    span->XCovIncr = 0;

    span->IsDummy = true;

    SetupAttrs(span, poly, vertex, vertex);
}

void DekoRenderer::SetupYSpan(int polynum, SpanSetupY* span, Polygon* poly, int from, int to, u32 y, int side)
{
    span->X0 = Pos[from][0];
    span->X1 = Pos[to][0];
    span->Y0 = Pos[from][1];
    span->Y1 = Pos[to][1];

    SetupAttrs(span, poly, from, to);

    bool negative = false;
    if (span->X1 > span->X0)
    {
        span->XMin = span->X0;
        span->XMax = span->X1-1;
    }
    else if (span->X1 < span->X0)
    {
        span->XMin = span->X1;
        span->XMax = span->X0-1;
        negative = true;
    }
    else
    {
        span->XMin = span->X0;
        if (side) span->XMin--;
        span->XMax = span->XMin;
    }

    span->IsDummy = false;

    s32 xlen = span->XMax+1 - span->XMin;
    s32 ylen = span->Y1 - span->Y0;

    // slope increment has a 18-bit fractional part
    // note: for some reason, x/y isn't calculated directly,
    // instead, 1/y is calculated and then multiplied by x
    // TODO: this is still not perfect (see for example x=169 y=33)
    if (ylen == 0)
    {
        span->Increment = 0;
    }
    else if (ylen == xlen)
    {
        span->Increment = 0x40000;
    }
    else
    {
        s32 yrecip = (1<<18) / ylen;
        span->Increment = (span->X1-span->X0) * yrecip;
        if (span->Increment < 0) span->Increment = -span->Increment;
    }

    bool xMajor = (span->Increment > 0x40000);

    if (side)
    {
        // right

        if (xMajor)
            span->DxInitial = negative ? (0x20000 + 0x40000) : (span->Increment - 0x20000);
        else if (span->Increment != 0)
            span->DxInitial = negative ? 0x40000 : 0;
        else
            span->DxInitial = -0x40000;
    }
    else
    {
        // left

        if (xMajor)
            span->DxInitial = negative ? ((span->Increment - 0x20000) + 0x40000) : 0x20000;
        else if (span->Increment != 0)
            span->DxInitial = negative ? 0x40000 : 0;
        else
            span->DxInitial = 0;
    }

    if (xMajor)
    {
        if (side)
        {
            span->I0 = span->X0 - 1;
            span->I1 = span->X1 - 1;
        }
        else
        {
            span->I0 = span->X0;
            span->I1 = span->X1;
        }

        // used for calculating AA coverage
        span->XCovIncr = (ylen << 10) / xlen;
    }
    else
    {
        span->I0 = span->Y0;
        span->I1 = span->Y1;
    }

    //if (span->I1 < span->I0)
    //    std::swap(span->I0, span->I1);

    if (span->I0 != span->I1)
        span->IRecip = (1<<30) / (span->I1 - span->I0);
    else
        span->IRecip = 0;

    span->Linear = (span->W0 == span->W1) && !(span->W0 & 0x7E) && !(span->W1 & 0x7E);

    if ((span->W0 & 0x1) && !(span->W1 & 0x1))
    {
        span->W0n = (span->W0 - 1) >> 1;
        span->W0d = (span->W0 + 1) >> 1;
        span->W1d = span->W1 >> 1;
    }
    else
    {
        span->W0n = span->W0 >> 1;
        span->W0d = span->W0 >> 1;
        span->W1d = span->W1 >> 1;
    }
}

inline u32 TextureWidth(u32 texparam)
{
    return 8 << ((texparam >> 20) & 0x7);
}

inline u32 TextureHeight(u32 texparam)
{
    return 8 << ((texparam >> 23) & 0x7);
}

inline u16 ColorAvg(u16 color0, u16 color1)
{
    u32 r0 = color0 & 0x001F;
    u32 g0 = color0 & 0x03E0;
    u32 b0 = color0 & 0x7C00;
    u32 r1 = color1 & 0x001F;
    u32 g1 = color1 & 0x03E0;
    u32 b1 = color1 & 0x7C00;

    u32 r = (r0 + r1) >> 1;
    u32 g = ((g0 + g1) >> 1) & 0x03E0;
    u32 b = ((b0 + b1) >> 1) & 0x7C00;

    return r | g | b;
}

inline u16 Color5of3(u16 color0, u16 color1)
{
    u32 r0 = color0 & 0x001F;
    u32 g0 = color0 & 0x03E0;
    u32 b0 = color0 & 0x7C00;
    u32 r1 = color1 & 0x001F;
    u32 g1 = color1 & 0x03E0;
    u32 b1 = color1 & 0x7C00;

    u32 r = (r0*5 + r1*3) >> 3;
    u32 g = ((g0*5 + g1*3) >> 3) & 0x03E0;
    u32 b = ((b0*5 + b1*3) >> 3) & 0x7C00;

    return r | g | b;
}

inline u16 Color3of5(u16 color0, u16 color1)
{
    u32 r0 = color0 & 0x001F;
    u32 g0 = color0 & 0x03E0;
    u32 b0 = color0 & 0x7C00;
    u32 r1 = color1 & 0x001F;
    u32 g1 = color1 & 0x03E0;
    u32 b1 = color1 & 0x7C00;

    u32 r = (r0*3 + r1*5) >> 3;
    u32 g = ((g0*3 + g1*5) >> 3) & 0x03E0;
    u32 b = ((b0*3 + b1*5) >> 3) & 0x7C00;

    return r | g | b;
}

inline void RGB5ToRGB6(uint8x16_t lo, uint8x16_t hi, uint8x16_t& red, uint8x16_t& green, uint8x16_t& blue)
{
    red = vandq_u8(vshlq_n_u8(lo, 1), vdupq_n_u8(0x3E));
    green = vbslq_u8(vdupq_n_u8(0xCE), vshrq_n_u8(lo, 4), vshlq_n_u8(hi, 4));
    blue = vandq_u8(vshrq_n_u8(hi, 1), vdupq_n_u8(0x3E));
    red = vandq_u8(vtstq_u8(red, red), vaddq_u8(red, vdupq_n_u8(1)));
    green = vandq_u8(vtstq_u8(green, green), vaddq_u8(green, vdupq_n_u8(1)));
    blue =  vandq_u8(vtstq_u8(blue, blue), vaddq_u8(blue, vdupq_n_u8(1)));
}

inline void RGB5ToRGB6(uint8x8_t lo, uint8x8_t hi, uint8x8_t& red, uint8x8_t& green, uint8x8_t& blue)
{
    red   = vand_u8(vshl_n_u8(lo, 1), vdup_n_u8(0x3E));
    green = vbsl_u8(vdup_n_u8(0xCE), vshr_n_u8(lo, 4), vshl_n_u8(hi, 4));
    blue  = vand_u8(vshr_n_u8(hi, 1), vdup_n_u8(0x3E));

    red   = vand_u8(vtst_u8(red, red), vadd_u8(red, vdup_n_u8(1)));
    green = vand_u8(vtst_u8(green, green), vadd_u8(green, vdup_n_u8(1)));
    blue  = vand_u8(vtst_u8(blue, blue), vadd_u8(blue, vdup_n_u8(1)));
}

inline u32 ConvertRGB5ToRGB8(u16 val)
{
    return (((u32)val & 0x1F) << 3)
        | (((u32)val & 0x3E0) << 6)
        | (((u32)val & 0x7C00) << 9);
}
inline u32 ConvertRGB5ToBGR8(u16 val)
{
    return (((u32)val & 0x1F) << 9)
        | (((u32)val & 0x3E0) << 6)
        | (((u32)val & 0x7C00) << 3);
}
inline u32 ConvertRGB5ToRGB6(u16 val)
{
    u8 r = (val & 0x1F) << 1;
    u8 g = (val & 0x3E0) >> 4;
    u8 b = (val & 0x7C00) >> 9;
    if (r) r++;
    if (g) g++;
    if (b) b++;
    return (u32)r | ((u32)g << 8) | ((u32)b << 16);
}

enum
{
    outputFmt_RGB6A5,
    outputFmt_RGBA8,
    outputFmt_BGRA8
};

template <int outputFmt>
void ConvertCompressedTexture(u32 width, u32 height, u32* output, u8* texData, u8* texAuxData, u16* palData)
{
    // we process a whole block at the time
    for (int y = 0; y < height / 4; y++)
    {
        for (int x = 0; x < width / 4; x++)
        {
            u32 data = ((u32*)texData)[x + y * (width / 4)];
            u16 auxData = ((u16*)texAuxData)[x + y * (width / 4)];

            u32 paletteOffset = auxData & 0x3FFF;
            u16 color0 = palData[paletteOffset*2] | 0x8000;
            u16 color1 = palData[paletteOffset*2+1] | 0x8000;
            u16 color2, color3;

            switch ((auxData >> 14) & 0x3)
            {
            case 0:
                color2 = palData[paletteOffset*2+2] | 0x8000;
                color3 = 0;
                break;
            case 1:
                {
                    u32 r0 = color0 & 0x001F;
                    u32 g0 = color0 & 0x03E0;
                    u32 b0 = color0 & 0x7C00;
                    u32 r1 = color1 & 0x001F;
                    u32 g1 = color1 & 0x03E0;
                    u32 b1 = color1 & 0x7C00;

                    u32 r = (r0 + r1) >> 1;
                    u32 g = ((g0 + g1) >> 1) & 0x03E0;
                    u32 b = ((b0 + b1) >> 1) & 0x7C00;
                    color2 = r | g | b | 0x8000;
                }
                color3 = 0;
                break;
            case 2:
                color2 = palData[paletteOffset*2+2] | 0x8000;
                color3 = palData[paletteOffset*2+3] | 0x8000;
                break;
            case 3:
                {
                    u32 r0 = color0 & 0x001F;
                    u32 g0 = color0 & 0x03E0;
                    u32 b0 = color0 & 0x7C00;
                    u32 r1 = color1 & 0x001F;
                    u32 g1 = color1 & 0x03E0;
                    u32 b1 = color1 & 0x7C00;

                    u32 r = (r0*5 + r1*3) >> 3;
                    u32 g = ((g0*5 + g1*3) >> 3) & 0x03E0;
                    u32 b = ((b0*5 + b1*3) >> 3) & 0x7C00;

                    color2 = r | g | b | 0x8000;
                }
                {
                    u32 r0 = color0 & 0x001F;
                    u32 g0 = color0 & 0x03E0;
                    u32 b0 = color0 & 0x7C00;
                    u32 r1 = color1 & 0x001F;
                    u32 g1 = color1 & 0x03E0;
                    u32 b1 = color1 & 0x7C00;

                    u32 r = (r0*3 + r1*5) >> 3;
                    u32 g = ((g0*3 + g1*5) >> 3) & 0x03E0;
                    u32 b = ((b0*3 + b1*5) >> 3) & 0x7C00;

                    color3 = r | g | b | 0x8000;
                }
                break;
            }

            // in 2020 our default data types are big enough to be used as lookup tables...
            u64 packed = color0 | ((u64)color1 << 16) | ((u64)color2 << 32) | ((u64)color3 << 48);

            for (int j = 0; j < 4; j++)
            {
                for (int i = 0; i < 4; i++)
                {
                    u16 color = (packed >> 16 * (data >> 2 * (i + j * 4))) & 0xFFFF;
                    u32 res;
                    switch (outputFmt)
                    {
                    case outputFmt_RGB6A5: res = ConvertRGB5ToRGB6(color)
                        | ((color & 0x8000) ? 0x1F000000 : 0); break;
                    case outputFmt_RGBA8: res = ConvertRGB5ToRGB8(color)
                        | ((color & 0x8000) ? 0xFF000000 : 0); break;
                    case outputFmt_BGRA8: res = ConvertRGB5ToBGR8(color)
                        | ((color & 0x8000) ? 0xFF000000 : 0); break;
                    }
                    output[x * 4 + i + (y * 4 + j) * width] = res;
                }
            }
        }
    }
}

template <int outputFmt, int X, int Y>
void ConvertAXIYTexture(u32 width, u32 height, u32* output, u8* texData, u16* palData)
{
    for (int y = 0; y < height; y++)
    {
        for (int x = 0; x < width; x++)
        {
            u8 val = texData[x + y * width];

            u32 idx = val & ((1 << Y) - 1);

            u16 color = palData[idx];
            u32 alpha = (val >> Y) & ((1 << X) - 1);
            if (X != 5)
                alpha = alpha * 4 + alpha / 2;

            u32 res;
            switch (outputFmt)
            {
            case outputFmt_RGB6A5: res = ConvertRGB5ToRGB6(color) | alpha << 24; break;
            // make sure full alpha == 255
            case outputFmt_RGBA8: res = ConvertRGB5ToRGB8(color) | (alpha << 27 | (alpha & 0x1C) << 22); break;
            case outputFmt_BGRA8: res = ConvertRGB5ToBGR8(color) | (alpha << 27 | (alpha & 0x1C) << 22); break;
            }
            output[x + y * width] = res;
        }
    }
}

void Convert16ColorsTexture(u32 width, u32 height, u32* output, u8* texData, u16* palData, bool color0Transparent)
{
    uint8x16x2_t palette = vld2q_u8((u8*)palData);

    uint8x16_t paletteR, paletteG, paletteB;
    RGB5ToRGB6(palette.val[0], palette.val[1], paletteR, paletteG, paletteB);

    uint8x16_t firstEntryAlpha = vdupq_n_u8(color0Transparent ? 0 : 0x1F);

    for (int i = 0; i < width*height/2; i += 16)
    {
        uint8x16_t packedIndices = vld1q_u8(&texData[i]);

        // unpack indices
        uint8x16_t oddIndices = vandq_u8(packedIndices, vdupq_n_u8(0xF));
        uint8x16_t evenIndices = vshrq_n_u8(packedIndices, 4);

        uint8x16_t indices0 = vzip1q_u8(oddIndices, evenIndices);
        uint8x16_t indices1 = vzip2q_u8(oddIndices, evenIndices);

        // palettise
        uint8x16x4_t finalPixels0, finalPixels1;
        finalPixels0.val[0] = vqtbl1q_u8(paletteR, indices0);
        finalPixels0.val[1] = vqtbl1q_u8(paletteG, indices0);
        finalPixels0.val[2] = vqtbl1q_u8(paletteB, indices0);
        finalPixels0.val[3] = vbslq_u8(vceqzq_u8(indices0), firstEntryAlpha, vdupq_n_u8(0x1F));
        finalPixels1.val[0] = vqtbl1q_u8(paletteR, indices1);
        finalPixels1.val[1] = vqtbl1q_u8(paletteG, indices1);
        finalPixels1.val[2] = vqtbl1q_u8(paletteB, indices1);
        finalPixels1.val[3] = vbslq_u8(vceqzq_u8(indices1), firstEntryAlpha, vdupq_n_u8(0x1F));

        vst4q_u8((u8*)&output[i*2], finalPixels0);
        vst4q_u8((u8*)&output[i*2+16], finalPixels1);
    }
}

template <int outputFmt, int colorBits>
void ConvertNColorsTexture(u32 width, u32 height, u32* output, u8* texData, u16* palData, bool color0Transparent)
{
    for (int y = 0; y < height; y++)
    {
        for (int x = 0; x < width / (8 / colorBits); x++)
        {
            u8 val = texData[x + y * (width / (8 / colorBits))];

            for (int i = 0; i < 8 / colorBits; i++)
            {
                u32 index = (val >> (i * colorBits)) & ((1 << colorBits) - 1);
                u16 color = palData[index];

                bool transparent = color0Transparent && index == 0;
                u32 res;
                switch (outputFmt)
                {
                case outputFmt_RGB6A5: res = ConvertRGB5ToRGB6(color)
                    | (transparent ? 0 : 0x1F000000); break;
                case outputFmt_RGBA8: res = ConvertRGB5ToRGB8(color)
                    | (transparent ? 0 : 0xFF000000); break;
                case outputFmt_BGRA8: res = ConvertRGB5ToBGR8(color)
                    | (transparent ? 0 : 0xFF000000); break;
                }
                output[x * (8 / colorBits) + y * width + i] = res;
            }
        }
    }
}

DekoRenderer::TexCacheEntry& DekoRenderer::GetTexture(GPU& gpu, u32 texParam, u32 palBase)
{
    // remove sampling and texcoord gen params
    texParam &= ~0xC00F0000;

    u32 fmt = (texParam >> 26) & 0x7;
    u64 key = texParam;
    if (fmt != 7)
    {
        key |= (u64)palBase << 32;
        if (fmt == 5)
            key &= ~((u64)1 << 29);
    }
    //printf("%" PRIx64 " %" PRIx32 " %" PRIx32 "\n", key, texParam, palBase);

    assert(fmt != 0 && "no texture is not a texture format!");

    auto it = TexCache.find(key);

    if (it != TexCache.end())
        return it->second;

    u32 widthLog2 = (texParam >> 20) & 0x7;
    u32 heightLog2 = (texParam >> 23) & 0x7;
    u32 width = 8 << widthLog2;
    u32 height = 8 << heightLog2;

    u32 addr = (texParam & 0xFFFF) * 8;

    TexCacheEntry entry = {0};

    entry.TextureRAMStart[0] = addr;
    entry.WidthLog2 = widthLog2;
    entry.HeightLog2 = heightLog2;

    // apparently a new texture
    if (fmt == 7)
    {
        entry.TextureRAMSize[0] = width*height*2;

        for (u32 i = 0; i < width*height; i += 16)
        {
            uint8x16x2_t pixels = vld2q_u8(&gpu.VRAMFlat_Texture[addr + i * 2]);

            uint8x16_t red, green, blue;
            RGB5ToRGB6(pixels.val[0], pixels.val[1], red, green, blue);
            uint8x16_t alpha = vbslq_u8(vtstq_u8(pixels.val[1], vdupq_n_u8(0x80)), vdupq_n_u8(0x1F), vdupq_n_u8(0));

            vst4q_u8((u8*)&TextureDecodingBuffer[i],
            {
                red,
                green,
                blue,
                alpha
            });
        }

    }
    else if (fmt == 5)
    {
        u8* texData = &gpu.VRAMFlat_Texture[addr];
        u32 slot1addr = 0x20000 + ((addr & 0x1FFFC) >> 1);
        if (addr >= 0x40000)
            slot1addr += 0x10000;
        u8* texAuxData = &gpu.VRAMFlat_Texture[slot1addr];

        u16* palData = (u16*)(gpu.VRAMFlat_TexPal + palBase*16);

        entry.TextureRAMSize[0] = width*height/16*4;
        entry.TextureRAMStart[1] = slot1addr;
        entry.TextureRAMSize[1] = width*height/16*2;
        entry.TexPalStart = palBase*16;
        entry.TexPalSize = 0x10000;

        ConvertCompressedTexture<outputFmt_RGB6A5>(width, height, TextureDecodingBuffer, texData, texAuxData, palData);
    }
    else
    {
        u32 texSize, palAddr = palBase*16, numPalEntries;
        switch (fmt)
        {
        case 1: texSize = width*height; numPalEntries = 32; break;
        case 6: texSize = width*height; numPalEntries = 8; break;
        case 2: texSize = width*height/4; numPalEntries = 4; palAddr >>= 1; break;
        case 3: texSize = width*height/2; numPalEntries = 16; break;
        case 4: texSize = width*height; numPalEntries = 256; break;
        }

        palAddr &= 0x1FFFF;

        /*printf("creating texture | fmt: %d | %dx%d | %08x | %08x\n", fmt, width, height, addr, palAddr);
        svcSleepThread(1000*1000);*/

        entry.TextureRAMSize[0] = texSize;
        entry.TexPalStart = palAddr;
        entry.TexPalSize = numPalEntries*2;

        u8* texData = &gpu.VRAMFlat_Texture[addr];
        u16* palData = (u16*)(gpu.VRAMFlat_TexPal + palAddr);

        //assert(entry.TexPalStart+entry.TexPalSize <= 128*1024*1024);

        bool color0Transparent = texParam & (1 << 29);

        switch (fmt)
        {
        case 1: ConvertAXIYTexture<outputFmt_RGB6A5, 3, 5>(width, height, TextureDecodingBuffer, texData, palData); break;
        case 6: ConvertAXIYTexture<outputFmt_RGB6A5, 5, 3>(width, height, TextureDecodingBuffer, texData, palData); break;
        case 2: ConvertNColorsTexture<outputFmt_RGB6A5, 2>(width, height, TextureDecodingBuffer, texData, palData, color0Transparent); break;
        case 3: Convert16ColorsTexture(width, height, TextureDecodingBuffer, texData, palData, color0Transparent); break;
        case 4: ConvertNColorsTexture<outputFmt_RGB6A5, 8>(width, height, TextureDecodingBuffer, texData, palData, color0Transparent); break;
        }
    }

    for (int i = 0; i < 2; i++)
    {
        if (entry.TextureRAMSize[i])
            entry.TextureHash[i] = XXH3_64bits(&gpu.VRAMFlat_Texture[entry.TextureRAMStart[i]], entry.TextureRAMSize[i]);
    }
    if (entry.TexPalSize)
        entry.TexPalHash = XXH3_64bits(&gpu.VRAMFlat_TexPal[entry.TexPalStart], entry.TexPalSize);

    auto& texArrays = TexArrays[widthLog2][heightLog2];
    auto& freeTextures = FreeTextures[widthLog2][heightLog2];

    if (freeTextures.size() == 0)
    {
        texArrays.resize(texArrays.size()+1);
        TexArray& array = texArrays[texArrays.size()-1];

        u32 layers = std::min<u32>((8*1024*1024) / (width*height*4), 64);

        // allocate new array texture
        dk::ImageLayout imageLayout;
        dk::ImageLayoutMaker{NxDeko::Device}
            .setType(DkImageType_2DArray)
            .setFormat(DkImageFormat_RGBA8_Uint)
            .setDimensions(width, height, layers)
            .initialize(imageLayout);

        assert(FreeImageDescriptorsCount > 0);
        array.ImageDescriptor = FreeImageDescriptors[--FreeImageDescriptorsCount];

        array.Memory = NxDeko::TextureHeap->Alloc(imageLayout.getSize(), imageLayout.getAlignment());
        array.Image.initialize(imageLayout, NxDeko::TextureHeap->MemBlock, array.Memory.Offset);

        dk::ImageDescriptor descriptor;
        descriptor.initialize(array.Image);
        DkGpuAddr descriptors = NxDeko::DataHeap->GpuAddr(ImageDescriptors);
        EmuCmdBuf.pushData(descriptors + (descriptorOffset_TexcacheStart + array.ImageDescriptor) * sizeof(DkImageDescriptor),
            &descriptor,
            sizeof(DkImageDescriptor));

        //printf("allocating new layer set for %d %d %d %d\n", width, height, texArrays.size()-1, array.ImageDescriptor);

        for (u16 i = 0; i < layers; i++)
        {
            freeTextures.push_back(TexArrayEntry{(u16)(texArrays.size()-1), i});
        }
    }

    TexArrayEntry storagePlace = freeTextures[freeTextures.size()-1];
    freeTextures.pop_back();

    TexArray& array = texArrays[storagePlace.TexArrayIdx];
    //printf("using storage place %d %d | %d %d (%d)\n", width, height, storagePlace.TexArrayIdx, storagePlace.LayerIdx, array.ImageDescriptor);

    UploadBuf.UploadAndCopyTexture(NxDeko::EmuCmdBuf, array.Image,
        (u8*)TextureDecodingBuffer,
        0, 0, width, height,
        width*4,
        storagePlace.LayerIdx);

    entry.DescriptorIdx = array.ImageDescriptor;
    entry.Texture = storagePlace;

    return TexCache.emplace(std::make_pair(key, entry)).first->second;
}

struct Variant
{
    s16 Texture, Sampler;
    u16 Width, Height;
    u8 BlendMode;

    bool operator==(const Variant& other) const
    {
        return Texture == other.Texture && Sampler == other.Sampler && BlendMode == other.BlendMode;
    }
};

/*
    Antialiasing
    W-Buffer
    Mit Textur
    0
    1, 3
    2
    Ohne Textur
    2
    0, 1, 3

    => 20 Shader + 1x Shadow Mask
*/

void DekoRenderer::RenderFrame(GPU& gpu)
{
    auto textureDirty = gpu.VRAMDirty_Texture.DeriveState(gpu.VRAMMap_Texture, gpu);
    auto texPalDirty = gpu.VRAMDirty_TexPal.DeriveState(gpu.VRAMMap_TexPal, gpu);

    bool textureChanged = gpu.MakeVRAMFlat_TextureCoherent(textureDirty);
    bool texPalChanged = gpu.MakeVRAMFlat_TexPalCoherent(texPalDirty);

    if (textureChanged || texPalChanged)
    {
        //printf("check invalidation %d\n", TexCache.size());
        for (auto it = TexCache.begin(); it != TexCache.end();)
        {
            TexCacheEntry& entry = it->second;
            if (textureChanged)
            {
                for (u32 i = 0; i < 2; i++)
                {
                    u32 startBit = entry.TextureRAMStart[i] / VRAMDirtyGranularity;
                    u32 bitsCount = ((entry.TextureRAMStart[i] + entry.TextureRAMSize[i] + VRAMDirtyGranularity - 1) / VRAMDirtyGranularity) - startBit;

                    u32 startEntry = startBit >> 6;
                    u64 entriesCount = ((startBit + bitsCount + 0x3F) >> 6) - startEntry;
                    for (u32 j = startEntry; j < startEntry + entriesCount; j++)
                    {
                        if (GetRangedBitMask(j, startBit, bitsCount) & textureDirty.Data[j])
                        {
                            u64 newTexHash = XXH3_64bits(&gpu.VRAMFlat_Texture[entry.TextureRAMStart[i]], entry.TextureRAMSize[i]);

                            if (newTexHash != entry.TextureHash[i])
                                goto invalidate;
                        }
                    }
                }
            }

            if (texPalChanged && entry.TexPalSize > 0)
            {
                u32 startBit = entry.TexPalStart / VRAMDirtyGranularity;
                u32 bitsCount = ((entry.TexPalStart + entry.TexPalSize + VRAMDirtyGranularity - 1) / VRAMDirtyGranularity) - startBit;

                u32 startEntry = startBit >> 6;
                u64 entriesCount = ((startBit + bitsCount + 0x3F) >> 6) - startEntry;
                for (u32 j = startEntry; j < startEntry + entriesCount; j++)
                {
                    if (GetRangedBitMask(j, startBit, bitsCount) & texPalDirty.Data[j])
                    {
                        u64 newPalHash = XXH3_64bits(&gpu.VRAMFlat_TexPal[entry.TexPalStart], entry.TexPalSize);
                        if (newPalHash != entry.TexPalHash)
                            goto invalidate;
                    }
                }
            }

            it++;
            continue;
        invalidate:
            FreeTextures[entry.WidthLog2][entry.HeightLog2].push_back(entry.Texture);

            //printf("invalidating texture %d\n", entry.ImageDescriptor);

            it = TexCache.erase(it);
        }
    }
    else if (gpu.GPU3D.RenderFrameIdentical && OutputSlot >= 0 && RequestedScale == CurScale)
    {
        return;
    }
    ApplyScale();

    int numYSpans = 0;
    int numSetupIndices = 0;

    u32 curSlice = CmdMem.Begin(EmuCmdBuf);

    u32 numVariants = 0, prevVariant, prevTexLayer;
    Variant variants[MaxVariants];

    int foundviatexcache = 0, foundviaprev = 0, numslow = 0;

    bool enableTextureMaps = gpu.GPU3D.RenderDispCnt & (1<<0);

    for (int i = 0; i < gpu.GPU3D.RenderNumPolygons; i++)
    {
        Polygon* polygon = gpu.GPU3D.RenderPolygonRAM[i];

        u32 nverts = polygon->NumVertices;
        u32 vtop = polygon->VTop, vbot = polygon->VBottom;
        s32 ytop = polygon->YTop, ybot = polygon->YBottom;
        // MPH-nx scaler: vertex positions at render size. 1x keeps the exact
        // native positions; larger sizes use the sub-pixel positions (4
        // fraction bits), as melonDS's compute renderer does.
        if (CurScale == 0)
        {
            for (u32 k = 0; k < nverts; k++)
            {
                Pos[k][0] = polygon->Vertices[k]->FinalPosition[0];
                Pos[k][1] = polygon->Vertices[k]->FinalPosition[1];
            }
        }
        else
        {
            ytop = ScrH; ybot = 0;
            for (u32 k = 0; k < nverts; k++)
            {
                const s32 hx = polygon->Vertices[k]->HiresPosition[0];
                const s32 hy = polygon->Vertices[k]->HiresPosition[1];
                // HiresPosition has 4 fraction bits: render pos = pos * scale / 16
                switch (CurScale)
                {
                case 1: Pos[k][0] = (hx * 3) >> 5; Pos[k][1] = (hy * 3) >> 5; break;   // 1.5x
                case 2: Pos[k][0] = hx >> 3; Pos[k][1] = hy >> 3; break;               // 2x
                case 3: Pos[k][0] = (hx * 3) >> 4; Pos[k][1] = (hy * 3) >> 4; break;   // 3x
                default: Pos[k][0] = hx >> 2; Pos[k][1] = hy >> 2; break;              // 4x
                }
                ytop = std::min(ytop, Pos[k][1]);
                ybot = std::max(ybot, Pos[k][1]);
            }
        }

        u32 curVL = vtop, curVR = vtop;
        u32 nextVL, nextVR;

        RenderPolygons[i].FirstXSpan = numSetupIndices;
        RenderPolygons[i].YTop = ytop;
        RenderPolygons[i].YBot = ybot;
        RenderPolygons[i].Attr = polygon->Attr;

        bool foundVariant = false;
        if (i > 0)
        {
            Polygon* prevPolygon = gpu.GPU3D.RenderPolygonRAM[i - 1];
            foundVariant = prevPolygon->TexParam == polygon->TexParam
                && prevPolygon->TexPalette == polygon->TexPalette
                && (prevPolygon->Attr & 0x30) == (polygon->Attr & 0x30)
                && prevPolygon->IsShadowMask == polygon->IsShadowMask;
            if (foundVariant)
                foundviaprev++;
        }

        if (!foundVariant)
        {
            Variant variant;
            variant.BlendMode = polygon->IsShadowMask ? 4 : ((polygon->Attr >> 4) & 0x3);
            variant.Texture = -1;
            variant.Sampler = -1;
            TexCacheEntry* texcacheEntry = nullptr;
            if (enableTextureMaps && (polygon->TexParam >> 26) & 0x7)
            {
                texcacheEntry = &GetTexture(gpu, polygon->TexParam, polygon->TexPalette);
                bool wrapS = (polygon->TexParam >> 16) & 1;
                bool wrapT = (polygon->TexParam >> 17) & 1;
                bool mirrorS = (polygon->TexParam >> 18) & 1;
                bool mirrorT = (polygon->TexParam >> 19) & 1;
                variant.Sampler = (wrapS ? (mirrorS ? 2 : 1) : 0) + (wrapT ? (mirrorT ? 2 : 1) : 0) * 3;
                variant.Texture = texcacheEntry->DescriptorIdx;
                prevTexLayer = texcacheEntry->Texture.LayerIdx;
                if (texcacheEntry->LastVariant < numVariants && variants[texcacheEntry->LastVariant] == variant)
                {
                    foundVariant = true;
                    prevVariant = texcacheEntry->LastVariant;
                    foundviatexcache++;
                }
            }

            if (!foundVariant)
            {
                numslow++;
                for (int j = numVariants - 1; j >= 0; j--)
                {
                    if (variants[j] == variant)
                    {
                        foundVariant = true;
                        prevVariant = j;
                        goto foundVariant;
                    }
                }

                prevVariant = numVariants;
                variants[numVariants] = variant;
                variants[numVariants].Width = TextureWidth(polygon->TexParam);
                variants[numVariants].Height = TextureHeight(polygon->TexParam);
                numVariants++;
                assert(numVariants <= MaxVariants);
            foundVariant:;

                if (texcacheEntry)
                    texcacheEntry->LastVariant = prevVariant;
            }
        }
        RenderPolygons[i].Variant = prevVariant;
        RenderPolygons[i].TextureLayer = (float)prevTexLayer;

        if (polygon->FacingView)
        {
            nextVL = curVL + 1;
            if (nextVL >= nverts) nextVL = 0;
            nextVR = curVR - 1;
            if ((s32)nextVR < 0) nextVR = nverts - 1;
        }
        else
        {
            nextVL = curVL - 1;
            if ((s32)nextVL < 0) nextVL = nverts - 1;
            nextVR = curVR + 1;
            if (nextVR >= nverts) nextVR = 0;
        }

        s32 minX = Pos[vtop][0];
        s32 minXY = Pos[vtop][1];
        s32 maxX = Pos[vtop][0];
        s32 maxXY = Pos[vtop][1];

        if (ybot == ytop)
        {
            vtop = 0; vbot = 0;

            RenderPolygons[i].YBot++;

            int j = 1;
            if (Pos[j][0] < Pos[vtop][0]) vtop = j;
            if (Pos[j][0] > Pos[vbot][0]) vbot = j;

            j = nverts - 1;
            if (Pos[j][0] < Pos[vtop][0]) vtop = j;
            if (Pos[j][0] > Pos[vbot][0]) vbot = j;

            assert(numYSpans < MaxYSpanSetups);
            u32 curSpanL = numYSpans;
            SetupYSpanDummy(&YSpanSetups[numYSpans++], polygon, vtop, 0);
            assert(numYSpans < MaxYSpanSetups);
            u32 curSpanR = numYSpans;
            SetupYSpanDummy(&YSpanSetups[numYSpans++], polygon, vbot, 1);

            minX = YSpanSetups[curSpanL].X0;
            minXY = YSpanSetups[curSpanL].Y0;
            maxX = YSpanSetups[curSpanR].X0;
            maxXY = YSpanSetups[curSpanR].Y0;
            if (maxX < minX)
            {
                std::swap(minX, maxX);
                std::swap(minXY, maxXY);
            }

            if (numSetupIndices < MaxYSpanIndices)   // full: drop this polygon's line
            {
                YSpanIndices[numSetupIndices].PolyIdx = i;
                YSpanIndices[numSetupIndices].SpanIdxL = curSpanL;
                YSpanIndices[numSetupIndices].SpanIdxR = curSpanR;
                YSpanIndices[numSetupIndices].Y = ytop;
                numSetupIndices++;
            }
        }
        else
        {
            u32 curSpanL = numYSpans;
            assert(numYSpans < MaxYSpanSetups);
            SetupYSpan(i, &YSpanSetups[numYSpans++], polygon, curVL, nextVL, ytop, 0);
            u32 curSpanR = numYSpans;
            assert(numYSpans < MaxYSpanSetups);
            SetupYSpan(i, &YSpanSetups[numYSpans++], polygon, curVR, nextVR, ytop, 1);

            for (u32 y = ytop; y < ybot; y++)
            {
                if (y >= Pos[nextVL][1] && curVL != polygon->VBottom)
                {
                    while (y >= Pos[nextVL][1] && curVL != polygon->VBottom)
                    {
                        curVL = nextVL;
                        if (polygon->FacingView)
                        {
                            nextVL = curVL + 1;
                            if (nextVL >= nverts)
                                nextVL = 0;
                        }
                        else
                        {
                            nextVL = curVL - 1;
                            if ((s32)nextVL < 0)
                                nextVL = nverts - 1;
                        }
                    }

                    if (Pos[curVL][0] < minX)
                    {
                        minX = Pos[curVL][0];
                        minXY = Pos[curVL][1];
                    }
                    if (Pos[curVL][0] > maxX)
                    {
                        maxX = Pos[curVL][0];
                        maxXY = Pos[curVL][1];
                    }

                    assert(numYSpans < MaxYSpanSetups);
                    curSpanL = numYSpans;
                    SetupYSpan(i,&YSpanSetups[numYSpans++], polygon, curVL, nextVL, y, 0);
                }
                if (y >= Pos[nextVR][1] && curVR != polygon->VBottom)
                {
                    while (y >= Pos[nextVR][1] && curVR != polygon->VBottom)
                    {
                        curVR = nextVR;
                        if (polygon->FacingView)
                        {
                            nextVR = curVR - 1;
                            if ((s32)nextVR < 0)
                                nextVR = nverts - 1;
                        }
                        else
                        {
                            nextVR = curVR + 1;
                            if (nextVR >= nverts)
                                nextVR = 0;
                        }
                    }

                    if (Pos[curVR][0] < minX)
                    {
                        minX = Pos[curVR][0];
                        minXY = Pos[curVR][1];
                    }
                    if (Pos[curVR][0] > maxX)
                    {
                        maxX = Pos[curVR][0];
                        maxXY = Pos[curVR][1];
                    }

                    assert(numYSpans < MaxYSpanSetups);
                    curSpanR = numYSpans;
                    SetupYSpan(i,&YSpanSetups[numYSpans++], polygon, curVR, nextVR, y, 1);
                }

                if (numSetupIndices >= MaxYSpanIndices) break;   // full: drop the rest of this polygon
                assert(numSetupIndices < MaxYSpanIndices);
                YSpanIndices[numSetupIndices].PolyIdx = i;
                YSpanIndices[numSetupIndices].SpanIdxL = curSpanL;
                YSpanIndices[numSetupIndices].SpanIdxR = curSpanR;
                YSpanIndices[numSetupIndices].Y = y;
                numSetupIndices++;
            }
        }

        if (Pos[nextVL][0] < minX)
        {
            minX = Pos[nextVL][0];
            minXY = Pos[nextVL][1];
        }
        if (Pos[nextVL][0] > maxX)
        {
            maxX = Pos[nextVL][0];
            maxXY = Pos[nextVL][1];
        }
        if (Pos[nextVR][0] < minX)
        {
            minX = Pos[nextVR][0];
            minXY = Pos[nextVR][1];
        }
        if (Pos[nextVR][0] > maxX)
        {
            maxX = Pos[nextVR][0];
            maxXY = Pos[nextVR][1];
        }

        RenderPolygons[i].XMin = minX;
        RenderPolygons[i].XMinY = minXY;
        RenderPolygons[i].XMax = maxX;
        RenderPolygons[i].XMaxY = maxXY;

        //printf("polygon min max %d %d | %d %d\n", RenderPolygons[i].XMin, RenderPolygons[i].XMinY, RenderPolygons[i].XMax, RenderPolygons[i].XMaxY);
    }

    /*for (u32 i = 0; i < gpu.GPU3D.RenderNumPolygons; i++)
    {
        if (RenderPolygons[i].Variant >= numVariants)
        {
            printf("blarb2 %d %d %d\n", RenderPolygons[i].Variant, i, gpu.GPU3D.RenderNumPolygons);
        }
        //assert(RenderPolygons[i].Variant < numVariants);
    }*/
    DkGpuAddr gpuAddrBinResult = NxDeko::DataHeap->GpuAddr(BinResultMemory);
    DkGpuAddr gpuAddrMetaUniform = NxDeko::DataHeap->GpuAddr(MetaUniformMemory);

    if (numYSpans > 0)
    {
        SpanSetupY* yspans = NxDeko::DataHeap->CpuAddr<SpanSetupY>(YSpanSetupMemory[curSlice]);
        memcpy(yspans, YSpanSetups, sizeof(SpanSetupY)*numYSpans);
        UploadBuf.UploadAndCopyData(EmuCmdBuf, NxDeko::TextureHeap->GpuAddr(YSpanIndicesTextureMemory), (u8*)YSpanIndices, numSetupIndices*4*2);

        memcpy(NxDeko::DataHeap->CpuAddr<void>(RenderPolygonMemory[curSlice]), RenderPolygons, gpu.GPU3D.RenderNumPolygons*sizeof(RenderPolygon));

        // we haven't accessed image data yet, so we don't need to invalidate anything
        EmuCmdBuf.barrier(DkBarrier_Full, DkInvalidateFlags_Image|DkInvalidateFlags_Descriptors|DkInvalidateFlags_L2Cache);
    }

    //printf("found via %d %d %d of %d\n", foundviatexcache, foundviaprev, numslow, gpu.GPU3D.RenderNumPolygons);

    // bind everything
    EmuCmdBuf.bindImageDescriptorSet(NxDeko::DataHeap->GpuAddr(ImageDescriptors), descriptorOffset_Count);
    EmuCmdBuf.bindSamplerDescriptorSet(NxDeko::DataHeap->GpuAddr(SamplerDescriptors), 9);
    EmuCmdBuf.bindStorageBuffers(DkStage_Compute, 0,
    {
        {NxDeko::DataHeap->GpuAddr(YSpanSetupMemory[curSlice]), YSpanSetupMemory[curSlice].Size},
        {NxDeko::DataHeap->GpuAddr(XSpanSetupMemory), XSpanSetupMemory.Size},
        {NxDeko::DataHeap->GpuAddr(RenderPolygonMemory[curSlice]), RenderPolygonMemory[curSlice].Size},
        {gpuAddrBinResult, BinResultMemory.Size},
        {NxDeko::DataHeap->GpuAddr(TileMemory), TileMemory.Size},
        {NxDeko::DataHeap->GpuAddr(FinalTileMemory), FinalTileMemory.Size},
        CurScale > 0
            ? DkBufExtents{NxDeko::DataHeap->GpuAddr(HiresOutputMemory[curSlice]), HiresOutputMemory[curSlice].Size}
            : DkBufExtents{NxDeko::ReadbackHeap->GpuAddr(FinalOutputMemory[curSlice]), FinalOutputMemory[curSlice].Size}
    });

    MetaUniform meta;
    meta.DispCnt = gpu.GPU3D.RenderDispCnt;
    meta.NumPolygons = gpu.GPU3D.RenderNumPolygons;
    meta.NumVariants = numVariants;
    meta.AlphaRef = gpu.GPU3D.RenderAlphaRef;
    {
        u32 r = (gpu.GPU3D.RenderClearAttr1 << 1) & 0x3E; if (r) r++;
        u32 g = (gpu.GPU3D.RenderClearAttr1 >> 4) & 0x3E; if (g) g++;
        u32 b = (gpu.GPU3D.RenderClearAttr1 >> 9) & 0x3E; if (b) b++;
        u32 a = (gpu.GPU3D.RenderClearAttr1 >> 16) & 0x1F;
        meta.ClearColor = r | (g << 8) | (b << 16) | (a << 24);
        meta.ClearDepth = ((gpu.GPU3D.RenderClearAttr2 & 0x7FFF) * 0x200) + 0x1FF;
        meta.ClearAttr = gpu.GPU3D.RenderClearAttr1 & 0x3F008000;
    }
    for (u32 i = 0; i < 32; i++)
    {
        u32 color = gpu.GPU3D.RenderToonTable[i];
        u32 r = (color << 1) & 0x3E;
        u32 g = (color >> 4) & 0x3E;
        u32 b = (color >> 9) & 0x3E;
        if (r) r++;
        if (g) g++;
        if (b) b++;

        meta.ToonTable[i*4+0] = r | (g << 8) | (b << 16);
    }
    for (u32 i = 0; i < 34; i++)
    {
        meta.ToonTable[i*4+1] = gpu.GPU3D.RenderFogDensityTable[i];
    }
    for (u32 i = 0; i < 8; i++)
    {
        u32 color = gpu.GPU3D.RenderEdgeTable[i];
        u32 r = (color << 1) & 0x3E;
        u32 g = (color >> 4) & 0x3E;
        u32 b = (color >> 9) & 0x3E;
        if (r) r++;
        if (g) g++;
        if (b) b++;

        meta.ToonTable[i*4+2] = r | (g << 8) | (b << 16);
    }
    meta.FogOffset = gpu.GPU3D.RenderFogOffset;
    meta.FogShift = gpu.GPU3D.RenderFogShift;
    {
        u32 fogR = (gpu.GPU3D.RenderFogColor << 1) & 0x3E; if (fogR) fogR++;
        u32 fogG = (gpu.GPU3D.RenderFogColor >> 4) & 0x3E; if (fogG) fogG++;
        u32 fogB = (gpu.GPU3D.RenderFogColor >> 9) & 0x3E; if (fogB) fogB++;
        u32 fogA = (gpu.GPU3D.RenderFogColor >> 16) & 0x1F;
        meta.FogColor = fogR | (fogG << 8) | (fogB << 16) | (fogA << 24);
    }
    meta.XScroll = gpu.GPU3D.GetRenderXPos() * ScrW / 256;
    EmuCmdBuf.bindUniformBuffer(DkStage_Compute, 0, NxDeko::DataHeap->GpuAddr(MetaUniformMemory), MetaUniformSize);
    EmuCmdBuf.pushConstants(gpuAddrMetaUniform, MetaUniformSize, 0, sizeof(MetaUniform), &meta);

    EmuCmdBuf.bindShaders(DkStageFlag_Compute, {&S->ShaderClearCoarseBinMask});
    EmuCmdBuf.dispatchCompute(CurTilesPerLine*CurTileLines/32, 1, 1);

    bool wbuffer = false;
    if (numYSpans > 0)
    {
        wbuffer = gpu.GPU3D.RenderPolygonRAM[0]->WBuffer;

        EmuCmdBuf.bindShaders(DkStageFlag_Compute, {&S->ShaderClearIndirectWorkCount});
        EmuCmdBuf.dispatchCompute((numVariants+31)/32, 1, 1);

        // calculate x-spans
        EmuCmdBuf.bindImages(DkStage_Compute, 0, {dkMakeImageHandle(descriptorOffset_YSpanIndices)});
        EmuCmdBuf.bindShaders(DkStageFlag_Compute, {&S->ShaderInterpXSpans[wbuffer]});
        EmuCmdBuf.dispatchCompute((numSetupIndices + 31) / 32, 1, 1);
        EmuCmdBuf.barrier(DkBarrier_Primitives, 0);

        // bin polygons
        EmuCmdBuf.bindShaders(DkStageFlag_Compute, {&S->ShaderBinCombined});
        EmuCmdBuf.dispatchCompute(((gpu.GPU3D.RenderNumPolygons + 31) / 32), ScrW/CoarseTileW, ScrH/CoarseTileH);
        EmuCmdBuf.barrier(DkBarrier_Primitives, 0);

        // calculate list offsets
        EmuCmdBuf.bindShaders(DkStageFlag_Compute, {&S->ShaderCalculateWorkListOffset});
        EmuCmdBuf.dispatchCompute((numVariants + 31) / 32, 1, 1);
        EmuCmdBuf.barrier(DkBarrier_Primitives, 0);

        // sort shader work
        EmuCmdBuf.bindShaders(DkStageFlag_Compute, {&S->ShaderSortWork});
        EmuCmdBuf.dispatchComputeIndirect(gpuAddrBinResult + offsetof(BinResult, SortWorkWorkCount));
        EmuCmdBuf.barrier(DkBarrier_Primitives, 0);

        // rasterise
        {
            bool highLightMode = gpu.GPU3D.RenderDispCnt & (1<<1);

            dk::Shader* shadersNoTexture[] =
            {
                &S->ShaderRasteriseNoTexture[wbuffer],
                &S->ShaderRasteriseNoTexture[wbuffer],
                highLightMode
                    ? &S->ShaderRasteriseNoTextureHighlight[wbuffer]
                    : &S->ShaderRasteriseNoTextureToon[wbuffer],
                &S->ShaderRasteriseNoTexture[wbuffer],
                &S->ShaderRasteriseShadowMask[wbuffer]
            };
            dk::Shader* shadersUseTexture[] =
            {
                &S->ShaderRasteriseUseTextureModulate[wbuffer],
                &S->ShaderRasteriseUseTextureDecal[wbuffer],
                highLightMode
                    ? &S->ShaderRasteriseUseTextureHighlight[wbuffer]
                    : &S->ShaderRasteriseUseTextureToon[wbuffer],
                &S->ShaderRasteriseUseTextureDecal[wbuffer],
                &S->ShaderRasteriseShadowMask[wbuffer]
            };

            dk::Shader* prevShader = NULL;
            s32 prevTexture = -1, prevSampler = -1;
            for (int i = 0; i < numVariants; i++)
            {
                dk::Shader* shader = NULL;
                if (variants[i].Texture == -1)
                {
                    shader = shadersNoTexture[variants[i].BlendMode];
                }
                else
                {
                    shader = shadersUseTexture[variants[i].BlendMode];
                    if (variants[i].Texture != prevTexture || variants[i].Sampler != prevSampler)
                    {
                        assert(variants[i].Sampler < 9);
                        EmuCmdBuf.bindTextures(DkStage_Compute, 0,
                        {
                            dkMakeTextureHandle(descriptorOffset_TexcacheStart + variants[i].Texture, variants[i].Sampler)
                        });
                        prevTexture = variants[i].Texture;
                        prevSampler = variants[i].Sampler;
                        meta.InvTextureSize[0] = 1.f / variants[i].Width;
                        meta.InvTextureSize[1] = 1.f / variants[i].Height;
                    }
                }
                assert(shader != NULL);
                if (shader != prevShader)
                {
                    EmuCmdBuf.bindShaders(DkStageFlag_Compute, {shader});
                    prevShader = shader;
                }
                meta.CurVariant = i;
                // not pretty, but alignment shouldn't matter as we only have 4 byte values
                EmuCmdBuf.pushConstants(gpuAddrMetaUniform, MetaUniformSize, offsetof(MetaUniform, CurVariant), 4*3, &meta.CurVariant);
                EmuCmdBuf.dispatchComputeIndirect(gpuAddrBinResult + offsetof(BinResult, VariantWorkCount) + i*4*4);
            }
        }
        EmuCmdBuf.barrier(DkBarrier_Primitives, 0);
    }
    else
    {
        EmuCmdBuf.barrier(DkBarrier_Primitives, 0);
    }

    // compose final image
    EmuCmdBuf.bindShaders(DkStageFlag_Compute, {&S->ShaderDepthBlend[wbuffer]});
    EmuCmdBuf.dispatchCompute(ScrW/8, ScrH/8, 1);
    EmuCmdBuf.barrier(DkBarrier_Primitives, 0);

    u32 finalPassShader = 0;
    if (gpu.GPU3D.RenderDispCnt & (1<<4))
        finalPassShader |= 0x4;
    if (gpu.GPU3D.RenderDispCnt & (1<<7))
        finalPassShader |= 0x2;
    if (gpu.GPU3D.RenderDispCnt & (1<<5))
        finalPassShader |= 0x1;
    EmuCmdBuf.bindShaders(DkStageFlag_Compute, {&S->ShaderFinalPass[finalPassShader]});
    EmuCmdBuf.dispatchCompute(ScrW/32, ScrH, 1);
    EmuCmdBuf.barrier(DkBarrier_Primitives, 0);
    if (CurScale > 0)
    {
        // MPH-nx scaler: the 2D compositor and display capture keep reading a
        // 256x192 image; take one sample per native pixel from the full one.
        EmuCmdBuf.bindStorageBuffers(DkStage_Compute, 6,
        {
            {NxDeko::DataHeap->GpuAddr(HiresOutputMemory[curSlice]), HiresOutputMemory[curSlice].Size},
            {NxDeko::ReadbackHeap->GpuAddr(FinalOutputMemory[curSlice]), FinalOutputMemory[curSlice].Size}
        });
        EmuCmdBuf.bindShaders(DkStageFlag_Compute, {&S->ShaderDownsample});
        EmuCmdBuf.dispatchCompute(256/32, 192, 1);
        EmuCmdBuf.barrier(DkBarrier_Primitives, 0);
    }
    SlotScale[curSlice] = CurScale;

    // Flush GPU caches with the fence so the CPU sees the finished image.
    EmuCmdBuf.signalFence(FinalOutputFence[curSlice], true);
    DkCmdList cmdlist = CmdMem.End(EmuCmdBuf);
    EmuQueue.submitCommands(cmdlist);
    EmuQueue.flush();
    UploadBuf.LastFlushBuffer = 0;
    if (OutputSlot != (int)curSlice) PrevSlot = OutputSlot;
    OutputSlot = (int)curSlice;
    SlotSwap[curSlice] = (nds_powercontrol9() >> 15) & 1;
    OutputWaited = false;

    /*u64 starttime = armGetSystemTick();
    EmuQueue.waitIdle();
    printf("total time %f\n", armTicksToNs(armGetSystemTick()-starttime)*0.000001f);*/

    /*for (u32 i = 0; i < gpu.GPU3D.RenderNumPolygons; i++)
    {
        if (RenderPolygons[i].Variant >= numVariants)
        {
            printf("blarb %d %d %d\n", RenderPolygons[i].Variant, i, gpu.GPU3D.RenderNumPolygons);
        }
        //assert(RenderPolygons[i].Variant < numVariants);
    }*/

    /*for (int i = 0; i < binresult->SortWorkWorkCount[0]*32; i++)
    {
        printf("sorted %x %x\n", binresult->SortedWork[i*2+0], binresult->SortedWork[i*2+1]);
    }*/
/*    if (polygonvisible != -1)
    {
        SpanSetupX* xspans = NxDeko::DataHeap->CpuAddr<SpanSetupX>(XSpanSetupMemory);
        printf("span result\n");
        Polygon* poly = gpu.GPU3D.RenderPolygonRAM[polygonvisible];
        u32 xspanoffset = RenderPolygons[polygonvisible].FirstXSpan;
        for (u32 i = 0; i < (poly->YBottom - poly->YTop); i++)
        {
            printf("%d: %d - %d | %d %d | %d %d\n", i + poly->YTop, xspans[xspanoffset + i].X0, xspans[xspanoffset + i].X1, xspans[xspanoffset + i].__pad0, xspans[xspanoffset + i].__pad1, RenderPolygons[polygonvisible].YTop, RenderPolygons[polygonvisible].YBot);
        }
    }*/
/*
    printf("xspans: %d\n", numSetupIndices);
    SpanSetupX* xspans = NxDeko::DataHeap->CpuAddr<SpanSetupX>(XSpanSetupMemory[curSlice]);
    for (int i = 0; i < numSetupIndices; i++)
    {
        printf("poly %d %d %d | line %d | %d to %d\n", YSpanIndices[i].PolyIdx, YSpanIndices[i].SpanIdxL, YSpanIndices[i].SpanIdxR, YSpanIndices[i].Y, xspans[i].X0, xspans[i].X1);
    }
    printf("bin result\n");
    BinResult* binresult = NxDeko::DataHeap->CpuAddr<BinResult>(BinResultMemory);
    for (u32 y = 0; y < 192/8; y++)
    {
        for (u32 x = 0; x < 256/8; x++)
        {
            printf("%08x ", binresult->BinnedMaskCoarse[(x + y * (256/8)) * 2]);
        }
        printf("\n");
    }*/
}

void DekoRenderer::ApplyScale()
{
    if (RequestedScale == CurScale) return;
    CurScale = (RequestedScale >= 0 && RequestedScale <= 4) ? RequestedScale : 0;
    S = &Sets[CurScale];
    static const int kW[5] = {256, 384, 512, 768, 1024}, kH[5] = {192, 288, 384, 576, 768};
    ScrW = kW[CurScale];
    ScrH = kH[CurScale];
    CurTilesPerLine = ScrW / TileSize;
    CurTileLines = ScrH / TileSize;
}

bool DekoRenderer::HiresOutput(uint64_t* gpuAddr, int* w, int* h)
{
    if (ShownSlot < 0 || !OutputWaited || SlotScale[ShownSlot] == 0) return false;
    const int sc = SlotScale[ShownSlot];
    *gpuAddr = NxDeko::DataHeap->GpuAddr(HiresOutputMemory[ShownSlot]);
    static const int kW[5] = {256, 384, 512, 768, 1024}, kH[5] = {192, 288, 384, 576, 768};
    *w = kW[sc];
    *h = kH[sc];
    return true;
}

void DekoRenderer::RestartFrame(GPU& gpu)
{

}

}
unsigned long long g_nx_deko_late_frames = 0;
// MPH-nx test11: steady 3D pacing. Always show the previous finished frame
// (a constant one-frame 3D delay) instead of alternating between the newest
// and the previous one whenever the GPU runs a little late.
bool g_nx_3d_steady = true;
namespace melonDS {
u32* DekoRenderer::GetLine(int line)
{
    if (OutputSlot < 0 || line < 0 || line >= 192)
        return DummyLine;
    if (!OutputWaited)
    {
        // One decision per frame. MPH-nx test10: if the GPU has not finished
        // the newest frame yet, show the previous finished one instead of
        // stalling the emulation thread (3D can lag 2D by one frame, and the
        // shown sequence never goes backwards).
        ShownSlot = OutputSlot;
        // The previous frame may stand in only when it was drawn for the same
        // screen. Dual-screen 3D (MPH menus) flips the screen swap bit every
        // frame, so there the previous 3D frame belongs to the other screen.
        const bool prevSameScreen = PrevSlot >= 0 && SlotSwap[PrevSlot] == SlotSwap[OutputSlot];
        if (g_nx_3d_steady && prevSameScreen)
        {
            ShownSlot = PrevSlot;
            FinalOutputFence[PrevSlot].wait();  // normally already done
        }
        else if (FinalOutputFence[OutputSlot].wait(0) != DkResult_Success)
        {
            if (prevSameScreen)
            {
                ShownSlot = PrevSlot;
                FinalOutputFence[PrevSlot].wait();  // normally already done
                ++LateFrames;
                ++g_nx_deko_late_frames;
            }
            else
                FinalOutputFence[OutputSlot].wait();
        }
        OutputWaited = true;
        // The GPU wrote this slot behind the CPU cache: drop stale lines.
        armDCacheFlush(NxDeko::ReadbackHeap->CpuAddr<void>(FinalOutputMemory[ShownSlot]), 256*192*4);
    }
    return NxDeko::ReadbackHeap->CpuAddr<u32>(FinalOutputMemory[ShownSlot]) + line * 256;
}

// MPH-nx resolution scaler bridge (frontend side, C linkage-free).
static DekoRenderer* g_nx_deko_renderer = nullptr;
static int g_nx_deko_scale_req = 0;

// MPH-nx factory used by the runner bridge. Null when deko3d is unavailable.
Renderer3D* NxCreateDekoRenderer()
{
    if (!NxDeko::Init())
        return nullptr;
    DekoRenderer* renderer = new DekoRenderer();
    if (!renderer->Init())
    {
        delete renderer;
        return nullptr;
    }
    g_nx_deko_renderer = renderer;
    renderer->RequestedScale = g_nx_deko_scale_req;
    return renderer;
}

}

// Scale index 0 (1x), 1 (1.5x), 2 (2x); applied at the next 3D frame.
void nx_deko_set_scale(int scale)
{
    melonDS::g_nx_deko_scale_req = scale;
    if (melonDS::g_nx_deko_renderer) melonDS::g_nx_deko_renderer->RequestedScale = scale;
}
// The render-size 3D image the 2D compositor is showing this frame.
bool nx_deko_hires(uint64_t* gpuAddr, int* w, int* h)
{
    return melonDS::g_nx_deko_renderer &&
           melonDS::g_nx_deko_renderer->HiresOutput(gpuAddr, w, h);
}
