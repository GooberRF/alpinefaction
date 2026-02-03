#pragma once

#include <string>
#include <chrono>
#include <functional>
#include <vector>
#include <common/HttpRequest.h>

class FactionFilesClient
{
public:
    struct LevelInfo
    {
        std::string name;
        std::string author;
        std::string description;
        unsigned size_in_bytes;
        int ticket_id;
    };

    struct VoteInfo
    {
        std::string fflink_player_token;
        std::string level_filename;
        std::string server_name;
        std::string mod_name;
        bool vote;
    };

    FactionFilesClient();
    std::optional<LevelInfo> find_map(const char* file_name);
    std::vector<bool> check_maps(const std::vector<std::string>& file_names);
    void download_map(const char* tmp_filename, int ticket_id,
        std::function<bool(unsigned bytes_received, std::chrono::milliseconds duration)> callback);

private:
    HttpSession session_;

    static std::optional<LevelInfo> parse_level_info(const char* buf);
};
