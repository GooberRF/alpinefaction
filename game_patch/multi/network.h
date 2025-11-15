#pragma once

#include <common/version/version.h>
#include "server_internal.h"
#include "../rf/multi.h"

constexpr uint32_t ALPINE_FACTION_SIGNATURE = 0x4E4C5246;
constexpr uint32_t DASH_FACTION_SIGNATURE = 0xDA58FAC7;
static constexpr uint32_t AF_FOOTER_MAGIC = 0x52544641u; // AFTR (LE)

#pragma pack(push, 1)
struct SBJoinReq_v5_0 // rfsb 5.0.1
{
    uint8_t sig;  // 0xEB
    uint8_t c0;   // 0x05
    uint8_t c1;   // 0x00
    uint8_t c2;   // 0x01
    uint8_t c3;   // 0x03
    uint8_t c4;   // 0x8B
    uint8_t c5;   // 0x05
    uint8_t c6;   // 0x00
};

struct SBJoinReq_v5_1 // rfsb 5.1.4
{
    uint8_t sig;  // 0xEB
    uint8_t c0;   // changes
    uint8_t c1;   // 0x05
    uint8_t c2;   // 0x00
    uint8_t c3;   // 0x01
    uint8_t c4;   // 0x03
    uint8_t c5;   // 0x8B
    uint8_t c6;   // 0x05
    uint8_t c7;   // 0x01
    uint8_t c8;   // 0xBD
};

struct DFJoinReq_v1 // df
{
    uint32_t signature;
    uint8_t version_major;
    uint8_t version_minor;
    uint8_t padding1;
    uint8_t padding2;
};

struct AFJoinReq_v1 // af v1.0
{
    uint32_t signature;
    uint8_t version_major;
    uint8_t version_minor;
    uint8_t padding1;
    uint8_t padding2;
    uint32_t flags;
};

struct AFJoinReq_v2 // af v1.1+
{
    uint32_t signature;
    uint8_t version_major;
    uint8_t version_minor;
    uint8_t version_patch;
    uint8_t version_type;
    uint32_t max_rfl_version;
    uint32_t flags;
};

struct AFGameInfoReq // af v1.2+
{
    uint32_t signature;
    uint8_t gi_req_ext_ver;
};

struct AFFooter
{
    uint16_t total_len; // bytes from start of AF block up to start of this footer
    uint32_t magic;
};
#pragma pack(pop)

// Appended to game_info packets
struct af_sign_packet_ext
{
    uint32_t af_signature = ALPINE_FACTION_SIGNATURE;
    uint8_t version_major = VERSION_MAJOR;
    uint8_t version_minor = VERSION_MINOR;
    uint8_t version_patch = VERSION_PATCH;
    uint8_t version_type = VERSION_TYPE;
    uint32_t af_flags = 0;

    void set_flags(const AFGameInfoFlags& flags)
    {
        af_flags = flags.game_info_flags_to_uint32();
    }
};

struct AlpineFactionJoinAcceptPacketExt
{
    uint32_t af_signature = ALPINE_FACTION_SIGNATURE;
    uint8_t version_major = VERSION_MAJOR;
    uint8_t version_minor = VERSION_MINOR;

    enum class Flags : uint32_t {
        none                = 0,
        saving_enabled      = 1u << 0,
        max_fov             = 1u << 1,
        allow_fb_mesh       = 1u << 2,
        allow_lmap          = 1u << 3,
        allow_no_ss         = 1u << 4,
        no_player_collide   = 1u << 5,
        allow_no_mf         = 1u << 6,
        click_limit         = 1u << 7,
        unlimited_fps       = 1u << 8,
        gaussian_spread     = 1u << 9,
        location_pinging    = 1u << 10,
        delayed_spawns      = 1u << 11,
    } flags = Flags::none;

    float max_fov;
    int semi_auto_cooldown;

};
template<>
struct EnableEnumBitwiseOperators<AlpineFactionJoinAcceptPacketExt::Flags> : std::true_type {};

struct StashedPacket
{
    rf::NetAddr addr;   // source
    const uint8_t* pkt;
    size_t len;         // engine trimmed
    size_t rx_len;      // full udp datagram
    uint8_t type;
};

struct AlpineFactionJoinReqPacketExt // used for stashed data during join process
{
    enum class Flags : uint32_t
    {
        none = 0,
    };

    uint32_t af_signature = 0u;
    uint8_t version_major = 0u;
    uint8_t version_minor = 0u;
    uint8_t version_patch = 0u;
    uint8_t version_type = 0u;
    uint32_t max_rfl_version = 0u;
    Flags flags = Flags::none;
};
template<>
struct EnableEnumBitwiseOperators<AlpineFactionJoinReqPacketExt::Flags> : std::true_type {};

bool packet_check_whitelist(int packet_type);
