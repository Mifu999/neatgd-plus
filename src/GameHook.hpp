#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

class PlayLayer;

namespace neatgd {

constexpr size_t SCAN_SAMPLES = 8;
constexpr float SCAN_OFFSETS[SCAN_SAMPLES] = {
    0.f, 40.f, 80.f, 120.f, 160.f, 200.f, 250.f, 300.f};

// 12 player-state inputs
//  + 4 * SCAN_SAMPLES terrain columns (floor / ceiling / hazard up / hazard down)
//  + 20 interaction inputs (nearest orb, pad and portal, plus speed / size /
//    dash / input-rhythm state).
//
// The interaction block is what lets a network learn "click ON this orb"
// instead of only "click at this exact moment": before it existed the object
// cache held solids and hazards only, so orbs, pads and portals were invisible
// to every genome.
constexpr size_t STATE_INPUTS = 12;
constexpr size_t TERRAIN_INPUTS = 4 * SCAN_SAMPLES;
constexpr size_t INTERACT_INPUTS = 20;
constexpr size_t INPUT_COUNT = STATE_INPUTS + TERRAIN_INPUTS + INTERACT_INPUTS;

// Input count of builds before interaction sensing existed. Sessions saved by
// those builds are migrated instead of rejected (see Population::upgradeInputs).
constexpr size_t LEGACY_INPUT_COUNT_V1 = STATE_INPUTS + TERRAIN_INPUTS;

constexpr float NORM_Y = 300.f;
constexpr float NORM_VEL = 16.f;
constexpr float NORM_DY = 120.f;
constexpr float NORM_CLEAR = 240.f;
// Orbs and pads only matter close up - a ring 300px away is not actionable.
constexpr float NORM_ORB = 200.f;
constexpr float NORM_PORTAL = 320.f;
constexpr float NORM_HOLD = 30.f;

enum class ObjKind : uint8_t {
    Solid = 0,
    Hazard = 1,
    Orb = 2,      // rings: you press WHILE touching them
    Pad = 3,      // pads: trigger on contact, no input needed
    Portal = 4,   // gamemode / gravity / size / dual changes
};

struct LevelObject {
    float x = 0.f;
    float y = 0.f;
    float w = 30.f;
    float h = 30.f;
    bool hazard = false;
    bool slope = false;
    int objectId = 0;

    ObjKind kind = ObjKind::Solid;
    // sub-type flags, meaningful per kind
    bool flagJump = false;     // orb/pad: plain jump (yellow, pink, red)
    bool flagGravity = false;  // orb/pad/portal: flips or changes gravity
    bool flagDash = false;     // orb: dash ring (held, not tapped)
    bool flagStrong = false;   // pad: red / pink (stronger launch)
    bool flagMode = false;     // portal: changes gamemode
    bool flagSize = false;     // portal: mini / regular
    bool flagDual = false;     // portal: dual / solo
};

std::vector<LevelObject> extractLevelObjects(PlayLayer* layer);

}
