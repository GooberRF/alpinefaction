#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <common/payload.h>
#include "../rf/math/vector.h"

namespace rf
{
    struct Event;
    struct Mover;
    struct Player;
}
struct EventCapturePointHandler;
struct HillInfo;

// Wire values of af_payload_state
enum class PayloadCartState : uint8_t
{
    idle = 0,
    pushing = 1,
    blocked = 2,
    rolling_back = 3,
    finale = 4,
    held = 5, // stays put after a checkpoint capture, whoever is in the push zone
};

struct PayloadCheckpoint
{
    int keyframe = -1;
    float distance = 0.0f; // along the track
    rf::Vector3 pos{};     // the keyframe's position
    EventCapturePointHandler* handler = nullptr; // a Payload_Checkpoint
    float hold_seconds = 0.0f;
    float time_bonus = 0.0f; // the checkpoint's or the level's; a server's pay_checkpoint_time_bonus wins over it
};

// Where the cart is along its mover path: `fraction` of the way by distance from keyframe `segment` to the next
struct PayloadCartPlacement
{
    int segment = 0;
    float fraction = 0.0f;
    float speed = 0.0f; // signed, m/s
};

// What af_payload_state carries
struct PayloadNetState
{
    uint8_t sequence = 0;
    PayloadCartState state = PayloadCartState::idle;
    bool overtime = false;
    bool heals = false;
    bool setup = false; // the round's start, until the cart is first free to move
    uint8_t heal_serial = 0;
    uint8_t next_checkpoint = 0;
    std::optional<float> countdown_seconds;
    float distance = 0.0f;
    float speed = 0.0f;
    std::vector<uint8_t> pushers;
    std::vector<uint8_t> blockers;
};

// ─── Read-only state, the same on the server and clients ─────────────────────

bool payload_active();
int payload_attacking_team(); // 0 red, 1 blue
int payload_defending_team();
rf::Mover* payload_cart_mover();
std::optional<rf::Vector3> payload_cart_center();
// The Payload_Handler's name, or the default one
const std::string& payload_cart_name();
float payload_push_radius();
// Half the height of the cart's bounds, so its centre less this is the floor it rides on
float payload_cart_half_height();
// The level allows it and the server heals a nonzero amount
bool payload_heals();
// It heals and the setup is over
bool payload_heals_now();
// Changes on each server heal tick with a live pusher, or nullopt until a client has the server's state
std::optional<uint8_t> payload_heal_serial();
float payload_distance();
float payload_total_length();
PayloadCartState payload_state();
// Blocked or rolling back
bool payload_defenders_have_cart();
// The cart's held at the round's start, or a client hasn't heard otherwise yet
bool payload_in_setup();
bool payload_in_overtime();
// Held: seconds until the hold ends. Idle: seconds until the cart starts rolling back. Else nullopt.
std::optional<float> payload_countdown_seconds();
// Rounded up and never below 1, as a client's countdown reaches 0 before the server's state change does
std::optional<int> payload_countdown_whole_seconds();
// In track order. The last may sit at the end of the track.
const std::vector<PayloadCheckpoint>& payload_checkpoints();
// The first checkpoint not yet captured, or the checkpoint count once all are
int payload_next_checkpoint_index();
// 0-1 from the last captured checkpoint, or the track's start, to the next one, or to the track's end after the last
float payload_next_checkpoint_progress();
bool payload_checkpoint_captured(const PayloadCheckpoint& checkpoint);
// The world position of a payload checkpoint hill (it has no trigger), or nullopt for any other hill
std::optional<rf::Vector3> payload_checkpoint_pos(const HillInfo& hill);
// The server's lists: attackers and defenders in the push zone
const std::vector<uint8_t>& payload_pusher_ids();
// The pushers up to the most that speed the cart up
int payload_useful_pushers();
const std::vector<uint8_t>& payload_blocker_ids();
// Where the cart's centre is at `distance` along the track
rf::Vector3 payload_track_point(float distance);
// The attackers once the cart reaches the end, else the defenders; -1 outside Payload or with no usable cart
int payload_winning_team();

// ─── Game type glue ──────────────────────────────────────────────────────────

void payload_level_init();
void payload_level_init_post();
// Takes back this level's checkpoint time bonuses, which rf::multi_time_limit would otherwise carry to the next
void payload_level_close();
void payload_do_frame();
// Server: whether the round is over now. Starts payload overtime when time runs out on a pushed cart.
bool payload_round_over(bool time_up);
bool payload_finale_reached();
bool payload_player_in_push_zone(const rf::Player& player);
// In the push zone, or among the last pushers of a cart that coasted onto a checkpoint
bool payload_player_credited_for_capture(const rf::Player& player);
// The Payload_Checkpoint events of the checkpoints, in track order
std::vector<rf::Event*> payload_checkpoint_handlers();
std::optional<PayloadCartPlacement> payload_cart_placement(const rf::Mover* mp);
bool payload_is_cart(int mover_handle);
void payload_force_state_sync_to(rf::Player* player);
// The listen-server host joins no game, so it gets the joiners' greeting here
void payload_greet_listen_host();
PayloadNetState payload_build_net_state();
void payload_apply_net_state(const PayloadNetState& net);
// The level's rules, with the server's overrides on a server
payload::LevelRules payload_rules();
