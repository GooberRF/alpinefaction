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
