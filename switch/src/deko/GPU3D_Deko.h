#ifndef GPU3D_DEKO
#define GPU3D_DEKO

#include "GPU3D.h"

#include <deko3d.hpp>

#include "CmdMemRing.h"
#include "GpuMemHeap.h"
#include "UploadBuffer.h"

#include "NonStupidBitfield.h"

#include <unordered_map>
#include <vector>

namespace melonDS
{

class DekoRenderer : public Renderer3D
{
public:
    DekoRenderer();
    ~DekoRenderer() override;

    // MPH-nx: bring up shaders and GPU memory; false keeps the caller on the
    // software renderer.
    bool Init();
    void Reset(GPU& gpu) override;

    void VCount144(GPU& gpu) override;

    void RenderFrame(GPU& gpu) override;
    void RestartFrame(GPU& gpu) override;
    u32* GetLine(int line) override;

    //dk::Fence FrameReady = {};
    //dk::Fence FrameReserveFence = {};
private:
    // MPH-nx resolution scaler: one shader set per render size
    // (0: 256x192, 1: 384x288 (1.5x), 2: 512x384 (2x), 3: 768x576 (3x),
    // 4: 1024x768 (4x)).
    struct ShaderSet
    {
        dk::Shader ShaderInterpXSpans[2];
        dk::Shader ShaderBinCombined;
        dk::Shader ShaderDepthBlend[2];
        dk::Shader ShaderRasteriseNoTexture[2];
        dk::Shader ShaderRasteriseNoTextureToon[2];
        dk::Shader ShaderRasteriseNoTextureHighlight[2];
        dk::Shader ShaderRasteriseUseTextureDecal[2];
        dk::Shader ShaderRasteriseUseTextureModulate[2];
        dk::Shader ShaderRasteriseUseTextureToon[2];
        dk::Shader ShaderRasteriseUseTextureHighlight[2];
        dk::Shader ShaderRasteriseShadowMask[2];
        dk::Shader ShaderClearCoarseBinMask;
        dk::Shader ShaderClearIndirectWorkCount;
        dk::Shader ShaderCalculateWorkListOffset;
        dk::Shader ShaderSortWork;
        dk::Shader ShaderFinalPass[8];
        dk::Shader ShaderDownsample;
    };
    ShaderSet Sets[5];
    bool SetLoaded[5] = {};
    ShaderSet* S = &Sets[0];
    bool LoadSet(int si, const char* sfx);
    bool InitRest();
public:
    // Requested scale index (0 to 4); applied at the next frame.
    int RequestedScale = 0;
    int CurScale = 0;
    int ScrW = 256, ScrH = 192;
    // GPU address and size of the full-size output for the presenter
    // (packed RGB6A5, ScrW x ScrH) of the slot GetLine shows this frame.
    bool HiresOutput(uint64_t* gpuAddr, int* w, int* h);
private:
    s32 Pos[10][2] = {};       // the current polygon's vertices at render size

    CmdMemRing<2> CmdMem;
    GpuMemHeap::Allocation YSpanIndicesTextureMemory;
    dk::Image YSpanIndicesTexture;
    GpuMemHeap::Allocation YSpanSetupMemory[2];
    GpuMemHeap::Allocation XSpanSetupMemory;
    GpuMemHeap::Allocation BinResultMemory;
    GpuMemHeap::Allocation RenderPolygonMemory[2];
    GpuMemHeap::Allocation TileMemory;
    GpuMemHeap::Allocation FinalTileMemory;

    // MPH-nx: the final pass writes packed RGB6A5 pixels into this
    // CPU-visible buffer (double buffered), read back through GetLine for
    // the software 2D compositor.
    GpuMemHeap::Allocation FinalOutputMemory[2];
    GpuMemHeap::Allocation HiresOutputMemory[2];   // render size, for the presenter
    int SlotScale[2] = {0, 0};
    dk::Fence FinalOutputFence[2] = {};
    int OutputSlot = -1;      // slot holding the latest submitted frame
    int PrevSlot = -1;        // the frame before it (finished by now)
    int SlotSwap[2] = {-1, -1}; // POWCNT1 bit 15 when each slot was rendered
    int ShownSlot = -1;       // what GetLine returns this frame
    bool OutputWaited = false;
public:
    unsigned long long LateFrames = 0;  // frames shown from PrevSlot
private:

    GpuMemHeap::Allocation ImageDescriptors;
    GpuMemHeap::Allocation SamplerDescriptors;

    struct MetaUniform
    {
        u32 NumPolygons;
        u32 NumVariants;

        u32 AlphaRef;
        u32 DispCnt;

        u32 ToonTable[4*34];

        u32 ClearColor, ClearDepth, ClearAttr;

        u32 FogOffset, FogShift, FogColor;

        u32 XScroll;
        // only used/updated for rasteriation
        u32 CurVariant;
        float InvTextureSize[2];
    };
    GpuMemHeap::Allocation MetaUniformMemory;
    const int MetaUniformSize = (sizeof(MetaUniform) + DK_UNIFORM_BUF_ALIGNMENT - 1) & ~(DK_UNIFORM_BUF_ALIGNMENT - 1);

    UploadBuffer UploadBuf;

    static const u32 TexCacheMaxImages = 4096;

    enum
    {
        descriptorOffset_YSpanIndices,
        descriptorOffset_FinalFB,
        descriptorOffset_WhiteTexture,
        descriptorOffset_TexcacheStart,
        descriptorOffset_Count = descriptorOffset_TexcacheStart + TexCacheMaxImages
    };

    u32 DummyLine[256] = {};

    struct SpanSetupY
    {
        // Attributes
        s32 Z0, Z1, W0, W1;
        s32 ColorR0, ColorG0, ColorB0;
        s32 ColorR1, ColorG1, ColorB1;
        s32 TexcoordU0, TexcoordV0;
        s32 TexcoordU1, TexcoordV1;

        // Interpolator
        s32 I0, I1;
        s32 Linear;
        s32 IRecip;
        s32 W0n, W0d, W1d;

        // Slope
        s32 Increment;

        s32 X0, X1, Y0, Y1;
        s32 XMin, XMax;
        s32 DxInitial;

        s32 XCovIncr;
        u32 IsDummy, __pad1;
    };
    struct SpanSetupX
    {
        s32 X0, X1;

        s32 EdgeLenL, EdgeLenR, EdgeCovL, EdgeCovR;

        s32 XRecip;

        u32 Flags;

        s32 Z0, Z1, W0, W1;
        s32 ColorR0, ColorG0, ColorB0;
        s32 ColorR1, ColorG1, ColorB1;
        s32 TexcoordU0, TexcoordV0;
        s32 TexcoordU1, TexcoordV1;

        s32 CovLInitial, CovRInitial;
    };
    struct SetupIndices
    {
        u16 PolyIdx, SpanIdxL, SpanIdxR, Y;
    };
    struct RenderPolygon
    {
        u32 FirstXSpan;
        s32 YTop, YBot;

        s32 XMin, XMax;
        s32 XMinY, XMaxY;

        u32 Variant;
        u32 Attr;

        float TextureLayer;
        u32 __pad0, __pad1;
    };

    static const int TileSize = 8;
    static const int CoarseTileCountX = 8;
    static const int CoarseTileCountY = 4;
    static const int CoarseTileW = CoarseTileCountX * TileSize;
    static const int CoarseTileH = CoarseTileCountY * TileSize;

    // Buffer sizes cover the largest render size (1024x768, 4x); the shaders
    // of each set use the prefix of every array that their size needs.
    static const int TilesPerLine = 1024/TileSize;
    static const int TileLines = 768/TileSize;
    int CurTilesPerLine = 256/TileSize, CurTileLines = 192/TileSize;

    static const int BinStride = 2048/32;
    static const int CoarseBinStride = BinStride/32;

    // Work tiles (polygon x tile pairs): each shader set uses at most
    // TilesPerLine*TileLines*WorkTileMul of them (build_deko_shaders.sh).
    static const int MaxWorkTiles = 128*96*14;   // Tiles buffer stays under the 128 MiB storage block limit
    static const int MaxVariants = 256;

    struct BinResult
    {
        u32 VariantWorkCount[MaxVariants*4];
        u32 SortedWorkOffset[MaxVariants];

        u32 SortWorkWorkCount[4];
        u32 UnsortedWorkDescs[MaxWorkTiles*2];
        u32 SortedWork[MaxWorkTiles*2];

        u32 BinnedMaskCoarse[TilesPerLine*TileLines*CoarseBinStride];
        u32 BinnedMask[TilesPerLine*TileLines*BinStride];
        u32 WorkOffsets[TilesPerLine*TileLines*BinStride];
    };

    struct Tiles
    {
        u32 ColorTiles[MaxWorkTiles*TileSize*TileSize];
        u32 DepthTiles[MaxWorkTiles*TileSize*TileSize];
        u32 AttrStencilTiles[MaxWorkTiles*TileSize*TileSize];
    };

    struct FinalTiles
    {
        u32 ColorResult[1024*768*2];
        u32 DepthResult[1024*768*2];
        u32 AttrResult[1024*768*2];
    };

    // eh those are pretty bad guesses
    // though real hw shouldn't be eable to render all 2048 polygons on every line either
    // One per polygon per scanline: render size scales it (4x: 4x the lines).
    static const int MaxYSpanIndices = 64*2048*8;
    static const int MaxYSpanSetups = 6144*2;
    SetupIndices YSpanIndices[MaxYSpanIndices];
    SpanSetupY YSpanSetups[MaxYSpanSetups];
    RenderPolygon RenderPolygons[2048];

    struct TexArrayEntry
    {
        u16 TexArrayIdx;
        u16 LayerIdx;
    };
    struct TexArray
    {
        GpuMemHeap::Allocation Memory;
        dk::Image Image;
        u32 ImageDescriptor;
    };

    struct TexCacheEntry
    {
        u32 DescriptorIdx;
        u32 LastVariant; // very cheap way to make variant lookup faster

        u32 TextureRAMStart[2], TextureRAMSize[2];
        u32 TexPalStart, TexPalSize;
        u8 WidthLog2, HeightLog2;
        TexArrayEntry Texture;

        u64 TextureHash[2];
        u64 TexPalHash;
    };
    std::unordered_map<u64, TexCacheEntry> TexCache;

    u32 FreeImageDescriptorsCount = 0;
    u32 FreeImageDescriptors[TexCacheMaxImages];

    std::vector<TexArrayEntry> FreeTextures[8][8];
    std::vector<TexArray> TexArrays[8][8];

    u32 TextureDecodingBuffer[1024*1024];

    TexCacheEntry& GetTexture(GPU& gpu, u32 textureParam, u32 paletteParam);

    void SetupAttrs(SpanSetupY* span, Polygon* poly, int from, int to);
    void SetupYSpan(int polynum, SpanSetupY* span, Polygon* poly, int from, int to, u32 y, int side);
    void ApplyScale();
    void SetupYSpanDummy(SpanSetupY* span, Polygon* poly, int vertex, int side);
};

}

#endif