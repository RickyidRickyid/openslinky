// Bow Aim Assist — predicts the arrow's flight and aims it at a target.
// category: combat   descriptor: defined in bow_aim_assist.cpp
// functions: gui bow_aim_assist_gui, register bow_aim_assist_register
//
// The aiming behaviour follows Driplite's aim_assist, because the thing that makes that one feel
// good is not the target maths but three specific choices:
//
//   1. It only engages while you are actually drawing the bow. Nothing moves the crosshair while
//      you are just walking around, so it never fights your normal mouse aim.
//   2. The correction speed grows with the *cube* of the angle still to cover. Small offsets —
//      the ones you notice as "it is fighting me" — get almost no correction; a large offset,
//      like a target that just came into view, gets nearly the full speed. That is the
//      "sticks tight but does not grab" feel.
//   3. The FOV test uses the silent rotation when one is active, so a silent aim already in
//      progress is not re-judged against the visible crosshair and cancelled.
//
// What is added on top is bow-specific: the launch pitch is solved against the real trajectory
// rather than aiming straight at the target, so arrows land on it instead of under it.
#pragma once
#include "slinky/api.h"

extern "C" void bow_aim_assist_gui(void *gui, void *window);
extern "C" void bow_aim_assist_register(void *module);
