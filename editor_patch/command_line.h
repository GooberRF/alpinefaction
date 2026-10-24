#pragma once

#include <optional>
#include <string>

// RED's command line, its switches matched without regard to case. Usable from DLL init.

bool editor_command_line_has(const wchar_t* name);
// The argument after the first `name`; none when `name` is missing or last.
std::optional<std::wstring> editor_command_line_value(const wchar_t* name);
