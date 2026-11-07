// MPH-nx: minimal deko3d context for the GPU 3D renderer.
// Adapted from RSDuck's melonDS Switch port (frontend/switch/Gfx), GPL-3.0.
#pragma once
#include <deko3d.hpp>
#include <optional>
#include "GpuMemHeap.h"

namespace NxDeko
{
extern dk::Device Device;
extern dk::Queue EmuQueue;
extern dk::CmdBuf EmuCmdBuf;
extern std::optional<GpuMemHeap> TextureHeap;
extern std::optional<GpuMemHeap> ShaderCodeHeap;
extern std::optional<GpuMemHeap> DataHeap;
extern std::optional<GpuMemHeap> ReadbackHeap;

// Creates the device, queue and heaps once. Returns false if deko3d could
// not be brought up; the caller then keeps the software renderer.
bool Init();
bool LoadShader(const char* name, dk::Shader& out);
}
