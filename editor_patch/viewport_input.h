#pragma once

// The one owner of the viewport mouse button handlers in the view message map. Each press goes to Terrain Tools
// while its panel is open, then to the transform gizmo, then to stock RED; a press a feature keeps never reaches
// stock, and neither does its release. Also keeps RED's idle hover focus off the views while a feature needs them.

void ApplyViewportInputPatches();
