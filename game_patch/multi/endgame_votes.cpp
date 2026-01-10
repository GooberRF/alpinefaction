#include "endgame_votes.h"
#include <xlog/xlog.h>
#include <thread>
#include <common/version/version.h>
#include "../main/main.h"
#include "../misc/misc.h"
#include "../hud/multi_scoreboard.h"
#include "../hud/hud_internal.h"
#include "../rf/gr/gr.h"
#include "../rf/gr/gr_font.h"
#include "../rf/multi.h"
#include "../rf/level.h"
#include "../rf/localize.h"
#include "../rf/gameseq.h"
#include "../rf/hud.h"

bool g_player_can_endgame_vote = false;

void multi_player_set_can_endgame_vote(bool can_vote)
{
    // only enable voting if fflink token is valid
    if (!can_vote || !g_game_config.fflink_token.value().empty()) {
        g_player_can_endgame_vote = can_vote;
    }
}

FactionFilesClient::VoteInfo build_vote_info(bool liked)
{
    FactionFilesClient::VoteInfo vote_info;
    vote_info.fflink_player_token = g_game_config.fflink_token.value();
    vote_info.level_filename = rf::level.filename.c_str();
    vote_info.server_name = rf::netgame.name.c_str();

    if (tc_mod_is_loaded()) {
        vote_info.mod_name = rf::mod_param.get_arg();
    }
    else {
        vote_info.mod_name = "";
    }

    vote_info.vote = liked;

    return vote_info;
}

void async_submit_vote(FactionFilesClient::VoteInfo vote_info)
{
    std::thread([vote_info]() {
        // Do not attempt to vote if token is invalid
        if (vote_info.fflink_player_token.empty()) {
            xlog::warn("Vote submission failed: No valid AFLink token.");
            return;
        }

        // Encode URL parameters
        std::string encoded_token = vote_info.fflink_player_token;
        std::string encoded_level = encode_uri_component(vote_info.level_filename);
        std::string encoded_server = encode_uri_component(vote_info.server_name);
        std::string encoded_mod = encode_uri_component(vote_info.mod_name);
        std::string vote_value = vote_info.vote ? "1" : "-1";

        // Construct the URL
        std::string vote_url = "https://link.factionfiles.com/aflauncher/v1/link_vote.php?token=" + encoded_token +
                               "&rfl=" + encoded_level + "&server=" + encoded_server + "&mod=" + encoded_mod +
                               "&vote=" + vote_value;

        xlog::info("Submitting vote asynchronously... Level: {}, Server: {}, Mod: {}, Positive vote? {}",
                   vote_info.level_filename, vote_info.server_name, vote_info.mod_name, vote_info.vote ? "yes" : "no");

        //xlog::info("Using vote URL: {}", vote_url);

        // Create HTTP session
        HttpSession session(AF_USER_AGENT_SUFFIX("Vote"));

        try {
            session.set_connect_timeout(3000);
            session.set_receive_timeout(3000);

            HttpRequest req(vote_url, "GET", session);
            req.send();

            std::string response;
            char buf[256];
            while (true) {
                size_t bytesRead = req.read(buf, sizeof(buf) - 1);
                if (bytesRead == 0)
                    break;
                buf[bytesRead] = '\0';
                response += buf;
            }

            // Trim whitespace
            response.erase(response.find_last_not_of(" \n\r\t") + 1);

            // Log response
            if (response == "accepted") {
                xlog::info("Vote successfully submitted.");
            }
            else {
                xlog::warn("Vote submission failed: You voted too recently.");
            }
        }
        catch (const std::exception& e) {
            xlog::warn("Vote submission request failed: {}", e.what());
        }
    }).detach();
}

void multi_attempt_endgame_vote(bool liked)
{
    if (g_player_can_endgame_vote) {
        FactionFilesClient::VoteInfo vote_info = build_vote_info(liked);    // build vote information
        async_submit_vote(vote_info);                                       // Submit the vote asynchronously
        multi_player_set_can_endgame_vote(false);                           // Mark as voted
    }
}
