#pragma once

#include "vehicle.h"

// All hull crash/landing tuning. Damage = spawn max life * frac * ((impact_dv - thr) / (ref_dv - thr))^2,
// impact_dv being the velocity change along the contact normal (u/s).
struct VehicleCrashTuning
{
    float wall_threshold;   // a wall/mover hit at or below this impact_dv is free
    float ground_threshold; // the same for a landing; every wheel contact is a landing
    float wall_ref_dv;      // the class top speed, i.e. a full-speed head-on wall hit
    float wall_frac;        // fraction of max life a wall hit at wall_ref_dv takes
    float land_ref_dv;      // cars: a 10 m drop's fall speed (capped at 1.5 x top); fighter/sub: top speed
    float land_frac;        // fraction of max life a landing at land_ref_dv takes
};

inline constexpr VehicleCrashTuning vehicle_crash_tuning[VDC_COUNT] = {
    // wall thr, ground thr, wall ref, wall frac, land ref, land frac
    {8.0f, 9.0f, 18.0f, 0.275f, 15.34f, 0.11f},   // VDC_JEEP
    {5.5f, 9.0f, 10.5f, 0.275f, 15.75f, 0.225f},  // VDC_APC
    {4.5f, 9.0f, 8.5f, 0.275f, 12.75f, 0.225f},   // VDC_DRILLER: every drop over ~6.1 m reaches the cap
    {10.0f, 10.0f, 25.0f, 0.275f, 25.0f, 0.275f}, // VDC_FIGHTER
    {6.0f, 6.0f, 15.0f, 0.275f, 15.0f, 0.275f},   // VDC_SUB
    {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},         // VDC_TURRET: no body, never measured
};

// A hit under this fraction of max life is a scrape and deals nothing.
inline constexpr float vehicle_crash_min_life_frac = 0.01f;
// Server: at most one crash per hull per this window.
inline constexpr int vehicle_crash_server_cooldown_ms = 400;
// Client: at most one report per hull per this window; longer than the server's so a late report lands.
inline constexpr int vehicle_crash_client_cooldown_ms = 500;
// Server: at most one crash report per sender per this window.
inline constexpr int vehicle_crash_report_cooldown_ms = 200;
// Simulated time an impact's velocity change is summed over (suspension spreads a landing).
inline constexpr float vehicle_crash_window_s = 0.1f;
// One frame's impact_dv that opens a window; gravity and drive stay below it.
inline constexpr float vehicle_crash_open_dv = 1.5f;
// Mean contact normal Y from which a chassis impact is a landing rather than a wall.
inline constexpr float vehicle_crash_ground_ny = 0.7f;
// Drilling driller: a chassis contact whose normal . hull forward is below this is the drill face, free.
inline constexpr float vehicle_crash_drill_face_dot = -0.5f;
