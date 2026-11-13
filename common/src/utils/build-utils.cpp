#include <common/utils/build-utils.h>
#include <functional>
#include <format>
#include <string>
#include <cstdio>

const std::string& get_build_date() {
    static std::string res = std::invoke([] {
        std::string date = __DATE__;
        // Find and erase double space.
        const size_t pos = date.find("  ");
        if (pos != std::string::npos) {
            date.erase(pos, 1);
        }
        return date;
    });
    return res;
}

const std::string& get_build_time() {
    static std::string res = std::invoke([] {
        int hours = 0, minutes = 0, seconds = 0;
        std::sscanf(__TIME__, "%d:%d:%d", &hours, &minutes, &seconds);
        const std::string_view suffix = (hours < 12) ? "AM" : "PM";
        // Use a zero-indexed 12-hour clock for elegance.
        return std::format("{}:{:02} {}", hours % 12, minutes, suffix);
    });
    return res;
}
