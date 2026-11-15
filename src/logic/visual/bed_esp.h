// Bed ESP — highlights beds.
// category: visual
// functions: gui bed_esp_gui, register bed_esp_register (defined in bed_esp.cpp)
// implementation: src/logic/visual/bed_esp.cpp
//
// RECONSTRUCTION ADDITION. The original client had no bed module: a bed was one entry of Block ESP's
// allowed_blocks list ("Bed block", id 26), which tied it to Block ESP's range, colour and render limit and
// to the global block-search tables. This module is independent - it collects beds from the world's loaded
// tile entities (head and foot are separate TileEntityBed instances) and never touches that search table.
//
// Everything (descriptor, setting objects, callbacks) is defined in bed_esp.cpp: unlike the reconstructed
// modules this one is not tied to byte offsets in the original image, so it declares its own storage.
#pragma once
#include "slinky/api.h"

extern "C" void bed_esp_gui(void *gui, void *window);
extern "C" void bed_esp_register(void *module);
