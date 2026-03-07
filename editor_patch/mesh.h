#pragma once

#include <cstddef>
#include "mfc_types.h"
#include "level.h"

void ApplyMeshPatches();

// Mesh serialization (called from level.cpp injection points)
void mesh_serialize_chunk(CDedLevel& level, rf::File& file);
void mesh_deserialize_chunk(CDedLevel& level, rf::File& file, std::size_t chunk_len);

// Mesh property dialog
void ShowMeshPropertiesDialog(DedMesh* mesh);

// Mesh object lifecycle
void PlaceNewMeshObject();
DedMesh* CloneMeshObject(DedMesh* source, bool add_to_level = true);
void DeleteMeshObject(DedMesh* mesh);
