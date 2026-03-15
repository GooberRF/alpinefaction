#pragma once

// Shared Alpine object infrastructure — hooks that dispatch to all Alpine object types.
// Type-specific logic lives in mesh.cpp / note.cpp; this file wires them together.
void ApplyAlpineObjectPatches();
