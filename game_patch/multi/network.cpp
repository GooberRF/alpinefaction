#include <array>
#include <algorithm>
#include <cstddef>
#include <cassert>
#include <cstring>
#include <format>
#include <functional>
#include <thread>
#include <utility>
#include <winsock2.h>
#include <iphlpapi.h>
#include <ws2ipdef.h>
#include <natupnp.h>
#include <common/config/BuildConfig.h>
#include <common/rfproto.h>
#include <common/version/version.h>
#include <common/utils/enum-bitwise-operators.h>
#include <common/utils/list-utils.h>
#include <common/ComPtr.h>
#include <xlog/xlog.h>
#include <patch_common/CallHook.h>
#include <patch_common/FunHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/AsmWriter.h>
#include <patch_common/ShortTypes.h>
#include "network.h"
#include "multi.h"
#include "alpine_packets.h"
#include "server.h"
#include "server_internal.h"
#include "../main/main.h"
#include "../hud/hud.h"
#include "../rf/multi.h"
#include "../rf/misc.h"
#include "../rf/player/player.h"
#include "../rf/weapon.h"
#include "../rf/entity.h"
#include "../rf/os/console.h"
#include "../rf/os/os.h"
#include "../rf/os/timer.h"
#include "../rf/geometry.h"
#include "../rf/level.h"
#include "../misc/misc.h"
#include "../misc/player.h"
#include "../misc/alpine_settings.h"
#include "../object/object.h"
#include "../os/console.h"
#include "../purefaction/pf.h"
#include "../sound/sound.h"
#include "../misc/tlv.h"

// NET_IFINDEX_UNSPECIFIED is not defined in MinGW headers
#ifndef NET_IFINDEX_UNSPECIFIED
#define NET_IFINDEX_UNSPECIFIED 0
#endif

int g_update_rate = 30; // client netfps

ClientVersion g_joining_client_version = ClientVersion::unknown;
AlpineFactionJoinReqPacketExt g_joining_player_info{};
StashedPacket g_join_request_stashed;
static const uint8_t* g_rx_base = nullptr;
static size_t g_rx_len = 0;

std::optional<int> g_desired_multiplayer_character; // caches local mp character when forced by server

using MultiIoPacketHandler = void(char* data, const rf::NetAddr& addr);

class BufferOverflowPatch
{
private:
    std::unique_ptr<BaseCodeInjection> m_movsb_patch;
    uintptr_t m_shr_ecx_2_addr;
    uintptr_t m_and_ecx_3_addr;
    int32_t m_buffer_size;
    int32_t m_ret_addr;

public:
    BufferOverflowPatch(uintptr_t shr_ecx_2_addr, uintptr_t and_ecx_3_addr, int32_t buffer_size) :
        m_movsb_patch(new CodeInjection{and_ecx_3_addr, [this](auto& regs) { movsb_handler(regs); }}),
        m_shr_ecx_2_addr(shr_ecx_2_addr), m_and_ecx_3_addr(and_ecx_3_addr), m_buffer_size(buffer_size)
    {}

    void install()
    {
        const std::byte* shr_ecx_2_ptr = reinterpret_cast<std::byte*>(m_shr_ecx_2_addr);
        const std::byte* and_ecx_3_ptr = reinterpret_cast<std::byte*>(m_and_ecx_3_addr);
        assert(std::memcmp(shr_ecx_2_ptr, "\xC1\xE9\x02", 3) == 0); // shr ecx, 2
        assert(std::memcmp(and_ecx_3_ptr, "\x83\xE1\x03", 3) == 0); // and ecx, 3

        using namespace asm_regs;
        if (std::memcmp(shr_ecx_2_ptr + 3, "\xF3\xA5", 2) == 0) { // rep movsd
            m_movsb_patch->set_addr(m_shr_ecx_2_addr);
            m_ret_addr = m_shr_ecx_2_addr + 5;
            AsmWriter(m_and_ecx_3_addr, m_and_ecx_3_addr + 3).xor_(ecx, ecx);
        }
        else if (std::memcmp(and_ecx_3_ptr + 3, "\xF3\xA4", 2) == 0) { // rep movsb
            AsmWriter(m_shr_ecx_2_addr, m_shr_ecx_2_addr + 3).xor_(ecx, ecx);
            m_movsb_patch->set_addr(m_and_ecx_3_addr);
            m_ret_addr = m_and_ecx_3_addr + 5;
        }
        else {
            assert(false);
        }

        m_movsb_patch->install();
    }

private:
    void movsb_handler(BaseCodeInjection::Regs& regs) const
    {
        char* dst_ptr = regs.edi;
        char* src_ptr = regs.esi;
        int num_bytes = regs.ecx;
        num_bytes = std::min(num_bytes, m_buffer_size - 1);
        std::memcpy(dst_ptr, src_ptr, num_bytes);
        dst_ptr[num_bytes] = '\0';
        regs.eip = m_ret_addr;
    }
};

// Note: player name is limited to 32 because some functions assume it is short (see 0x0046EBA5 for example)
// Note: server browser internal functions use strings safely (see 0x0044DDCA for example)
// Note: level filename was limited to 64 because of VPP format limits
std::array g_buffer_overflow_patches{
    BufferOverflowPatch{0x0047B2D3, 0x0047B2DE, 256}, // process_game_info_packet (server name)
    BufferOverflowPatch{0x0047B334, 0x0047B33D, 256}, // process_game_info_packet (level name)
    BufferOverflowPatch{0x0047B38E, 0x0047B397, 256}, // process_game_info_packet (mod name)
    BufferOverflowPatch{0x0047ACF6, 0x0047AD03, 32},  // process_join_req_packet (player name)
    BufferOverflowPatch{0x0047AD4E, 0x0047AD55, 256}, // process_join_req_packet (password)
    BufferOverflowPatch{0x0047A8AE, 0x0047A8B5, 64},  // process_join_accept_packet (level filename)
    BufferOverflowPatch{0x0047A5F4, 0x0047A5FF, 32},  // process_new_player_packet (player name)
    BufferOverflowPatch{0x00481EE6, 0x00481EEF, 32},  // process_players_packet (player name)
    BufferOverflowPatch{0x00481BEC, 0x00481BF8, 64},  // process_state_info_req_packet (level filename)
    BufferOverflowPatch{0x004448B0, 0x004448B7, 256}, // process_chat_line_packet (message)
    BufferOverflowPatch{0x0046EB24, 0x0046EB2B, 32},  // process_name_change_packet (player name)
    BufferOverflowPatch{0x0047C1C3, 0x0047C1CA, 64},  // process_leave_limbo_packet (level filename)
    BufferOverflowPatch{0x0047EE6E, 0x0047EE77, 256}, // process_obj_kill_packet (item name)
    BufferOverflowPatch{0x0047EF9C, 0x0047EFA5, 256}, // process_obj_kill_packet (item name)
    BufferOverflowPatch{0x00475474, 0x0047547D, 256}, // process_entity_create_packet (entity name)
    BufferOverflowPatch{0x00479FAA, 0x00479FB3, 256}, // process_item_create_packet (item name)
    BufferOverflowPatch{0x0046C590, 0x0046C59B, 256}, // process_rcon_req_packet (password)
    BufferOverflowPatch{0x0046C751, 0x0046C75A, 512}, // process_rcon_packet (command)
};

// clang-format off
enum packet_type : uint8_t {
    game_info_request      = 0x00,
    game_info              = 0x01,
    join_request           = 0x02,
    join_accept            = 0x03,
    join_deny              = 0x04,
    new_player             = 0x05,
    players                = 0x06,
    left_game              = 0x07,
    end_game               = 0x08,
    state_info_request     = 0x09,
    state_info_done        = 0x0A,
    client_in_game         = 0x0B,
    chat_line              = 0x0C,
    name_change            = 0x0D,
    respawn_request        = 0x0E,
    trigger_activate       = 0x0F,
    use_key_pressed        = 0x10,
    pregame_boolean        = 0x11,
    pregame_glass          = 0x12,
    pregame_remote_charge  = 0x13,
    suicide                = 0x14,
    enter_limbo            = 0x15, // level end
    leave_limbo            = 0x16, // level change
    team_change            = 0x17,
    ping                   = 0x18,
    pong                   = 0x19,
    netgame_update         = 0x1A, // stats
    rate_change            = 0x1B,
    select_weapon_request  = 0x1C,
    clutter_udate          = 0x1D,
    clutter_kill           = 0x1E,
    ctf_flag_pick_up       = 0x1F,
    ctf_flag_capture       = 0x20,
    ctf_flag_update        = 0x21,
    ctf_flag_return        = 0x22,
    ctf_flag_drop          = 0x23,
    remote_charge_kill     = 0x24,
    item_update            = 0x25,
    obj_update             = 0x26,
    obj_kill               = 0x27,
    item_apply             = 0x28,
    boolean_               = 0x29,
    // Note: `mover_update` overlaps with `pf_player_stats`.
    // Has an empty handler in stock netcode.
    mover_update           = 0x2A,
    respawn                = 0x2B,
    entity_create          = 0x2C,
    item_create            = 0x2D,
    reload                 = 0x2E,
    reload_request         = 0x2F,
    weapon_fire            = 0x30,
    fall_damage            = 0x31,
    rcon_request           = 0x32,
    rcon                   = 0x33,
    sound                  = 0x34,
    team_score             = 0x35,
    glass_kill             = 0x36,
    pf_player_stats        = 0x2A,
    af_ping_location_req   = 0x50,
    af_ping_location       = 0x51,
    af_damage_notify       = 0x52,
    af_obj_update          = 0x53,
    af_client_req          = 0x55,
    af_just_spawned_info   = 0x56,
    af_koth_hill_state     = 0x57,
    af_koth_hill_captured  = 0x58,
    af_just_died_info      = 0x59,
    af_server_info         = 0x5A,
    af_spectate_start      = 0x5B,
    af_spectate_notify     = 0x5C,
    af_server_msg          = 0x5D
};

// client -> server
std::array g_server_side_packet_whitelist{
    game_info_request,
    join_request,
    left_game,
    state_info_request,
    client_in_game,
    chat_line,
    name_change,
    respawn_request,
    use_key_pressed,
    suicide,
    team_change,
    pong,
    rate_change,
    select_weapon_request,
    obj_update,
    reload_request,
    weapon_fire,
    fall_damage,
    rcon_request,
    rcon,
    af_ping_location_req,
    af_client_req,
    af_spectate_start
};

// server -> client
std::array g_client_side_packet_whitelist{
    game_info,
    join_accept,
    join_deny,
    new_player,
    players,
    left_game,
    end_game,
    state_info_done,
    chat_line,
    name_change,
    trigger_activate,
    pregame_boolean,
    pregame_glass,
    pregame_remote_charge,
    enter_limbo,
    leave_limbo,
    team_change,
    ping,
    netgame_update,
    rate_change,
    clutter_udate,
    clutter_kill,
    ctf_flag_pick_up,
    ctf_flag_capture,
    ctf_flag_update,
    ctf_flag_return,
    ctf_flag_drop,
    remote_charge_kill,
    item_update,
    obj_update,
    obj_kill,
    item_apply,
    boolean_,
    respawn,
    entity_create,
    item_create,
    reload,
    weapon_fire,
    sound,
    team_score,
    glass_kill,
    pf_player_stats,
    af_ping_location,
    af_damage_notify,
    af_obj_update,
    af_just_spawned_info,
    af_koth_hill_state,
    af_koth_hill_captured,
    af_just_died_info,
    af_server_info,
    af_spectate_notify,
    af_server_msg
};
// clang-format on

std::optional<AlpineFactionServerInfo> g_af_server_info;

bool packet_check_whitelist(int packet_type) {
    bool allowed = false;
    if (rf::is_server) {
        auto& whitelist = g_server_side_packet_whitelist;
        allowed = std::find(whitelist.begin(), whitelist.end(), packet_type) != whitelist.end();
    }
    else {
        auto& whitelist = g_client_side_packet_whitelist;
        allowed = std::find(whitelist.begin(), whitelist.end(), packet_type) != whitelist.end();
    }
    return allowed;
}

CodeInjection process_game_packet_whitelist_filter{
    0x0047918D,
    [](auto& regs) {
        int packet_type = regs.esi;
        if (!packet_check_whitelist(packet_type)) {
            xlog::warn("Ignoring packet 0x{:x}", packet_type);
            regs.eip = 0x00479194;
        }
        else {
            xlog::trace("Processing packet 0x{:x}", packet_type);
        }
    },
};

FunHook<MultiIoPacketHandler> process_game_info_packet_hook{
    0x0047B2A0,
    [](char* data, const rf::NetAddr& addr) {
        process_game_info_packet_hook.call_target(data, addr);

        // If this packet is from the server that we are connected to, use game_info for the netgame name
        // Useful for joining using protocol handler because when we join we do not have the server name available yet
        const char* server_name = data + 1;
        if (addr == rf::netgame.server_addr) {
            rf::netgame.name = server_name;
        }
    },
};

CodeInjection process_game_info_packet_game_type_bounds_patch{
    0x0047B30B,
    [](auto& regs) {
        // Valid game types are between 0 and 6
        // 3 = KOTH, 4 = DC, 5 = REV, 6 = RUN
        regs.ecx = std::clamp<int>(regs.ecx, 0, 6);
    },
};

FunHook<MultiIoPacketHandler> process_join_deny_packet_hook{
    0x0047A400,
    [](char* data, const rf::NetAddr& addr) {
        if (rf::multi_is_connecting_to_server(addr)) // client-side
            process_join_deny_packet_hook.call_target(data, addr);
    },
};

FunHook<MultiIoPacketHandler> process_new_player_packet_hook{
    0x0047A580,
    [](char* data, const rf::NetAddr& addr) {
        if (GetForegroundWindow() != rf::main_wnd && g_alpine_game_config.player_join_beep)
            Beep(750, 300);
        process_new_player_packet_hook.call_target(data, addr);
    },
};

static void verify_player_id_in_packet(char* player_id_ptr, const rf::NetAddr& addr, const char* packet_name)
{
    if (!rf::is_server) {
        // Only server-side checking makes sense because client receives all packets from the server address
        return;
    }
    rf::Player* src_player = rf::multi_find_player_by_addr(addr);
    if (!src_player) {
        // should not happen except for join/game_info packets (protected in rf::multi_io_process_packets)
        assert(false);
        return;
    }
    rf::ubyte received_player_id = static_cast<rf::ubyte>(*player_id_ptr);
    rf::ubyte real_player_id = src_player->net_data->player_id;
    if (received_player_id != real_player_id) {
        xlog::warn("Wrong player ID in {} packet from {} (expected {:02X} but got {:02X})",
            packet_name, src_player->name.c_str(), real_player_id, received_player_id);
        *player_id_ptr = real_player_id; // fix player ID
    }
}

FunHook<MultiIoPacketHandler> process_left_game_packet_hook{
    0x0047BBC0,
    [](char* data, const rf::NetAddr& addr) {
        // server-side and client-side
        verify_player_id_in_packet(&data[0], addr, "left_game");

        if (!rf::is_server && !rf::is_dedicated_server) {
            rf::Player* const player = rf::multi_find_player_by_id(data[0]);
            if (player) {
                g_local_player_spectators.erase(player);
                build_local_player_spectators_strings();
            }
        }

        process_left_game_packet_hook.call_target(data, addr);
    },
};

void handle_vote_or_ready_up_msg(const std::string_view msg) {
    constexpr std::string_view vote_start_prefix =
        "\n=============== VOTE STARTING ===============\n";

    if (string_starts_with_ignore_case(msg, vote_start_prefix)) {
        // Move past the prefix to start parsing the actual vote title
        const std::string_view title = msg.substr(vote_start_prefix.size());
        // Find the position of " vote started by"
        const size_t vote_started_by = title.find(" vote started by");
        if (vote_started_by != std::string::npos) {
            // Extract vote type by copying characters up to the found position
            const std::string vote_type{title.substr(0, vote_started_by)};
            // Pass extracted vote type to the handler
            draw_hud_vote_notification(vote_type.c_str());
        }
    }

    // possible messages that end a vote
    constexpr std::array<std::string_view, 4> vote_end_messages = {
        "\xA6 Vote failed",
        "\xA6 Vote passed",
        "\xA6 Vote canceled",
        "\xA6 Vote timed out"
    };

    // remove the vote notification if the vote has ended
    for (const std::string_view& end_msg : vote_end_messages) {
        if (string_starts_with_ignore_case(msg, end_msg)) {
            remove_hud_vote_notification();
            break;
        }
    }

    // possible messages that indicate ready up state
    constexpr std::array<std::string_view, 4> ready_messages = {
        "\xA6 You are NOT ready",
        "\n>>>>>>>>>>>>>>>>> ", // For initial match queue
        "\xA6 Match is queued and waiting for players",
        "\xA6 You are no longer ready"
    };

    // display the notification if player should ready
    for (const std::string_view& ready_msg : ready_messages) {
        if (string_starts_with_ignore_case(msg, ready_msg)) {
            set_local_pre_match_active(true);
            break;
        }
    }

    // remove ready up prompt if match is cancelled prematurely
    constexpr std::string_view match_canceled_msg =
        "\xA6 Vote passed: The match has been canceled";
    if (string_starts_with_ignore_case(msg, match_canceled_msg)) {
        set_local_pre_match_active(false);
    }
}

void handle_sound_msg(const std::string_view msg) {
    constexpr std::string_view normal_prefix = "\xA8 ";
    constexpr std::string_view taunt_prefix = "\xA8[Taunt] ";
    if (string_starts_with_ignore_case(msg, taunt_prefix)) {
        play_chat_sound(msg.substr(taunt_prefix.size()), true);
    } else if (string_starts_with_ignore_case(msg, normal_prefix)) {
        play_chat_sound(msg.substr(normal_prefix.size()), false);
    }
}

FunHook<MultiIoPacketHandler> process_chat_line_packet_hook{
    0x00444860,
    [](char* data, const rf::NetAddr& addr) {
        // server-side and client-side
        if (rf::is_server) {
            verify_player_id_in_packet(&data[0], addr, "chat_line");

            rf::Player* src_player = rf::multi_find_player_by_addr(addr);
            if (!src_player) {
                return; // shouldnt happen (protected in rf::multi_io_process_packets)
            }

            const uint8_t team_msg = static_cast<uint8_t>(data[1]);
            const char* msg = data + 2;

            if (team_msg && rf::is_dedicated_server) {
                rf::multi_chat_add_msg(src_player, msg, true);
            }

            if (check_server_chat_command(msg, src_player)) {
                return;
            }
        } else if (!rf::is_dedicated_server) {
            const char* msg = data + 2;
            handle_vote_or_ready_up_msg(msg);
            handle_sound_msg(msg);
        }

        process_chat_line_packet_hook.call_target(data, addr);
    },
};

FunHook<MultiIoPacketHandler> process_name_change_packet_hook{
    0x0046EAE0,
    [](char* data, const rf::NetAddr& addr) {
        // server-side and client-side
        verify_player_id_in_packet(&data[0], addr, "name_change");
        process_name_change_packet_hook.call_target(data, addr);
    },
};

FunHook<MultiIoPacketHandler> process_team_change_packet_hook{
    0x004825B0,
    [](char* data, const rf::NetAddr& addr) {
        // server-side and client-side
        if (rf::is_server) {
            verify_player_id_in_packet(&data[0], addr, "team_change");
            data[1] = std::clamp(data[1], '\0', '\1'); // team validation (fixes "green team")
            rf::Player* player = rf::multi_find_player_by_id(data[0]);
            //xlog::warn("Player {}, team {}", player->name, player->team);
            if (is_player_ready(player) || is_player_in_match(player)) {
                auto msg = std::format("\xA6 You can't change teams {}", is_player_ready(player)
                    ? "while ready for a match. Use \"/unready\" first."
                    : "during a match.");
                af_send_automated_chat_msg(msg, player);
                return;
            }
        }        
        process_team_change_packet_hook.call_target(data, addr);
    },
};

FunHook<MultiIoPacketHandler> process_rate_change_packet_hook{
    0x004807B0,
    [](char* data, const rf::NetAddr& addr) {
        // server-side and client-side?
        verify_player_id_in_packet(&data[0], addr, "rate_change");
        process_rate_change_packet_hook.call_target(data, addr);
    },
};

FunHook<MultiIoPacketHandler> process_entity_create_packet_hook{
    0x00475420,
    [](char* data, const rf::NetAddr& addr) {
        // Temporary change default player weapon to the weapon type from the received packet
        // Created entity always receives Default Player Weapon (from game.tbl) and if server has it overriden
        // player weapons would be in inconsistent state with server without this change.
        size_t name_size = strlen(data) + 1;
        char player_id = data[name_size + 58];
        // Check if this is not NPC
        if (player_id != '\xFF') {
            int weapon_type;
            std::memcpy(&weapon_type, data + name_size + 63, sizeof(weapon_type));
            auto old_default_player_weapon = rf::default_player_weapon;
            rf::default_player_weapon = rf::weapon_types[weapon_type].name;
            process_entity_create_packet_hook.call_target(data, addr);
            rf::default_player_weapon = old_default_player_weapon;
        }
        else {
            process_entity_create_packet_hook.call_target(data, addr);
        }
    },
};

FunHook<MultiIoPacketHandler> process_reload_packet_hook{
    0x00485AB0,
    [](char* data, const rf::NetAddr& addr) {
        if (!rf::is_server) { // client-side
            // Update clip_size and max_ammo if received values are greater than values from local weapons.tbl
            int weapon_type, ammo, clip_ammo;
            std::memcpy(&weapon_type, data + 4, sizeof(weapon_type));
            std::memcpy(&ammo, data + 8, sizeof(ammo));
            std::memcpy(&clip_ammo, data + 12, sizeof(clip_ammo));

            if (rf::weapon_types[weapon_type].clip_size < clip_ammo)
                rf::weapon_types[weapon_type].clip_size = clip_ammo;
            if (rf::weapon_types[weapon_type].max_ammo < ammo)
                rf::weapon_types[weapon_type].max_ammo = ammo;
            xlog::trace("process_reload_packet weapon_type {} clip_ammo {} ammo {}", weapon_type, clip_ammo, ammo);

            // Call original handler
            process_reload_packet_hook.call_target(data, addr);
        }
    },
};

FunHook<MultiIoPacketHandler> process_reload_request_packet_hook{
    0x00485A60,
    [](char* data, const rf::NetAddr& addr) {
        if (!rf::is_server) {
            return;
        }
        rf::Player* pp = rf::multi_find_player_by_addr(addr);
        int weapon_type;
        std::memcpy(&weapon_type, data, sizeof(weapon_type));
        if (pp) {
            void multi_reload_weapon_server_side(rf::Player* pp, int weapon_type);
            multi_reload_weapon_server_side(pp, weapon_type);
        }
    },
};

CodeInjection process_obj_update_check_flags_injection{
        0x0047E058,
        [](auto& regs) {
            auto stack_frame = regs.esp + 0x9C;
            rf::Player* pp = addr_as_ref<rf::Player*>(stack_frame - 0x6C);
            int flags = regs.ebx;
            rf::Entity* ep = regs.edi;
            bool valid = true;
            if (rf::is_server) {
                // server-side
                if (ep && ep->handle != pp->entity_handle) {
                    xlog::trace("Invalid obj_update entity {:x} {:x} {}", ep->handle, pp->entity_handle,
                        pp->name.c_str());
                    valid = false;
                }
                else if (flags & (0x4 | 0x20 | 0x80)) { // OUF_WEAPON_TYPE | OUF_HEALTH_ARMOR | OUF_ARMOR_STATE
                    xlog::info("Invalid obj_update flags {:x}", flags);
                    valid = false;
                }
            }
            if (!valid) {
                regs.edi = 0;
            }
        },
    };

CodeInjection process_obj_update_weapon_fire_injection{
    0x0047E2FF,
    [](auto& regs) {
        rf::Entity* entity = regs.edi;
        int flags = regs.ebx;
        auto stack_frame = regs.esp + 0x9C;
        auto* pp = addr_as_ref<rf::Player*>(stack_frame - 0x6C); // null client-side

        constexpr int ouf_fire = 0x40;
        constexpr int ouf_alt_fire = 0x10;

        bool is_on = flags & ouf_fire;
        bool alt_fire = flags & ouf_alt_fire;
        void multi_turn_weapon_on(rf::Entity* ep, rf::Player* pp, bool alt_fire);
        void multi_turn_weapon_off(rf::Entity* ep);
        if (is_on) {
            multi_turn_weapon_on(entity, pp, alt_fire);
        }
        else {
            multi_turn_weapon_off(entity);
        }
        regs.eip = 0x0047E346;
    },
};

FunHook<uint8_t()> multi_alloc_player_id_hook{
    0x0046EF00,
    []() {
        uint8_t player_id = multi_alloc_player_id_hook.call_target();
        if (player_id == 0xFF)
            player_id = multi_alloc_player_id_hook.call_target();
        return player_id;
    },
};

FunHook<rf::Object*(int32_t)> multi_get_obj_from_server_handle_hook{
    0x00484B00,
    [](int32_t remote_handle) {
        size_t index = static_cast<uint16_t>(remote_handle);
        if (index >= obj_limit)
            return static_cast<rf::Object*>(nullptr);
        return multi_get_obj_from_server_handle_hook.call_target(remote_handle);
    },
};

FunHook<int32_t(int32_t)> multi_get_obj_handle_from_server_handle_hook{
    0x00484B30,
    [](int32_t remote_handle) {
        size_t index = static_cast<uint16_t>(remote_handle);
        if (index >= obj_limit)
            return -1;
        return multi_get_obj_handle_from_server_handle_hook.call_target(remote_handle);
    },
};

FunHook<void(int32_t, int32_t)> multi_set_obj_handle_mapping_hook{
    0x00484B70,
    [](int32_t remote_handle, int32_t local_handle) {
        size_t index = static_cast<uint16_t>(remote_handle);
        if (index >= obj_limit)
            return;
        multi_set_obj_handle_mapping_hook.call_target(remote_handle, local_handle);
    },
};

CodeInjection process_boolean_packet_validate_shape_index_patch{
    0x004765A3,
    [](auto& regs) { regs.ecx = std::clamp<int>(regs.ecx, 0, 3); },
};

CodeInjection process_boolean_packet_validate_room_uid_patch{
    0x0047661C,
    [](auto& regs) {
        int num_rooms = rf::level.geometry->all_rooms.size();
        if (regs.edx < 0 || regs.edx >= num_rooms) {
            xlog::warn("Invalid room in Boolean packet - skipping");
            regs.esp += 0x64;
            regs.eip = 0x004766A5;
        }
    },
};

CodeInjection process_pregame_boolean_packet_validate_shape_index_patch{
    0x0047672F,
    [](auto& regs) {
        // only meshes 0 - 3 are supported
        regs.ecx = std::clamp<int>(regs.ecx, 0, 3);
    },
};

CodeInjection process_pregame_boolean_packet_validate_room_uid_patch{
    0x00476752,
    [](auto& regs) {
        int num_rooms = rf::level.geometry->all_rooms.size();
        if (regs.edx < 0 || regs.edx >= num_rooms) {
            xlog::warn("Invalid room in PregameBoolean packet - skipping");
            regs.esp += 0x68;
            regs.eip = 0x004767AA;
        }
    },
};

CodeInjection process_glass_kill_packet_check_room_exists_patch{
    0x004723B3,
    [](auto& regs) {
        if (regs.eax == 0)
            regs.eip = 0x004723EC;
    },
};

CallHook<int(void*, int, int, rf::NetAddr&, int)> net_get_tracker_hook{
    0x00482ED4,
    [](void* data, int a2, int a3, rf::NetAddr& addr, int super_type) {
        int res = net_get_tracker_hook.call_target(data, a2, a3, addr, super_type);
        if (res != -1 && addr != rf::tracker_addr)
            res = -1;
        return res;
    },
};

std::pair<std::unique_ptr<std::byte[]>, size_t> extend_packet_bytes(const std::byte* data, size_t len, const void* add, size_t add_len)
{
    auto passthrough = [&](const char* why) {
        xlog::warn("extend_packet_bytes: passthrough ({})", why);
        auto out = std::make_unique<std::byte[]>(len);
        std::memcpy(out.get(), data, len);
        return std::pair{std::move(out), len};
    };

    if (!data || len < sizeof(RF_GamePacketHeader))
        return passthrough("bad input");
    if (!add || add_len == 0)
        return passthrough("nothing to append");
    if (len > SIZE_MAX - add_len)
        return passthrough("size_t overflow");

    RF_GamePacketHeader hdr;
    std::memcpy(&hdr, data, sizeof(hdr));

    using size_field_t = decltype(hdr.size);
    static_assert(std::is_unsigned_v<size_field_t>, "header.size must be unsigned");

    // prevent header size overflow
    if (add_len > size_t(std::numeric_limits<size_field_t>::max() - hdr.size))
        return passthrough("header.size overflow");

    auto out = std::make_unique<std::byte[]>(len + add_len);

    hdr.size = static_cast<size_field_t>(hdr.size + static_cast<size_field_t>(add_len));

    // header
    std::memcpy(out.get(), &hdr, sizeof(hdr));
    // old payload
    std::memcpy(out.get() + sizeof(hdr), data + sizeof(hdr), len - sizeof(hdr));
    // appended bytes
    std::memcpy(out.get() + len, add, add_len);

    return {std::move(out), len + add_len};
}

struct AfGiReqSeen
{
    uint8_t ver = 0; // game_info_req version: 1 is current, pre-Alpine v1.2 never sends a version
    int last_seen_ms = 0;
};

static std::unordered_map<uint64_t, AfGiReqSeen> g_af_gi_req_seen;

static inline uint64_t addr_key(const rf::NetAddr& a)
{
    return (uint64_t(a.ip_addr) << 16) | (uint64_t(a.port) & 0xFFFF);
}

// find game_type field inside a game_info packet
static uint8_t* locate_game_type_field(uint8_t* pkt, size_t len)
{
    if (!pkt || len < sizeof(RF_GamePacketHeader) + 1)
        return nullptr;

    const auto* gh = reinterpret_cast<const RF_GamePacketHeader*>(pkt);
    const size_t pkt_payload_len = gh->size;
    if (len < sizeof(*gh) + pkt_payload_len)
        return nullptr;

    uint8_t* payload = pkt + sizeof(*gh);
    uint8_t* end = payload + pkt_payload_len;

    // game_type follows version and server name
    if (payload >= end)
        return nullptr;
    uint8_t* p = payload + 1;       // skip version
    while (p < end && *p != 0) ++p; // scan server name
    if (p >= end)
        return nullptr;
    ++p; // skip NUL
    if (p >= end)
        return nullptr;
    return p; // p = game_type
}

CallHook<int(const rf::NetAddr*, std::byte*, size_t)> send_game_info_packet_hook{
    0x0047B287,
    [](const rf::NetAddr* addr, std::byte* data, size_t len) {
        // core packet ext
        af_sign_packet_ext ext{};
        ext.af_signature = ALPINE_FACTION_SIGNATURE;
        ext.version_major = VERSION_MAJOR;
        ext.version_minor = VERSION_MINOR;
        ext.version_patch = VERSION_PATCH;
        ext.version_type = VERSION_TYPE;
        ext.set_flags(g_game_info_server_flags);

        // level filename
        uint8_t fname[64] = {0};
        size_t fname_len = 0;
        if (rf::level.flags & rf::LEVEL_LOADED) {
            auto s = rf::level.filename.substr(0, 63);
            fname_len = s.size() + 1; // null terminator
            std::memcpy(fname, s.c_str(), fname_len);
        }

        // build tail
        std::vector<uint8_t> tail;
        tail.reserve(sizeof(ext) + fname_len);
        tail.insert(tail.end(), reinterpret_cast<const uint8_t*>(&ext), reinterpret_cast<const uint8_t*>(&ext) + sizeof(ext));

        if (fname_len)
            tail.insert(tail.end(), fname, fname + fname_len);

        auto [buf, new_len] = extend_packet_bytes(data, len, tail.data(), tail.size());

        // Only send new gametype IDs to compatible clients
        // legacy clients will crash when they receive a game_info packet with
        // gametype ID > 2. If the server is running a new gametype, we lie and tell
        // legacy clients that it is gametype 2 (TDM) to avoid crashing them

        // if recipient sent an AF game_info_req within 20 sec, consider it compatible
        // there is a very small risk here that if the server is running a new gametype,
        // a legacy client will crash if the user polled a server list on AF v1.2+ from
        // the same port within this window, but that is extremely unlikely

        // purge stale recorded Alpine client game_info_req entries
        constexpr int seen_ttl_ms = 20000;
        const uint64_t key = addr_key(*addr);
        const int now = rf::timer_get(1000);

        for (auto it = g_af_gi_req_seen.begin(); it != g_af_gi_req_seen.end();) {
            if (now - it->second.last_seen_ms > seen_ttl_ms)
                it = g_af_gi_req_seen.erase(it);
            else
                ++it;
        }

        // check if a fresh Alpine client game_info_req entry was from this socket
        bool is_af_capable = false;
        if (auto it = g_af_gi_req_seen.find(key); it != g_af_gi_req_seen.end()) {
            is_af_capable = (it->second.ver >= 1);
        }

        // game_info_req was from a legacy client, fall back to game_type 2 to avoid accidentally crashing them
        if (!is_af_capable) {
            if (uint8_t* gt = locate_game_type_field(reinterpret_cast<uint8_t*>(buf.get()), new_len)) {
                if (*gt > 2) {
                    xlog::debug("Legacy GI reply to {:x}:{}: mapping game_type {} -> 2", addr->ip_addr, addr->port, int(*gt));
                    *gt = 2;
                }
            }
        }

        return send_game_info_packet_hook.call_target(addr, buf.get(), new_len);
    },
};

std::pair<std::unique_ptr<std::byte[]>, size_t> append_af_tail(const std::byte* pkt, size_t len, const void* core, size_t core_len)
{
    if (!core || core_len == 0) {
        // passthrough
        return extend_packet_bytes(pkt, len, nullptr, 0);
    }

    // tail = [core][footer]
    AFFooter f{};
    f.total_len = static_cast<uint16_t>(core_len);
    f.magic = AF_FOOTER_MAGIC;

    std::vector<uint8_t> tail;
    tail.reserve(core_len + sizeof(f));
    tail.insert(tail.end(),
        reinterpret_cast<const uint8_t*>(core),
        reinterpret_cast<const uint8_t*>(core) + core_len);
    const uint8_t* fptr = reinterpret_cast<const uint8_t*>(&f);
    tail.insert(tail.end(), fptr, fptr + sizeof(f));

    return extend_packet_bytes(pkt, len, tail.data(), tail.size());
}

CallHook<int(const rf::NetAddr*, std::byte*, size_t)> send_game_info_req_packet_hook{
    0x0047B470,
    [](const rf::NetAddr* addr, std::byte* data, size_t len) {
        AFGameInfoReq core{ALPINE_FACTION_SIGNATURE, 1};
        // version 1 of gi_req extension, only used to identify non-legacy clients, unlikely this will need to increment
        auto [buf, new_len] = append_af_tail(data, len, &core, sizeof(core));
        return send_game_info_req_packet_hook.call_target(addr, buf.get(), new_len);
    },
};

std::pair<std::unique_ptr<std::byte[]>, size_t> append_af_v3_tail(
    const std::byte* pkt, size_t len, const AFJoinReq_v2& core_af_ext, const std::vector<uint8_t>& tlvs)
{
    // tail is [core_af_ext][tlvs][footer]
    const uint16_t total_len = static_cast<uint16_t>(sizeof(core_af_ext) + tlvs.size());
    std::vector<uint8_t> tail;
    tail.reserve(total_len + sizeof(AFFooter));

    // core_af_ext
    tail.insert(tail.end(), reinterpret_cast<const uint8_t*>(&core_af_ext), reinterpret_cast<const uint8_t*>(&core_af_ext) + sizeof(core_af_ext));

    // tlvs
    tail.insert(tail.end(), tlvs.begin(), tlvs.end());

    // footer
    AFFooter f{};
    f.total_len = total_len; 
    f.magic = AF_FOOTER_MAGIC;
    const uint8_t* fptr = reinterpret_cast<const uint8_t*>(&f);
    tail.insert(tail.end(), fptr, fptr + sizeof(f));

    return extend_packet_bytes(pkt, len, tail.data(), tail.size());
}

ConsoleCommand2 bot_shared_secret_cmd{
    "bot_shared_secret",
    [] (const std::optional<uint32_t> secret) {
        if (rf::is_dedicated_server) {
            rf::console::print(
                "This console variable is not available on dedicated servers"
            );
            return;
        }

        if (secret.has_value()) {
            g_alpine_game_config.bot_shared_secret = secret.value();
        }

        if (g_alpine_game_config.bot_shared_secret) {
            rf::console::print(
                "Bot shared secret is {}",
                g_alpine_game_config.bot_shared_secret
            );
        } else {
             rf::console::print("Bot shared secret is 0 [disabled]");
        }
    },
    "Set a shared secret to signal your client as a bot",
};

enum JoinReqTlv : uint8_t {
    JR_TLV_BOT_SHARED_SECRET = 0x1,
};

CallHook<int(const rf::NetAddr*, std::byte*, size_t)> send_join_req_packet_hook{
    0x0047ABFB,
    [] (const rf::NetAddr* const addr, std::byte* const data, const size_t len) {
        // Add Alpine Faction info to join_req packet
        AFJoinReq_v2 ext_data{
            .signature = ALPINE_FACTION_SIGNATURE,
            .version_major = VERSION_MAJOR,
            .version_minor = VERSION_MINOR,
            .version_patch = VERSION_PATCH,
            .version_type = VERSION_TYPE,
            .max_rfl_version = MAXIMUM_RFL_VERSION,
            .flags = 0u,
        };

        std::vector<uint8_t> tlvs{};
        tlvs.reserve(32);
        TlvWriter<JoinReqTlv> writer{tlvs};
        if (g_alpine_game_config.bot_shared_secret) {
            writer.write_le(
                JR_TLV_BOT_SHARED_SECRET,
                g_alpine_game_config.bot_shared_secret
            );
        }
        const auto [buf, new_len] = append_af_v3_tail(data, len, ext_data, tlvs);

        return send_join_req_packet_hook.call_target(addr, buf.get(), new_len);
    },
};

CallHook<int(const rf::NetAddr*, std::byte*, size_t)> send_join_accept_packet_hook{
    0x0047A825,
    [](const rf::NetAddr* addr, std::byte* data, size_t len) {
        // Add Alpine Faction signature to join_accept packet
        AlpineFactionJoinAcceptPacketExt ext_data;
        if (server_is_saving_enabled()) {
            ext_data.flags |= AlpineFactionJoinAcceptPacketExt::Flags::saving_enabled;
        }
        if (server_allow_fullbright_meshes()) {
            ext_data.flags |= AlpineFactionJoinAcceptPacketExt::Flags::allow_fb_mesh;
        }
        if (server_allow_lightmaps_only()) {
            ext_data.flags |= AlpineFactionJoinAcceptPacketExt::Flags::allow_lmap;
        }
        if (server_allow_disable_screenshake()) {
            ext_data.flags |= AlpineFactionJoinAcceptPacketExt::Flags::allow_no_ss;
        }
        if (server_no_player_collide()) {
            ext_data.flags |= AlpineFactionJoinAcceptPacketExt::Flags::no_player_collide;
        }
        if (server_allow_disable_muzzle_flash()) {
            ext_data.flags |= AlpineFactionJoinAcceptPacketExt::Flags::allow_no_mf;
        }
        if (server_apply_click_limiter()) {
            ext_data.flags |= AlpineFactionJoinAcceptPacketExt::Flags::click_limit;
            ext_data.semi_auto_cooldown = server_get_alpine_config().click_limiter_config.cooldown;
        }
        if (server_allow_unlimited_fps()) {
            ext_data.flags |= AlpineFactionJoinAcceptPacketExt::Flags::unlimited_fps;
        }
        if (server_gaussian_spread()) {
            ext_data.flags |= AlpineFactionJoinAcceptPacketExt::Flags::gaussian_spread;
        }
        if (server_location_pinging()) {
            ext_data.flags |= AlpineFactionJoinAcceptPacketExt::Flags::location_pinging;
        }
        if (server_delayed_spawns()) {
            ext_data.flags |= AlpineFactionJoinAcceptPacketExt::Flags::delayed_spawns;
        }
        auto [buf, new_len] = extend_packet_bytes(data, len, &ext_data, sizeof(ext_data));
        //auto [new_data, new_len] = extend_packet_fixed(data, len, ext_data);
        return send_join_accept_packet_hook.call_target(addr, buf.get(), new_len);
    },
};

CodeInjection process_join_accept_injection{
    0x0047A979,
    [](auto& regs) {
        std::byte* packet = regs.ebp;
        auto ext_offset = regs.esi + 5;
        AlpineFactionJoinAcceptPacketExt ext_data;
        std::copy(packet + ext_offset, packet + ext_offset + sizeof(AlpineFactionJoinAcceptPacketExt),
            reinterpret_cast<std::byte*>(&ext_data));
        xlog::debug("Checking for join_accept AF extension: {:08X}", ext_data.af_signature);
        if (ext_data.af_signature == ALPINE_FACTION_SIGNATURE) {
            AlpineFactionServerInfo server_info;
            server_info.version_major = ext_data.version_major;
            server_info.version_minor = ext_data.version_minor;
            xlog::debug("Got AF server info: {} {} {}", ext_data.version_major, ext_data.version_minor,
                static_cast<int>(ext_data.flags));
            server_info.saving_enabled = !!(ext_data.flags & AlpineFactionJoinAcceptPacketExt::Flags::saving_enabled);
            server_info.allow_fb_mesh = !!(ext_data.flags & AlpineFactionJoinAcceptPacketExt::Flags::allow_fb_mesh);
            server_info.allow_lmap = !!(ext_data.flags & AlpineFactionJoinAcceptPacketExt::Flags::allow_lmap);
            server_info.allow_no_ss = !!(ext_data.flags & AlpineFactionJoinAcceptPacketExt::Flags::allow_no_ss);
            server_info.no_player_collide = !!(ext_data.flags & AlpineFactionJoinAcceptPacketExt::Flags::no_player_collide);
            server_info.allow_no_mf = !!(ext_data.flags & AlpineFactionJoinAcceptPacketExt::Flags::allow_no_mf);
            server_info.click_limit = !!(ext_data.flags & AlpineFactionJoinAcceptPacketExt::Flags::click_limit);
            server_info.unlimited_fps = !!(ext_data.flags & AlpineFactionJoinAcceptPacketExt::Flags::unlimited_fps);
            server_info.gaussian_spread = !!(ext_data.flags & AlpineFactionJoinAcceptPacketExt::Flags::gaussian_spread);
            server_info.location_pinging = !!(ext_data.flags & AlpineFactionJoinAcceptPacketExt::Flags::location_pinging);
            server_info.delayed_spawns = !!(ext_data.flags & AlpineFactionJoinAcceptPacketExt::Flags::delayed_spawns);

            constexpr float default_fov = 90.0f;
            if (!!(ext_data.flags & AlpineFactionJoinAcceptPacketExt::Flags::max_fov) && ext_data.max_fov >= default_fov) {
                server_info.max_fov = ext_data.max_fov;
            }
            if (!!(ext_data.flags & AlpineFactionJoinAcceptPacketExt::Flags::click_limit)) {
                server_info.semi_auto_cooldown = ext_data.semi_auto_cooldown;
            }
            g_af_server_info = std::optional{server_info};
        }
        else {
            g_af_server_info.reset();
        }
    },
};

static bool parse_af_tail_v3(const uint8_t* payload, size_t payload_len, const AFJoinReq_v2*& out_prefix, const uint8_t*& tlv_begin, const uint8_t*& tlv_end)
{
    if (payload_len < sizeof(AFFooter))
        return false;

    const uint8_t* end = payload + payload_len;
    const auto* footer = reinterpret_cast<const AFFooter*>(end - sizeof(AFFooter));
    if (footer->magic != AF_FOOTER_MAGIC) // validate footer
        return false;

    const uint16_t total_len = footer->total_len;
    if (total_len < sizeof(AFJoinReq_v2))
        return false;
    if (total_len > payload_len - sizeof(AFFooter)) // may be a mistake
        return false;

    const uint8_t* af_start = end - sizeof(AFFooter) - total_len;
    if (af_start < payload)
        return false;

    const auto* pre = reinterpret_cast<const AFJoinReq_v2*>(af_start);
    if (pre->signature != ALPINE_FACTION_SIGNATURE) // validate AF extension
        return false;

    out_prefix = pre;
    tlv_begin = af_start + sizeof(AFJoinReq_v2);
    tlv_end = end - sizeof(AFFooter);
    return true;
}

static bool parse_af_join_req_any_tail(const uint8_t* pkt, size_t datalen, size_t rawlen)
{
    g_joining_client_version = ClientVersion::unknown;
    g_joining_player_info = {};
    if (!pkt || datalen < sizeof(RF_GamePacketHeader) || rawlen < sizeof(RF_GamePacketHeader))
        return false;

    RF_GamePacketHeader gh{};
    std::memcpy(&gh, pkt, sizeof(gh));
    if (gh.type != RF_GPT_JOIN_REQUEST)
        return false;

    const size_t rf_payload_end = sizeof(RF_GamePacketHeader) + gh.size;

    // is it a browser?
    // Note: RFSB appends its tail after the boundary of the RF header size field
    if (rawlen >= rf_payload_end + sizeof(SBJoinReq_v5_1)) { // rfsb 5.1.4
        SBJoinReq_v5_1 t{};
        std::memcpy(&t, pkt + rf_payload_end, sizeof(t));

        const bool is_rfsb =
            t.sig == 0xEB &&
            t.c1 == 0x05 &&
            t.c2 == 0x00 &&
            t.c3 == 0x01 &&
            t.c4 == 0x03 &&
            t.c5 == 0x8B &&
            t.c6 == 0x05 &&
            t.c7 == 0x01 &&
            t.c8 == 0xBD;

        if (is_rfsb) {
            g_joining_client_version = ClientVersion::browser;
            g_joining_player_info.af_signature = t.sig;
            g_joining_player_info.version_major = 5u;
            g_joining_player_info.version_minor = 1u;
            g_joining_player_info.version_patch = 4u;
            g_joining_player_info.version_type = VERSION_TYPE_RELEASE;
            g_joining_player_info.max_rfl_version = MAXIMUM_RFL_VERSION;
            g_joining_player_info.flags = AlpineFactionJoinReqPacketExt::Flags::none;
            return true;
        }
    }

    if (rawlen >= rf_payload_end + sizeof(SBJoinReq_v5_0)) { // rfsb 5.0.1
        SBJoinReq_v5_0 t{};
        std::memcpy(&t, pkt + rf_payload_end, sizeof(t));

        const bool is_rfsb =
            t.sig == 0xEB &&
            t.c0 == 0x05 &&
            t.c1 == 0x00 &&
            t.c2 == 0x01 &&
            t.c3 == 0x03 &&
            t.c4 == 0x8B &&
            t.c5 == 0x05 &&
            t.c6 == 0x00;

        if (is_rfsb) {
            g_joining_client_version = ClientVersion::browser;
            g_joining_player_info.af_signature = t.sig;
            g_joining_player_info.version_major = 5u;
            g_joining_player_info.version_minor = 0u;
            g_joining_player_info.version_patch = 1u;
            g_joining_player_info.version_type = VERSION_TYPE_RELEASE;
            g_joining_player_info.max_rfl_version = MAXIMUM_RFL_VERSION;
            g_joining_player_info.flags = AlpineFactionJoinReqPacketExt::Flags::none;
            return true;
        }
    }

    // not a browser, is it a Alpine or Dash client?
    const uint8_t* payload = pkt + sizeof(gh);
    const size_t plen = gh.size;
    if (datalen < sizeof(gh) + plen)
        return false;

    // try to parse as v3 (AF v1.2+)
    const AFJoinReq_v2* pre = nullptr;
    const uint8_t *tlv_b = nullptr, *tlv_e = nullptr;

    if (parse_af_tail_v3(payload, plen, pre, tlv_b, tlv_e)) {
        xlog::debug("matched v3 join_req tail with signature {}", pre->signature);
        g_joining_client_version = ClientVersion::alpine_faction;
        g_joining_player_info.af_signature = pre->signature;
        g_joining_player_info.version_major = pre->version_major;
        g_joining_player_info.version_minor = pre->version_minor;
        g_joining_player_info.version_patch = pre->version_patch;
        g_joining_player_info.version_type = pre->version_type;
        g_joining_player_info.max_rfl_version = pre->max_rfl_version;
        g_joining_player_info.flags = static_cast<AlpineFactionJoinReqPacketExt::Flags>(pre->flags);

        TlvReader<JoinReqTlv> reader{tlv_b, tlv_e};
        while (std::optional value = reader.next()) {
            if (value->type == JR_TLV_BOT_SHARED_SECRET) {
                const uint32_t secret = value->read_le<uint32_t>();
                if (secret) {
                    g_joining_player_info.bot_shared_secret = std::optional{secret};
                }
            }
        }

        return true;
    }

    // try to parse as v2 (AF v1.1)
    if (plen >= sizeof(AFJoinReq_v2)) {
        const uint8_t* p = payload + plen - sizeof(AFJoinReq_v2);
        const auto* v2 = reinterpret_cast<const AFJoinReq_v2*>(p);
        xlog::debug("matched v2 join_req tail, sig {}", v2->signature);
        if (v2->signature == ALPINE_FACTION_SIGNATURE) {
            g_joining_client_version = ClientVersion::alpine_faction;
            g_joining_player_info.af_signature = v2->signature;
            g_joining_player_info.version_major = v2->version_major;
            g_joining_player_info.version_minor = v2->version_minor;
            g_joining_player_info.version_patch = v2->version_patch;
            g_joining_player_info.version_type = v2->version_type;
            g_joining_player_info.max_rfl_version = v2->max_rfl_version;
            g_joining_player_info.flags = static_cast<AlpineFactionJoinReqPacketExt::Flags>(v2->flags);
            return true;
        }
    }

    // try to parse as v1 (AF v1.0)
    if (plen >= sizeof(AFJoinReq_v1)) {        
        const uint8_t* p = payload + plen - sizeof(AFJoinReq_v1);
        const auto* v1 = reinterpret_cast<const AFJoinReq_v1*>(p);
        xlog::debug("matched v1 join_req tail, sig {}", v1->signature);
        if (v1->signature == ALPINE_FACTION_SIGNATURE) {
            g_joining_client_version = ClientVersion::alpine_faction;
            g_joining_player_info.af_signature = v1->signature;
            g_joining_player_info.version_major = v1->version_major;
            g_joining_player_info.version_minor = v1->version_minor;
            g_joining_player_info.version_patch = 0u;
            g_joining_player_info.version_type = VERSION_TYPE_RELEASE;
            g_joining_player_info.max_rfl_version = 300u;
            g_joining_player_info.flags = AlpineFactionJoinReqPacketExt::Flags::none;
            return true;
        }
    }

    // try to parse as authentic DF tail
    if (plen >= sizeof(DFJoinReq_v1)) {
        const uint8_t* p = payload + plen - sizeof(DFJoinReq_v1);
        const auto* v2 = reinterpret_cast<const DFJoinReq_v1*>(p);
        xlog::debug("matched df join_req tail, sig {}", v2->signature);
        if (v2->signature == DASH_FACTION_SIGNATURE) {
            g_joining_client_version = ClientVersion::dash_faction;
            g_joining_player_info.af_signature = v2->signature;
            g_joining_player_info.version_major = v2->version_major;
            g_joining_player_info.version_minor = v2->version_minor;
            g_joining_player_info.version_patch = 0u;
            g_joining_player_info.version_type = VERSION_TYPE_RELEASE;
            g_joining_player_info.max_rfl_version = 200u;
            g_joining_player_info.flags = AlpineFactionJoinReqPacketExt::Flags::none;
            return true;
        }
        // Dash v1.9 clients masquerading as Alpine v1.1 clients
        else if (v2->signature == ALPINE_FACTION_SIGNATURE) {
            g_joining_client_version = ClientVersion::dash_faction;
            g_joining_player_info.af_signature = v2->signature;
            g_joining_player_info.version_major = 1u;
            g_joining_player_info.version_minor = 9u;
            g_joining_player_info.version_patch = 0u;
            g_joining_player_info.version_type = VERSION_TYPE_BETA;
            g_joining_player_info.max_rfl_version = 200u;
            g_joining_player_info.flags = AlpineFactionJoinReqPacketExt::Flags::none;
            return true;
        }
    }

    // not Alpine or Dash, assume it's a v1.2 retail client
    // could also be PF, but no way implemented yet to detect that
    g_joining_client_version = ClientVersion::unknown;
    g_joining_player_info.af_signature = 0u;
    g_joining_player_info.version_major = 1u;
    g_joining_player_info.version_minor = 2u;
    g_joining_player_info.version_patch = 0u;
    g_joining_player_info.version_type = VERSION_TYPE_RELEASE;
    g_joining_player_info.max_rfl_version = 200u;
    g_joining_player_info.flags = AlpineFactionJoinReqPacketExt::Flags::none;

    // could not match a known tail
    return false;
}

FunHook<void(int, rf::NetAddr*)> process_join_req_packet_hook{
    0x0047AC60,
    [](int pPacket, rf::NetAddr* addr) {
        process_join_req_packet_hook.call_target(pPacket, addr);

        if (rf::Player* valid_player = rf::multi_find_player_by_addr(*addr)) { // player successfully joined
            if (g_joining_client_version == ClientVersion::alpine_faction ||
                g_joining_client_version == ClientVersion::dash_faction ||
                g_joining_client_version == ClientVersion::browser) {
                auto& pdata = get_player_additional_data(valid_player);

                pdata.client_version = g_joining_client_version;
                pdata.client_version_major = g_joining_player_info.version_major;
                pdata.client_version_minor = g_joining_player_info.version_minor;
                pdata.client_version_patch = g_joining_player_info.version_patch;
                pdata.client_version_type = g_joining_player_info.version_type;
                pdata.max_rfl_version = g_joining_player_info.max_rfl_version;

                if (g_joining_player_info.bot_shared_secret
                    == std::optional{g_alpine_server_config.bot_shared_secret}) {
                    pdata.is_bot_player = true;
                    rf::console::print(
                        "{}'s bot shared secret was valid",
                        valid_player->name
                    );
                } else {
                    rf::console::print(
                        "{}'s bot shared secret was invalid",
                        valid_player->name
                    );
                }

                // reset for safety
                g_joining_player_info = {};
                g_joining_client_version = ClientVersion::unknown;
            }

            if (g_dedicated_launched_from_ads) {
                print_player_info(valid_player, true);
            }
        }
    },
};

FunHook<int(rf::NetAddr*, rf::JoinRequest*)> check_access_for_new_player_hook {
    0x0047AE10,
    [](rf::NetAddr* addr, rf::JoinRequest* join_req) {
        auto reason = check_access_for_new_player_hook.call_target(addr, join_req);
        
        if (reason != 0 && rf::is_dedicated_server) {
            in_addr ia;
            ia.S_un.S_addr = ntohl(addr->ip_addr);
            RF_JoinDenyReason jdr = static_cast<RF_JoinDenyReason>(reason);
            std::string jdr_str = "unknown";

            if (jdr == RF_JoinDenyReason::RF_JDR_INVALID_PASSWORD) {
                jdr_str = std::format("wrong password '{}'", join_req->password);
            }
            else if (jdr == RF_JoinDenyReason::RF_JDR_BANNED) {
                jdr_str = "banned";
            }
            else if (jdr == RF_JoinDenyReason::RF_JDR_SERVER_IS_FULL) {
                jdr_str = "server full";
            }
            else if (jdr == RF_JoinDenyReason::RF_JDR_THE_SAME_IP) {
                jdr_str = "same socket as another player";
            }
            else if (jdr == RF_JoinDenyReason::RF_JDR_LEVEL_CHANGING) {
                jdr_str = "level change in progress";
            }
            else if (jdr == RF_JoinDenyReason::RF_JDR_DATA_DOESNT_MATCH) {
                jdr_str = "failed data validation";
            }

            rf::console::print("Join request from {}:{} was rejected (reason: {})\n", inet_ntoa(ia), addr->port, jdr_str);
        }

        return reason;
    },
};

static std::tuple<AlpineRestrictVerdict, std::string, bool> check_join_request_restrict_status(
    ClientVersion cv, const AlpineFactionJoinReqPacketExt& info)
{
    const ClientVersionInfoProfile version_info{cv, info.version_major, info.version_minor, info.version_patch, info.version_type, info.max_rfl_version};

    const auto [verdict, verdict_string, hard_reject] = evaluate_alpine_restrict_status(version_info, true);

    return {verdict, verdict_string, hard_reject};
}

CodeInjection process_join_req_injection{
    0x0047ADAB,
    [](auto& regs) {
        bool found_tail = parse_af_join_req_any_tail(g_join_request_stashed.pkt, g_join_request_stashed.len, g_join_request_stashed.rx_len);
        g_join_request_stashed = {};

        const auto [verdict, reason, hard_reject] = check_join_request_restrict_status(g_joining_client_version, g_joining_player_info);

        if (verdict != AlpineRestrictVerdict::ok && hard_reject) {
            if (auto* addr = static_cast<rf::NetAddr*>(regs.esi)) {
                in_addr ia;
                ia.S_un.S_addr = ntohl(addr->ip_addr);
                rf::console::print("Join request from {}:{} was rejected (reason: {})\n", inet_ntoa(ia), addr->port, reason);
            }

            regs.eax = 8; // RF_JDR_UNSUPPORTED_VERSION
        }
    },
};

CodeInjection process_join_accept_send_game_info_req_injection{
    0x0047AA00,
    [](auto& regs) {
        // Force game_info update in case we were joining using protocol handler (server list not fully refreshed) or
        // using old fav entry with outdated name
        rf::NetAddr* server_addr = regs.edi;
        xlog::trace("Sending game_info_req to {:x}:{}", server_addr->ip_addr, server_addr->port);
        rf::send_game_info_req_packet(*server_addr);
    },
};

static inline const char* rcon_player_name(const rf::NetAddr a)
{
    if (auto p = rf::multi_find_player_by_addr(a)) {
        return p->name.empty() ? "<unnamed player>" : p->name.c_str();
    }
    return "<unknown player>";
}

CodeInjection process_rcon_packet_log1{
    0x0046C7E4,
    [](auto& regs) {
        const char* cmd = reinterpret_cast<const char*>(regs.esp + 0x10);
        auto& addr = *addr_as_ref<rf::NetAddr*>(regs.esp + 0x410 + 8);
        const char* pname = rcon_player_name(addr);

        rf::console::print(
            "{} issued command '{}' via rcon, successfully executed",
            rcon_player_name(addr), cmd ? cmd : "");
    },
};

CodeInjection process_rcon_packet_log2{
    0x0046C71D,
    [](auto& regs) {
        const uint32_t netplayer_flags = *reinterpret_cast<const uint32_t*>(regs.eax + 0x8);
        const bool authorized = (netplayer_flags & 0x100u) != 0u;

        if (!authorized) {
            const char* cmd = addr_as_ref<const char*>(regs.esp + 0x410 + 4);
            const auto& addr = *addr_as_ref<rf::NetAddr*>(regs.esp + 0x410 + 8);

            rf::console::print(
                "{} issued command '{}' via rcon, DENIED because not an rcon holder",
                rcon_player_name(addr), cmd ? cmd : "");
        }
    },
};

CodeInjection process_rcon_packet_log3{
    0x0046C7D9,
    [](auto& regs) {
        const char* cmd = reinterpret_cast<const char*>(regs.esp + 0x10);
        const auto& addr = *addr_as_ref<rf::NetAddr*>(regs.esp + 0x410 + 8);

        rf::console::print(
            "{} issued command '{}' via rcon, DENIED because not an allowed rcon command",
            rcon_player_name(addr), cmd ? cmd : "");
    },
};

CodeInjection rcon_pw_denied_log{
    0x0046C5A0,
    [](auto& regs) {
        const char* tried_pw = reinterpret_cast<const char*>(regs.esp + 0x10);
        const auto& addr = *addr_as_ref<rf::NetAddr*>(regs.esp + 0x118);

        if (!tried_pw || rf::rcon_password[0] == '\0')
            return;

        if (std::strcmp(tried_pw, rf::rcon_password) != 0) {
            rf::console::print("{} requested rcon with password '{}', DENIED because the password is wrong",
                rcon_player_name(addr), tried_pw ? tried_pw : "");
        }
        else {
            rf::console::print("{} requested rcon with the correct rcon password", rcon_player_name(addr));
        }
    },
};

CodeInjection process_entity_create_packet_injection{
    0x0047559B,
    [](auto& regs) {
        rf::Player* player = regs.ebx;
        int mp_character = regs.edx;

        // save my current character if the server forced me to spawn with a different one
        if (player == rf::local_player && player->settings.multi_character != mp_character) {
            g_desired_multiplayer_character = player->settings.multi_character;
            xlog::debug("Server forced spawn as character {}. Caching current character {}.",
                mp_character, g_desired_multiplayer_character.value_or(-1));
        }
    },
};

CodeInjection process_entity_create_packet_injection2{
    0x0047560E,
    [](auto& regs) {
        rf::Player* player = regs.ebx;

        // after spawning, set my character back to the one I want
        if (player == rf::local_player && g_desired_multiplayer_character.has_value()) {
            xlog::debug("Setting character to cached value {}.", g_desired_multiplayer_character.value());
            player->settings.multi_character = g_desired_multiplayer_character.value();
            g_desired_multiplayer_character.reset();
        }
    },
};

std::optional<std::string> determine_local_ip_address()
{
    ULONG forward_table_size = 0;
    if (GetIpForwardTable(nullptr, &forward_table_size, FALSE) != ERROR_INSUFFICIENT_BUFFER) {
        xlog::error("GetIpForwardTable failed");
        return {};
    }

    std::unique_ptr<MIB_IPFORWARDTABLE> forward_table{ reinterpret_cast<PMIB_IPFORWARDTABLE>(operator new(forward_table_size)) };
    if (GetIpForwardTable(forward_table.get(), &forward_table_size, TRUE) != NO_ERROR) {
        xlog::error("GetIpForwardTable failed");
        return {};
    }
    IF_INDEX default_route_if_index = NET_IFINDEX_UNSPECIFIED;
    for (unsigned i = 0; i < forward_table->dwNumEntries; i++) {
        auto& row = forward_table->table[i];
        if (row.dwForwardDest == 0) {
            // Default route to gateway
            xlog::debug("Found default route: IfIndex {}", row.dwForwardIfIndex);
            default_route_if_index = row.dwForwardIfIndex;
            break;
        }
    }
    if (default_route_if_index == NET_IFINDEX_UNSPECIFIED) {
        xlog::info("No default route found - is this computer connected to internet?");
        return {};
    }

    ULONG ip_table_size = 0;
    if (GetIpAddrTable(nullptr, &ip_table_size, FALSE) != ERROR_INSUFFICIENT_BUFFER) {
        xlog::error("GetIpAddrTable failed");
        return {};
    }
    std::unique_ptr<MIB_IPADDRTABLE> ip_table{ reinterpret_cast<PMIB_IPADDRTABLE>(operator new(ip_table_size)) };
    if (GetIpAddrTable(ip_table.get(), &ip_table_size, FALSE) != NO_ERROR) {
        xlog::error("GetIpAddrTable failed");
        return {};
    }
    for (unsigned i = 0; i < ip_table->dwNumEntries; ++i) {
        auto& row = ip_table->table[i];
        auto* addr_bytes = reinterpret_cast<uint8_t*>(&row.dwAddr);
        auto addr_str = std::format("{}.{}.{}.{}", addr_bytes[0], addr_bytes[1], addr_bytes[2], addr_bytes[3]);
        xlog::debug("IpAddrTable: dwIndex {} dwAddr {}", row.dwIndex, addr_str);
        if (row.dwIndex == default_route_if_index) {
            return {addr_str};
        }
    }
    xlog::debug("Interface {} not found", default_route_if_index);
    return {};
}

bool try_to_auto_forward_port(int port)
{
    xlog::Logger log{"UPnP"};
    log.info("Configuring UPnP port forwarding (port {})", port);

    auto local_ip_addr_opt = determine_local_ip_address();
    if (!local_ip_addr_opt) {
        log.warn("Cannot determine local IP address");
        return false;
    }
    log.info("Local IP address: {}", local_ip_addr_opt.value().c_str());

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        log.warn("CoInitializeEx failed: hr {:x}", hr);
        return false;
    }

    ComPtr<IUPnPNAT> nat;
    hr = CoCreateInstance(__uuidof(UPnPNAT), nullptr, CLSCTX_ALL, __uuidof(IUPnPNAT), reinterpret_cast<void**>(&nat));
    if (FAILED(hr)) {
        log.warn("CoCreateInstance IUPnPNAT failed: hr {:x}", hr);
        return false;
    }

    ComPtr<IStaticPortMappingCollection> collection;
    int attempt_num = 0;
    while (true) {
        hr = nat->get_StaticPortMappingCollection(&collection);
        if (FAILED(hr)) {
            log.warn("IUPnPNAT::get_StaticPortMappingCollection failed: hr {:x}", hr);
            return false;
        }
        if (collection) {
            break;
        }

        // get_StaticPortMappingCollection sometimes sets collection to nullptr and returns success
        // It may return a proper collecion pointer after few seconds. See UltraVNC code:
        // https://github.com/veyon/ultravnc/blob/master/uvnc_settings2/uvnc_settings/upnp.cpp
        log.info("IUPnPNAT::get_StaticPortMappingCollection returned hr {:x}, collection {}", hr, static_cast<void*>(&*collection));
        ++attempt_num;
        if (attempt_num == 10) {
            return false;
        }
        Sleep(1000);
    }
    log.info("Got NAT port mapping table after {} tries", attempt_num);

    wchar_t ip_addr_wide_str[256];
    mbstowcs(ip_addr_wide_str, local_ip_addr_opt.value().c_str(), std::size(ip_addr_wide_str));
    auto* proto = SysAllocString(L"UDP");
    auto* desc = SysAllocString(L"Red Faction");
    auto* internal_client = SysAllocString(ip_addr_wide_str);
    ComPtr<IStaticPortMapping> mapping;
    hr = collection->Add(port, proto, port, internal_client, TRUE, desc, &mapping);
    SysFreeString(proto);
    SysFreeString(desc);
    SysFreeString(internal_client);
    if (FAILED(hr)) {
        log.warn("IStaticPortMappingCollection::Add failed: hr {:x}", hr);
        return false;
    }
    log.info("Successfully added UPnP port forwarding (port {})", port);
    return true;
}

FunHook<void(int, rf::NetAddr*)> multi_start_hook{
    0x0046D5B0,
    [](int is_client, rf::NetAddr *serv_addr) {
        if (!rf::net_port && !is_client) {
            // If no port was specified and this is a server recreate the socket and bind it to port 7755
            xlog::info("Recreating socket using TCP port 7755");
            shutdown(rf::net_udp_socket, 1);
            closesocket(rf::net_udp_socket);
            rf::net_init_socket(7755);
        }
        multi_start_hook.call_target(is_client, serv_addr);
    },
};

FunHook<void()> tracker_do_broadcast_server_hook{
    0x00483130,
    []() {
        tracker_do_broadcast_server_hook.call_target();
        if (g_alpine_server_config.upnp_enabled) {
            // Auto forward server port using UPnP (in background thread)
            std::thread upnp_thread{try_to_auto_forward_port, rf::net_port};
            upnp_thread.detach();
        }
    },
};

FunHook<void()> multi_stop_hook{
    0x0046E2C0,
    []() {
        g_af_server_info.reset(); // Clear server info when leaving
        g_local_player_spectators.clear();
        g_remote_server_cfg_popup.reset();
        set_local_pre_match_active(false); // clear pre-match state when leaving
        reset_local_pending_game_type(); // clear pending game type when leaving
        if (rf::local_player) {
            reset_player_additional_data(rf::local_player); // clear player additional data when leaving
        }
        multi_stop_hook.call_target();
    },
};

const std::optional<AlpineFactionServerInfo>& get_df_server_info()
{
    return g_af_server_info;
}

std::optional<AlpineFactionServerInfo>& get_af_server_info_mutable()
{
    return g_af_server_info;
}

void send_chat_line_packet(const std::string_view msg, rf::Player* target, rf::Player* sender, bool is_team_msg)
{
    if (!rf::is_server && sender == nullptr) {
        sender = rf::local_player;
    }
    rf::ubyte buf[512];
    RF_ChatLinePacket packet;
    packet.header.type = RF_GPT_CHAT_LINE;
    const size_t len = std::min(msg.size(), 255uz);
    packet.header.size = static_cast<uint16_t>(sizeof(packet) - sizeof(packet.header) + len + 1);
    packet.player_id = sender ? sender->net_data->player_id : 0xFF;
    packet.is_team_msg = is_team_msg;
    std::memcpy(buf, &packet, sizeof(packet));
    char* packet_msg = reinterpret_cast<char*>(buf + sizeof(packet));
    std::strncpy(packet_msg, msg.data(), len);
    packet_msg[len] = '\0';
    if (target == nullptr && rf::is_server) {
        rf::multi_io_send_reliable_to_all(buf, packet.header.size + sizeof(packet.header), 0);
        rf::console::print("Server: {}", msg);
    }
    else {
        rf::multi_io_send_reliable(target, buf, packet.header.size + sizeof(packet.header), 0);
    }
}

CodeInjection client_update_rate_injection{
    0x0047E5D8,
    [](auto& regs) {
        auto& send_obj_update_interval = *static_cast<int*>(regs.esp);
        send_obj_update_interval = 1000 / g_update_rate;
    },
};

CodeInjection server_update_rate_injection{
    0x0047E891,
    [](auto& regs) {
        auto& min_send_obj_update_interval = *static_cast<int*>(regs.esp);
        min_send_obj_update_interval = 1000 / g_alpine_game_config.server_netfps;
    },
};

ConsoleCommand2 netfps_cmd{
    "sv_netfps",
    [](std::optional<int> update_rate) {
        if (update_rate) {
            g_alpine_game_config.set_server_netfps(update_rate.value());
        }
        rf::console::print("Server netfps: {}", g_alpine_game_config.server_netfps);
    },
    "Set number of updates sent from server to clients per second",
};

CodeInjection obj_interp_rotation_fix{
    0x0048443C,
    [](auto& regs) {
        auto& phb_diff = addr_as_ref<rf::Vector3>(regs.ecx);
        constexpr float pi = 3.141592f;
        if (phb_diff.y > pi) {
            phb_diff.y -= 2 * pi;
        }
        else if (phb_diff.y < -pi) {
            phb_diff.y += 2 * pi;
        }
    },
};

CodeInjection obj_interp_too_fast_fix{
    0x00483C3B,
    [](auto& regs) {
        // Make all calculations on milliseconds instead of using microseconds and rounding them up
        auto now = rf::timer_get(1000);
        int frame_time_us = regs.ebp;
        regs.eax = now - frame_time_us;
        regs.edi = now;
    },
};

CodeInjection send_state_info_injection{
    0x0048186F,
    [](auto& regs) {
        rf::Player* player = regs.edi;
        trigger_send_state_info(player);
        pf_player_level_load(player);
    },
};

FunHook<void(rf::Player*)> send_players_packet_hook{
    0x00481C70,
    [](rf::Player *player) {
        send_players_packet_hook.call_target(player);
        pf_player_init(player);
        if (rf::is_server) {
            server_reliable_socket_ready(player);
        }
    },
};

FunHook<void(rf::Entity*, int, int, int)> send_reload_packet_hook{
    0x00485B50, [](rf::Entity* ep, int weapon_type, int clip_ammo, int ammo) {
        // Log the clip_ammo and ammo values
        //xlog::warn("Sending a reload packet for {} with weapon {}, clip_ammo: {}, ammo: {}", ep->name, weapon_type, clip_ammo, ammo);

        // Call the original function
        send_reload_packet_hook.call_target(ep, weapon_type, clip_ammo, ammo);
    }};


extern FunHook<void __fastcall(void*, int, int, bool, int)> multi_io_stats_add_hook;

void __fastcall multi_io_stats_add_new(void *this_, int edx, int size, bool is_send, int packet_type)
{
    // Fix memory corruption when sending/processing packets with non-standard type
    if (packet_type < 56) {
        multi_io_stats_add_hook.call_target(this_, edx, size, is_send, packet_type);
    }
}

FunHook<void __fastcall(void*, int, int, bool, int)> multi_io_stats_add_hook{0x0047CAC0, multi_io_stats_add_new};

static void process_custom_packet([[maybe_unused]] void* data, [[maybe_unused]] int len,
                                  [[maybe_unused]] const rf::NetAddr& addr, [[maybe_unused]] rf::Player* player)
{
    pf_process_packet(data, len, addr, player);
    af_process_packet(data, len, addr, player);
}

static bool parse_af_gi_req_tail(const uint8_t* pkt, size_t datalen, uint8_t& out_ver)
{
    out_ver = 0;

    if (!pkt || datalen < sizeof(RF_GamePacketHeader))
        return false;

    RF_GamePacketHeader gh{};
    std::memcpy(&gh, pkt, sizeof(gh));
    if (gh.type != RF_GPT_GAME_INFO_REQUEST)
        return false;

    const uint8_t* payload = pkt + sizeof(gh);
    const size_t plen = gh.size;
    if (datalen < sizeof(gh) + plen)
        return false;
    if (plen < sizeof(AFFooter))
        return false;

    const uint8_t* end = payload + plen;
    const auto* foot = reinterpret_cast<const AFFooter*>(end - sizeof(AFFooter));
    if (foot->magic != AF_FOOTER_MAGIC)
        return false;

    const uint16_t total_len = foot->total_len;
    if (total_len < sizeof(AFGameInfoReq))
        return false;
    if (total_len > plen - sizeof(AFFooter))
        return false;

    const uint8_t* af_start = end - sizeof(AFFooter) - total_len;
    if (af_start < payload)
        return false;

    const auto* core = reinterpret_cast<const AFGameInfoReq*>(af_start);
    if (core->signature != ALPINE_FACTION_SIGNATURE)
        return false;

    out_ver = core->gi_req_ext_ver;
    return true;
}

CodeInjection multi_io_process_packets_injection{
    0x0047918D,
    [](auto& regs) {
        int packet_type = regs.esi;
        if (packet_type > 0x37 || packet_type == static_cast<int>(pf_packet_type::player_stats)) {
            auto stack_frame = regs.esp + 0x1C;
            std::byte* data = regs.ecx;
            int offset = regs.ebp;
            int len = regs.edi;
            auto& addr = *addr_as_ref<rf::NetAddr*>(stack_frame + 0xC);
            auto player = addr_as_ref<rf::Player*>(stack_frame + 0x10);
            process_custom_packet(data + offset, len, addr, player);
            regs.eip = 0x00479194;
        }
        if (rf::is_dedicated_server || (rf::is_server && !rf::is_dedicated_server)) {
            // stash the join req packet so we can analyze it if the player successfully joins
            if (packet_type == static_cast<int>(RF_GamePacketType::RF_GPT_JOIN_REQUEST)) {
                const uint8_t* base = static_cast<const uint8_t*>(regs.ecx);
                auto stack_frame = regs.esp + 0x1C;
                auto& addr = *addr_as_ref<rf::NetAddr*>(stack_frame + 0xC);
                const int off = regs.ebp;
                const int len = regs.edi;

                // UDP datagram
                size_t rx_len = 0;
                if (g_rx_base && g_rx_len && base + off >= g_rx_base && base + off <= g_rx_base + g_rx_len) {
                    rx_len = (g_rx_base + g_rx_len) - (base + off);
                }

                // join req stash for later analysis
                g_join_request_stashed = {addr, base + off, size_t(len), rx_len, uint8_t(packet_type)};
            }
            // analyze the game_info_req packet so we can adjust the response if needed
            if (packet_type == static_cast<int>(RF_GamePacketType::RF_GPT_GAME_INFO_REQUEST)) {
                const uint8_t* base = static_cast<const uint8_t*>(regs.ecx);
                auto stack_frame = regs.esp + 0x1C;
                const auto& addr = *addr_as_ref<rf::NetAddr*>(stack_frame + 0xC);
                const int off = regs.ebp;
                const int len = regs.edi;

                uint8_t ver = 0;
                if (parse_af_gi_req_tail(base + off, size_t(len), ver)) {
                    const int now = rf::timer_get(1000);
                    g_af_gi_req_seen[addr_key(addr)] = AfGiReqSeen{ver, now};
                    xlog::debug("AF GI-REQ detected from {:x}:{} (ver={})", addr.ip_addr, addr.port, ver);
                }
            }
        }
    },
};

CallHook<void(const void*, size_t, const rf::NetAddr&, rf::Player*)> process_unreliable_game_packets_hook{
    0x00479244,
    [](const void* data, size_t len, const rf::NetAddr& addr, rf::Player* player) {
        // stash full UDP datagram
        if (rf::is_server) {
            g_rx_base = static_cast<const uint8_t*>(data);
            g_rx_len = len;
        }

        if (pf_process_raw_unreliable_packet(data, len, addr)) {
            return;
        }
        rf::multi_io_process_packets(data, len, addr, player);

        // clear UDP datagram stash after parsing
        if (rf::is_server) {
            g_rx_base = nullptr;
            g_rx_len = 0;
        }
    },
};

CodeInjection net_rel_work_injection{
    0x005291F7,
    [](auto& regs) {
        // Clear rsocket variable before processing next packet
        regs.ebp = 0;
    },
};

CallHook<int()> game_info_num_players_hook{
    0x0047B141,
    []() {
        int player_count = 0;
        auto player_list = SinglyLinkedList{rf::player_list};
        for (auto& current_player : player_list) {
            if (get_player_additional_data(&current_player).client_version == ClientVersion::browser) continue;
            player_count++;
        }
        return player_count;
    },
};

// add af_obj_update packet, send at the same time as normal obj_update packet
// note this is after the check for entity flag 1 (dying) and obj flag 2 (pending delete)
CodeInjection send_players_obj_update_packets_injection{
    0x0047E710,
    [](auto& regs) {
        rf::Player* player = regs.esi;
        // use new packet for clients that can process it (Alpine 1.1+)
        if (player) {
            if (is_player_minimum_af_client_version(player, 1, 1)) {
                af_send_obj_update_packet(player);
            }
        }
    },
};

FunHook<void(rf::Player*)> send_netgame_update_packet_hook{
    0x00484DD0,
    [] (rf::Player* const player) {
        const auto send_stats = [] (rf::Player* const player) {
            const auto& pdata = get_player_additional_data(player);
            if (pdata.client_version == ClientVersion::pure_faction
                || is_player_minimum_af_client_version(player, 1, 2)) {
                send_pf_player_stats_packet(player);
            }
        };

        if (!player) {
            for (rf::Player& p : SinglyLinkedList{rf::player_list}) {
                send_stats(&p);
            }
        } else {
            send_stats(player);
        }

        send_netgame_update_packet_hook.call_target(player);
    },
};

void network_init()
{
    // Support af_obj_update packet
    send_players_obj_update_packets_injection.install();

    // Improve simultaneous ping
    rf::simultaneous_ping = 32;

    // Change server info timeout to 2s
    write_mem<u32>(0x0044D357 + 2, 2000);

    // Change delay between server info requests
    write_mem<u8>(0x0044D338 + 1, 20);

    // Allow ports < 1023 (especially 0 - any port)
    AsmWriter(0x00528F24).nop(2);

    // Default port: 0
    write_mem<u16>(0x0059CDE4, 0);
    write_mem<i32>(0x004B159D + 1, 0); // TODO: add setting in launcher

    // Do not overwrite multi_entity in Single Player
    AsmWriter(0x004A415F).nop(10);

    // Show valid info for servers with incompatible version
    write_mem<u8>(0x0047B3CB, asm_opcodes::jmp_rel_short);

    // Change default Server List sort to players count
    write_mem<u32>(0x00599D20, 4);

    // Buffer Overflow fixes
    for (auto& patch : g_buffer_overflow_patches) {
        patch.install();
    }

    //  Filter packets based on the side (client-side vs server-side)
    process_game_packet_whitelist_filter.install();

    // Hook packet handlers
    process_join_deny_packet_hook.install();
    process_new_player_packet_hook.install();
    process_left_game_packet_hook.install();
    process_chat_line_packet_hook.install();
    process_name_change_packet_hook.install();
    process_team_change_packet_hook.install();
    process_rate_change_packet_hook.install();
    process_entity_create_packet_hook.install();
    process_reload_packet_hook.install();
    process_reload_request_packet_hook.install();
    process_entity_create_packet_injection.install(); // save char if server forces it
    process_entity_create_packet_injection2.install(); // reset char after server forced it

    // Fix obj_update packet handling
    process_obj_update_check_flags_injection.install();

    // Verify on/off weapons handling
    process_obj_update_weapon_fire_injection.install();

    // Client-side green team fix
    using namespace asm_regs;
    AsmWriter(0x0046CAD7, 0x0046CADA).cmp(al, -1);

    // Hide IP addresses in players packet
    AsmWriter(0x00481D31, 0x00481D33).xor_(eax, eax);
    AsmWriter(0x00481D40, 0x00481D44).xor_(edx, edx);
    // Hide IP addresses in new_player packet
    AsmWriter(0x0047A4A0, 0x0047A4A2).xor_(edx, edx);
    AsmWriter(0x0047A4A6, 0x0047A4AA).xor_(ecx, ecx);

    // Fix "Orion bug" - default 'miner1' entity spawning client-side periodically
    multi_alloc_player_id_hook.install();

    // Fix buffer-overflows in multi handle mapping
    multi_get_obj_from_server_handle_hook.install();
    multi_get_obj_handle_from_server_handle_hook.install();
    multi_set_obj_handle_mapping_hook.install();

    // Use server name from game_info packet for netgame name if address matches current server
    process_game_info_packet_hook.install();

    // Fix game_type out of bounds vulnerability in game_info packet
    process_game_info_packet_game_type_bounds_patch.install();

    // Fix shape_index out of bounds vulnerability in boolean packet
    process_boolean_packet_validate_shape_index_patch.install();

    // Fix room_index out of bounds vulnerability in boolean packet
    process_boolean_packet_validate_room_uid_patch.install();

    // Fix shape_index out of bounds vulnerability in pregame_boolean packet
    process_pregame_boolean_packet_validate_shape_index_patch.install();

    // Fix room_index out of bounds vulnerability in pregame_boolean packet
    process_pregame_boolean_packet_validate_room_uid_patch.install();

    // Fix crash if room does not exist in glass_kill packet
    process_glass_kill_packet_check_room_exists_patch.install();

    // Make sure tracker packets come from configured tracker
    net_get_tracker_hook.install();

    // Add Alpine Faction signature to game_info, game_info_req, join_req, join_accept packets
    send_game_info_packet_hook.install();
    send_game_info_req_packet_hook.install();
    send_join_req_packet_hook.install();
    process_join_req_packet_hook.install();
    process_join_req_injection.install();
    send_join_accept_packet_hook.install();
    process_join_accept_injection.install();
    process_join_accept_send_game_info_req_injection.install();
    multi_stop_hook.install();

    bot_shared_secret_cmd.register_cmd();

    // print rcon commands
    process_rcon_packet_log1.install();
    process_rcon_packet_log2.install();
    process_rcon_packet_log3.install();
    rcon_pw_denied_log.install();

    // print join_req denial reasons
    check_access_for_new_player_hook.install();

    // Use port 7755 when hosting a server without 'Force port' option
    multi_start_hook.install();

    // Use UPnP for port forwarding if server is not in LAN-only mode
    tracker_do_broadcast_server_hook.install();

    // Allow changing client and server update rate
    client_update_rate_injection.install();
    server_update_rate_injection.install();
    netfps_cmd.register_cmd();

    // Fix rotation interpolation (Y axis) when it goes from 360 to 0 degrees
    obj_interp_rotation_fix.install();

    // Fix object interpolation playing too fast causing a possible jitter
    obj_interp_too_fast_fix.install();

    // Send trigger_activate packets for late joiners
    send_state_info_injection.install();

    // Send more packets after reliable connection has been established
    send_players_packet_hook.install();

    // Handle infinite ammo reloads
    send_reload_packet_hook.install();

    // Use spawnpoint team property in TeamDM game (PF compatible)
    write_mem<u8>(0x00470395 + 4, 0); // change cmp argument: CTF -> DM
    write_mem<u8>(0x0047039A, asm_opcodes::jz_rel_short);  // invert jump condition: jnz -> jz

    // Preserve password case when processing rcon_request command
    write_mem<i8>(0x0046C85A + 1, 1);

    // Fix reliable sockets being incorrectly assigned to clients server-side if two clients with the same IP address
    // connect at the same time. Unpatched game assigns newly connected reliable sockets to a player with the same
    // IP address that the reliable socket uses. This change ensures that the port is also compared.
    write_mem<i8>(0x0046E8DA + 1, 1);

    // Support custom packet types
    AsmWriter{0x0047916D}.nop(2);
    multi_io_process_packets_injection.install();
    multi_io_stats_add_hook.install();
    process_unreliable_game_packets_hook.install();

    // Fix rejecting reliable packets from non-connected clients
    // Fixes players randomly losing connection to the server when some player sends double left game packets
    // when leaving because of missing level file
    net_rel_work_injection.install();

    // Ignore browsers when calculating player count for info requests
    game_info_num_players_hook.install();

    // Send `pf_player_stats_packet` with score.
    send_netgame_update_packet_hook.install();
}
