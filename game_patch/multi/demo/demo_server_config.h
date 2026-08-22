#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "../server_config_snapshot.h"

// Self-describing versioned block that carries a ServerConfigSnapshot as the payload of a
// demo server_info record (DemoRecordType::server_info). All integers little-endian:
//
//   u8  block_version = 1
//   u8  rf_flags
//   u32 gi_flags
//   u8  match_state
//   u16 mutator_count
//     repeat: str name; u16 setting_count; repeat: str key; value
//   u16 gametype_setting_count
//     repeat: str key; value
//
// where str = u16 length + bytes, and value = u8 tag + payload:
//   tag 0 bool (u8 0/1), 1 int64 (8B), 2 double (8B), 3 string (str). Any other tag fails.
//
// The v1 field order is frozen: a future block_version>1 may only APPEND fields, and the
// decoder ignores trailing bytes so it always parses the v1 prefix.

// Encodes the current snapshot as a block_version=1 block. Returns an empty vector (writer
// then skips the record) if the block would exceed the u16 record payload cap.
std::vector<uint8_t> encode_server_config_block(const server_config::ServerConfigSnapshot& snapshot);

// Decodes a block. `data`/`len` are treated as untrusted: every read is bounds-checked,
// counts and string lengths are capped, and any short read or unknown value tag yields
// std::nullopt rather than an OOB read or over-allocation.
std::optional<server_config::ServerConfigSnapshot> decode_server_config_block(const uint8_t* data, size_t len);
