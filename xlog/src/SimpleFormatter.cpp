#include <xlog/SimpleFormatter.h>
#include <string>
#include <string_view>
#include <windows.h>

std::string xlog::SimpleFormatter::prepare(xlog::Level level, const std::string& logger_name) const
{
    static const uint64_t start_ticks = GetTickCount64();
    std::string buf;
    buf.reserve(64);

    if (include_time_) {
        const float ticks = (GetTickCount64() - start_ticks) / 1000.f;
        std::format_to(std::back_inserter(buf), "[{:7.2f}] ", ticks);
    }

    if (include_level_) {
        static const char* level_prefix[] = {"ERROR: ", "WARN: ", "INFO: ", "DEBUG: ", "TRACE: "};
        buf += level_prefix[static_cast<int>(level)];
    }

    if (include_logger_name_ && !logger_name.empty()) {
        buf += logger_name;
        buf += ' ';
    }

    return buf;
}
