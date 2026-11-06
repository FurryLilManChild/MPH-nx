// MPH-nx: minimal deko3d context for the GPU 3D renderer.
// Adapted from RSDuck's melonDS Switch port (frontend/switch/Gfx.cpp), GPL-3.0.
#include "nx_deko.h"

#include <cstdio>
#include <cstring>
#include <memory>

struct NxDekoShaderBlob { const char* name; const unsigned char* data; unsigned size; };
extern "C" const NxDekoShaderBlob g_nx_deko_shaders[];
extern "C" const unsigned g_nx_deko_shader_count;

namespace
{
struct DkshHeader
{
    uint32_t magic, header_sz, control_sz, code_sz, programs_off, num_programs;
};
}

namespace NxDeko
{
dk::Device Device;
dk::Queue EmuQueue;
dk::CmdBuf EmuCmdBuf;
std::optional<GpuMemHeap> TextureHeap;
std::optional<GpuMemHeap> ShaderCodeHeap;
std::optional<GpuMemHeap> DataHeap;

namespace
{
bool g_ready = false;

void DebugOutput(void*, const char* context, DkResult result, const char* message)
{
    std::fprintf(stderr, "[deko3d] %s: result %d: %s\n", context ? context : "?", (int)result,
                 message ? message : "");
}
}

bool Init()
{
    if (g_ready) return true;
    Device = dk::DeviceMaker{}.setCbDebug(DebugOutput).create();
    if (!Device) { std::fprintf(stderr, "[deko3d] device creation failed\n"); return false; }
    EmuQueue = dk::QueueMaker{Device}
        .setFlags(DkQueueFlags_HighPrio | DkQueueFlags_Compute | DkQueueFlags_DisableZcull)
        .setCommandMemorySize(DK_QUEUE_MIN_CMDMEM_SIZE * 4)
        .setFlushThreshold(DK_QUEUE_MIN_CMDMEM_SIZE)
        .create();
    TextureHeap.emplace(Device, 1024 * 1024 * 128, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, 1024 * 8);
    ShaderCodeHeap.emplace(Device, 1024 * 1024 * 4,
        DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code, 64);
    DataHeap.emplace(Device, 1024 * 1024 * 160, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, 256);
    EmuCmdBuf = dk::CmdBufMaker{Device}.create();
    g_ready = true;
    std::fprintf(stderr, "[deko3d] device ready (%u embedded shaders)\n", g_nx_deko_shader_count);
    return true;
}

bool LoadShader(const char* name, dk::Shader& out)
{
    for (unsigned i = 0; i < g_nx_deko_shader_count; ++i)
    {
        const NxDekoShaderBlob& blob = g_nx_deko_shaders[i];
        if (std::strcmp(blob.name, name) != 0) continue;
        DkshHeader header;
        std::memcpy(&header, blob.data, sizeof header);
        if (header.control_sz + header.code_sz > blob.size) break;
        std::unique_ptr<u8[]> ctrl(new u8[header.control_sz]);
        std::memcpy(ctrl.get(), blob.data, header.control_sz);
        GpuMemHeap::Allocation code = ShaderCodeHeap->Alloc(header.code_sz, DK_SHADER_CODE_ALIGNMENT);
        std::memcpy(ShaderCodeHeap->CpuAddr<void>(code), blob.data + header.control_sz, header.code_sz);
        dk::ShaderMaker{ShaderCodeHeap->MemBlock, code.Offset}
            .setControl(ctrl.get())
            .setProgramId(0)
            .initialize(out);
        return true;
    }
    std::fprintf(stderr, "[deko3d] shader %s missing\n", name);
    return false;
}
}
