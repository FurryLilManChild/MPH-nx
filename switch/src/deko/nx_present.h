// MPH-nx: deko3d screen presenter. Replaces the SDL renderer (and with it
// mesa/OpenGL) on the frame path: both DS screens go from CPU memory into
// pitch-linear images and the 2D engine scales them into the swapchain.
#pragma once
#include <cstdint>

struct NxRect { int x, y, w, h; };

struct NxPresentFrame
{
    const uint32_t* top;      // 256 x 192, 0xAARRGGBB
    const uint32_t* bottom;   // 256 x 192
    NxRect top_rect;          // in 1280 x 720 window pixels
    NxRect bottom_rect;       // w == 0: bottom not drawn
    bool dim_top;             // darken the top screen (bottom shown over it)
    bool bottom_over_top;     // draw order: bottom after top (overlay)
    bool cursor;              // crosshair on the bottom screen
    float cursor_x, cursor_y; // DS pixels
    const char* hud_text;     // top-left text, nullptr or "" for none
    const char* notice;       // bottom-screen notice, nullptr for none
    const char* (*glyph)(char); // 5x7 font: 35 chars of '0'/'1'
};

struct NxPresentTicks { uint64_t upload, draw, swap; };

// Creates the deko3d device (shared with the 3D renderer), a graphics queue
// and a 1280x720 swapchain on the default window. False: keep SDL.
bool nx_present_init();
bool nx_present_active();
void nx_present_frame(const NxPresentFrame& frame, NxPresentTicks* ticks);
