#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <cstddef>
#include <cwchar>
#include <optional>
#include <string>
#include <vector>
#include "command_line.h"

namespace
{
    const std::vector<std::wstring>& command_line_args()
    {
        static const std::vector<std::wstring> args = [] {
            std::vector<std::wstring> result;
            int argc = 0;
            wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
            for (int i = 1; argv && i < argc; ++i) {
                result.emplace_back(argv[i]);
            }
            LocalFree(argv);
            return result;
        }();
        return args;
    }

    bool is_switch(const std::wstring& arg, const wchar_t* name)
    {
        return _wcsicmp(arg.c_str(), name) == 0;
    }
}

bool editor_command_line_has(const wchar_t* name)
{
    const std::vector<std::wstring>& args = command_line_args();
    return std::any_of(args.begin(), args.end(), [name](const std::wstring& arg) {
        return is_switch(arg, name);
    });
}

std::optional<std::wstring> editor_command_line_value(const wchar_t* name)
{
    const std::vector<std::wstring>& args = command_line_args();
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (is_switch(args[i], name)) {
            return args[i + 1];
        }
    }
    return std::nullopt;
}

std::optional<std::string> editor_command_line_path(const wchar_t* name)
{
    const std::optional<std::wstring> wide = editor_command_line_value(name);
    if (!wide) {
        return std::nullopt;
    }
    int len = WideCharToMultiByte(CP_ACP, 0, wide->c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) {
        return std::string{};
    }
    std::string result(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_ACP, 0, wide->c_str(), -1, result.data(), len, nullptr, nullptr);
    return result;
}
