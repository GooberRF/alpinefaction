#pragma once

#include <string>
#include "../rf/geometry.h"
#include "../rf/math/matrix.h"
#include "../rf/math/vector.h"

void scene_capture_apply_patch();

bool scene_capture_render(const rf::Vector3& pos, const rf::Matrix3& orient, float fov,
                          int target_bm, int w, int h, rf::GRoom* eye_room);
bool projector_activate(int event_uid, int camera_handle, const std::string& atx_handle,
                        float fov, float interval_s, int w, int h);
void projector_deactivate(int event_uid);
void projector_clear_all();
