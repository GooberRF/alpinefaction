#pragma once

#include "vehicle.h"

// All hull crash/landing tuning. Damage = spawn max life * frac * min((impact_dv - thr) / (ref_dv - thr),
// vehicle_crash_max_over_ref)^2, impact_dv being the pre-contact speed into the surface (u/s).
struct VehicleCrashTuning
{
    float wall_threshold;   // a wall/mover hit at or below this impact_dv is free
    float ground_threshold; // the same for a landing; every wheel contact is a landing
    float wall_ref_dv;      // measured flat-out cruise speed, i.e. a full-speed head-on wall hit
    float wall_frac;        // fraction of max life a wall hit at wall_ref_dv takes
    float land_ref_dv;      // cars: a 10 m drop's fall speed (capped at 1.5 x top); fighter/sub: top speed
    float land_frac;        // fraction of max life a landing at land_ref_dv takes
};

// Cars cruise past car_max_speed (the engine only cuts above it), so the wall ref is the measured cruise.
inline constexpr VehicleCrashTuning vehicle_crash_tuning[VDC_COUNT] = {
    // wall thr, ground thr, wall ref, wall frac, land ref, land frac
    {8.0f, 9.0f, 18.0f, 0.275f, 15.34f, 0.11f},   // VDC_JEEP: cruises at ~18
    {5.5f, 9.0f, 12.3f, 0.275f, 15.75f, 0.225f},  // VDC_APC: cruises at ~12.3 (car_max_speed 10.5)
    {4.5f, 9.0f, 9.6f, 0.275f, 12.75f, 0.225f},   // VDC_DRILLER: cruises at ~9.6 (8.5); drops over ~6.1 m cap
    {10.0f, 10.0f, 25.0f, 0.275f, 25.0f, 0.275f}, // VDC_FIGHTER
    {6.0f, 6.0f, 15.0f, 0.275f, 15.0f, 0.275f},   // VDC_SUB
    {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},         // VDC_TURRET: no body, never measured
};

// Cap on (impact_dv - thr) / (ref_dv - thr): no hit costs more than frac x 1.5625 of max life.
inline constexpr float vehicle_crash_max_over_ref = 1.25f;
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
// A ground window opened within this much contact time of airtime is a landing...
inline constexpr float vehicle_crash_land_air_grace_s = 0.1f;
// ...billed at its full pre-contact speed once the net normal velocity change reaches this fraction of it...
inline constexpr float vehicle_crash_land_stop_frac = 0.5f;
// ...waited for up to this long in total; past it, the summed change as for any other hit.
inline constexpr float vehicle_crash_land_window_s = 0.25f;
// u/s^2: a frame whose impact_dv reaches this times its simulated step opens a window (1.5 u/s at 60 fps).
inline constexpr float vehicle_crash_open_accel = 90.0f;
// Mean contact normal Y from which a chassis impact is a landing rather than a wall.
inline constexpr float vehicle_crash_ground_ny = 0.7f;
// Drilling driller: a chassis contact whose normal . hull forward is below this is the drill face, free.
inline constexpr float vehicle_crash_drill_face_dot = -0.5f;
