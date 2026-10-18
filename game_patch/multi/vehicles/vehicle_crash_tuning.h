#pragma once

#include "vehicle.h"

// All hull crash/landing tuning. Damage = spawn max life * max(frac * min((impact_dv - thr) / (ref_dv - thr),
// vehicle_crash_max_over_ref)^2, min_frac), impact_dv being the pre-contact speed into the surface (u/s).
struct VehicleCrashTuning
{
    float wall_threshold;   // a wall/mover hit at or below this impact_dv is free
    float ground_threshold; // the same for a landing; every wheel contact is a landing
    float wall_ref_dv;      // measured flat-out cruise speed, i.e. a full-speed head-on wall hit
    float wall_frac;        // fraction of max life a wall hit at wall_ref_dv takes
    float land_ref_dv;      // cars: a 10 m drop's fall speed at the class air_gravity; fighter cruise; sub top
    float land_frac;        // fraction of max life a landing at land_ref_dv takes
    float min_frac;         // the least any hit past its threshold takes
};

// Cars cruise past car_max_speed (the engine only cuts above it), so the wall ref is the measured cruise.
// The fighter's thrust and drag ramp settle it at ~21, short of its max_speed of 25. Its impact effects start at
// its thresholds (vehicle_fx.cpp), so a slam that shows costs hull; a descend tap's landing stays free.
inline constexpr VehicleCrashTuning vehicle_crash_tuning[VDC_COUNT] = {
    // wall thr, ground thr, wall ref, wall frac, land ref, land frac, min frac
    {8.0f, 9.0f, 18.0f, 0.275f, 15.34f, 0.11f, 0.0f},   // VDC_JEEP: cruises at ~18
    {5.5f, 9.0f, 12.3f, 0.275f, 17.15f, 0.225f, 0.0f},  // VDC_APC: cruises at ~12.3 (car_max_speed 10.5)
    {4.5f, 9.0f, 9.6f, 0.275f, 17.15f, 0.225f, 0.0f},   // VDC_DRILLER: cruises at ~9.6 (8.5)
    {5.0f, 10.0f, 21.0f, 0.6f, 21.0f, 0.6f, 0.02f},     // VDC_FIGHTER
    {6.0f, 6.0f, 15.0f, 0.275f, 15.0f, 0.275f, 0.0f},   // VDC_SUB
    {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},         // VDC_TURRET: no body, never measured
};

// Hull on hull, per VICTIM class. Damage = spawn max life * frac * ((closing - vehicle_ram_min_speed) /
// (ref_closing - vehicle_ram_min_speed))^2 * the other hull's mass over its own, clamped to
// [vehicle_ram_mass_ratio_min, mass_ratio_max]. Every row but the fighter's comes to about 0.26 * excess^2 at
// the class's tuned life (a turret keeps its tbl $Life of 400).
struct VehicleRamTuning
{
    float ref_closing;    // u/s
    float frac;           // fraction of max life an equal-mass ram at ref_closing takes
    float mass_ratio_max;
};

inline constexpr VehicleRamTuning vehicle_ram_tuning[VDC_COUNT] = {
    {36.0f, 0.2f, 4.0f},    // VDC_JEEP: two jeeps head-on at cruise
    {24.5f, 0.0165f, 4.0f}, // VDC_APC
    {19.0f, 0.0068f, 4.0f}, // VDC_DRILLER
    // Two fighters head-on at 18 each (cruise ~21) both go down; flying into a heavy hull costs about what the
    // same wall hit does.
    {36.0f, 1.0f, 2.0f},    // VDC_FIGHTER
    {30.0f, 0.18f, 4.0f},   // VDC_SUB
    {36.0f, 0.625f, 4.0f},  // VDC_TURRET: rammed only
};

// Sliding along a surface while touching it. Damage per second = spawn max life * frac_per_s *
// (slide speed - min_speed) / (ref_speed - min_speed) along a wall or ceiling; along the ground the free speed
// is ground_min_speed instead, over the same span from there to ref_speed. A zero frac_per_s takes none. A
// fighter's scrape effects start at the same speeds.
struct VehicleScrapeTuning
{
    float min_speed;        // u/s along a wall or ceiling below which a touch is free
    float ground_min_speed; // the same along the ground: a taxi or a slow touchdown is free
    float ref_speed;
    float frac_per_s;       // fraction of max life per second of sliding at ref_speed
};

inline constexpr VehicleScrapeTuning vehicle_scrape_tuning[VDC_COUNT] = {
    {0.0f, 0.0f, 0.0f, 0.0f},   // VDC_JEEP
    {0.0f, 0.0f, 0.0f, 0.0f},   // VDC_APC
    {0.0f, 0.0f, 0.0f, 0.0f},   // VDC_DRILLER
    {5.0f, 8.0f, 21.0f, 0.1f},  // VDC_FIGHTER: ref at cruise
    {0.0f, 0.0f, 0.0f, 0.0f},   // VDC_SUB
    {0.0f, 0.0f, 0.0f, 0.0f},   // VDC_TURRET
};

// Scrape damage is banked and dealt, or reported, once per this window; a bank still unsent a window past it
// is the last drive's and is dropped.
inline constexpr int vehicle_scrape_flush_ms = 250;
// Server: the refilling budget of sliding time a hull's scrape reports may bill, at most this much.
inline constexpr int vehicle_scrape_report_max_window_ms = 500;

// Cap on (impact_dv - thr) / (ref_dv - thr): no hit costs more than frac x 1.5625 of max life.
inline constexpr float vehicle_crash_max_over_ref = 1.25f;
// A hit under this fraction of max life is a graze and deals nothing; a row's min_frac lifts every hit past its
// threshold above it.
inline constexpr float vehicle_crash_min_life_frac = 0.01f;
// A hull's crashes inside this window bill only what each adds over the largest; the client's reports alike.
inline constexpr int vehicle_crash_bill_window_ms = 400;
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
