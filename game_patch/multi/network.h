#pragma once

#include <string>
#include <vector>
#include <common/rfproto.h>
#include <common/version/version.h>
#include "server_internal.h"
#include "../rf/multi.h"
#include "../os/io_cursor.h"

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

// CTF flag packet payloads used by process_* hooks.
struct RFCtfFlagDroppedPacket
{
    uint8_t is_red;
    uint8_t red_score;
    uint8_t blue_score;
    float pos_x;
    float pos_y;
    float pos_z;

    rf::Vector3 get_flag_position() const
    {
        return {pos_x, pos_y, pos_z};
    }
};

struct RFCtfFlagSingleTeamPacket
{
    uint8_t is_red;
};

struct RFCtfFlagPickedUpPacket
{
    uint8_t picker_player_id;
    uint8_t red_score;
    uint8_t blue_score;
};
#pragma pack(pop)

static_assert(sizeof(RFCtfFlagDroppedPacket) == 15);
static_assert(sizeof(RFCtfFlagSingleTeamPacket) == 1);
static_assert(sizeof(RFCtfFlagPickedUpPacket) == 3);

// Appended to game_info packets (legacy, sent to gi_req_ext_ver 1-2 clients)
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

// In-memory representation of the AF game_info v2 extension.
// On-wire layout: [sig][ver*4][flags][filename\0][bot_counts*4]
// The wire format is built by serialize_to_wire(); it does not match the in-memory layout.
struct af_game_info_ext_v2
{
    // on-wire size of fields before the filename (sig + ver*4 + flags)
    static constexpr size_t wire_pre_fname_size = 12;
    // on-wire size of fields after the filename (bot_counts*4)
    static constexpr size_t wire_post_fname_size = 4;

    uint32_t af_signature = ALPINE_FACTION_SIGNATURE;
    uint8_t version_major = VERSION_MAJOR;
    uint8_t version_minor = VERSION_MINOR;
    uint8_t version_patch = VERSION_PATCH;
    uint8_t version_type = VERSION_TYPE;
    uint32_t af_flags = 0;
    std::string level_filename;
    uint8_t num_bots = 0;
    uint8_t num_human_players = 0;
    uint8_t num_browsers = 0;
    uint8_t num_total_clients = 0; // bots + humans + browsers

    void set_flags(const AFGameInfoFlags& flags)
    {
        af_flags = flags.game_info_flags_to_uint32();
    }

    // Build the wire representation: [sig][ver*4][flags][filename\0][bot_counts*4]
    std::vector<uint8_t> serialize_to_wire() const;
};

#pragma pack(push, 1)
struct AlpineFactionJoinAcceptPacketExt
{
    uint32_t af_signature = ALPINE_FACTION_SIGNATURE;
    uint8_t version_major = VERSION_MAJOR;
    uint8_t version_minor = VERSION_MINOR;
    uint8_t version_patch = VERSION_PATCH;
    uint8_t version_type = VERSION_TYPE;

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
        geo_chunk_physics   = 1u << 12,
        allow_footsteps     = 1u << 13,
        allow_outlines      = 1u << 14,
        allow_outlines_xray = 1u << 15,
        clear_stale_movement_input = 1u << 16,
    } flags = Flags::none;

    float max_fov = 0.0f;
    int32_t semi_auto_cooldown = 0;

};
#pragma pack(pop)
static_assert(sizeof(AlpineFactionJoinAcceptPacketExt) == 20, "unexpected AlpineFactionJoinAcceptPacketExt size");
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

struct AfGiReqSeen
{
    uint8_t ver = 0; // game_info_req version: 1 is current, pre-Alpine v1.2 never sends a version
    int64_t last_seen_ms = 0;
};

enum class RconCommandCheckResult
{
    Allowed,
    NotHolder,
    ProfileDenied
};

struct RconAccessEntry
{
    size_t profile_index = 0;
};

struct RconPasswordLookup
{
    std::optional<size_t> profile_index;
};

struct AlpineFactionJoinReqPacketExt // used for stashed data during join process
{
    enum class Flags : uint32_t
    {
        none = 0,
        client_bot = 1u << 0,
        client_d3d11 = 1u << 1,
    };

    uint32_t af_signature = 0u;
    uint8_t version_major = 0u;
    uint8_t version_minor = 0u;
    uint8_t version_patch = 0u;
    uint8_t version_type = 0u;
    uint32_t max_rfl_version = 0u;
    Flags flags = Flags::none;
    std::optional<uint32_t> bot_shared_secret{};
};
template<>
struct EnableEnumBitwiseOperators<AlpineFactionJoinReqPacketExt::Flags> : std::true_type {};

// Per-server extra data parsed from the AF game_info v2 extension
struct AFGameInfoExtra
{
    uint8_t version_major = 0;
    uint8_t version_minor = 0;
    uint8_t version_patch = 0;
    uint8_t version_type = 0;
    uint32_t af_flags = 0;
    std::string level_filename;
    uint8_t num_bots = 0;
    uint8_t num_human_players = 0;
    uint8_t num_browsers = 0;
    uint8_t num_total_clients = 0;
};

// Look up AF extra data for a server by address. Returns nullptr if not found.
const AFGameInfoExtra* get_server_browser_extra(const rf::NetAddr& addr);
void clear_server_browser_extra();

bool packet_check_whitelist(int packet_type);
void handle_vote_or_ready_up_msg(std::string_view msg);
void handle_sound_msg(std::string_view name);
void send_queues_rel_clear_packets(int socket_id);
void send_queues_rel_add_packet(int socket_id, const uint8_t* data, size_t len);
void clear_rcon_profile_sessions();
void multi_disconnect_from_server();

template <io_cursor_detail::Write Storage>
class PacketBuilder {
public:
    explicit PacketBuilder(const Storage storage)
        noexcept(std::is_nothrow_move_constructible_v<Storage>)
        : storage{std::move(storage)}
        , cursor{this->storage} {
        this->cursor.write(DUMMY_HEADER);
    }

    template <typename... Args>
    requires std::constructible_from<Storage, Args...>
    explicit PacketBuilder(Args&&... args)
        noexcept(std::is_nothrow_constructible_v<Storage, Args...>)
        : storage{std::forward<Args>(args)...}
        , cursor{this->storage} {
        this->cursor.write(DUMMY_HEADER);
    }

    template <io_cursor_detail::PodLike T>
    bool write(this PacketBuilder& self, const T& value) noexcept {
        return self.cursor.write(value);
    }

    bool write_zstring(
        this PacketBuilder& self,
        const std::string_view text
    ) noexcept {
        return self.cursor.write_zstring(text);
    }

    template <typename E>
    requires std::is_enum_v<E>
        && (sizeof(std::underlying_type_t<E>) == 1)
    void finalize(this PacketBuilder& self, const E type) noexcept {
        const RF_GamePacketHeader header{
            .type = std::to_underlying(type),
            .size = static_cast<uint16_t>(self.payload_len())
        };
        self.storage.write(
            0,
            std::span{
                reinterpret_cast<const char*>(&header),
                sizeof(RF_GamePacketHeader)
            }
        );
    }

    [[nodiscard]]
    size_t capacity(this const PacketBuilder& self) noexcept {
        return self.cursor.len();
    }

    [[nodiscard]]
    size_t payload_len(this const PacketBuilder& self) noexcept {
        return self.cursor.position() - sizeof(RF_GamePacketHeader);
    }

    [[nodiscard]]
    size_t len(this const PacketBuilder& self) noexcept {
        return self.cursor.position();
    }

    [[nodiscard]]
    bool is_poisoned(this const PacketBuilder& self) noexcept {
        return self.cursor.is_poisoned();
    }

    [[nodiscard]]
    const char* ptr(this const PacketBuilder& self) noexcept {
        return self.storage.ptr();
    }

private:
    static constexpr RF_GamePacketHeader DUMMY_HEADER{};

    Storage storage;
    IoCursor<Storage> cursor;
};

template <size_t N>
PacketBuilder(char (&)[N]) -> PacketBuilder<io_cursor_detail::FixedSpanStorage>;
PacketBuilder(std::span<char>) -> PacketBuilder<io_cursor_detail::FixedSpanStorage>;
PacketBuilder(void*, size_t) -> PacketBuilder<io_cursor_detail::FixedSpanStorage>;
PacketBuilder(std::vector<char>&) -> PacketBuilder<io_cursor_detail::VectorGrowStorage>;
