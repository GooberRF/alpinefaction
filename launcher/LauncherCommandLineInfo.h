#pragma once

#include <wxx_wincore.h>
#include <string_view>
#include <string>
#include <optional>
#include <vector>

class LauncherCommandLineInfo
{
public:
    void Parse()
    {
        auto args = Win32xx::GetCommandLineArgs();
        bool has_level_arg = false;
        bool has_dedicated_arg = false;
        for (unsigned i = 1; i < args.size(); ++i) {
            std::string_view arg = args[i].c_str();
            if (arg == "-game") {
                m_game = true;
            }
            else if (arg == "-editor") {
                m_editor = true;
            }
            else if (arg == "-help" || arg == "-h") {
                m_help = true;
            }
            else if (arg == "-exe-path") {
                m_exe_path = {args[++i].c_str()};
            }
            else if (arg == "-aflink" && i + 1 < args.size()) {
                Win32xx::CString cstrArg = args[++i];
                std::string narrowArg = std::string(cstrArg);
                ParseAFLink(std::string_view(narrowArg));
            }
            else {
                if (arg == "-level" || arg == "-levelm") {
                    has_level_arg = true;
                }
                else if (arg == "-dedicated" || arg == "-ads") {
                    has_dedicated_arg = true;
                }
                m_pass_through_args.emplace_back(arg);
            }
        }
        if (!m_game && !m_editor && (has_level_arg || has_dedicated_arg)) {
            m_game = true;
        }
    }

    void ParseAFLink(std::string_view url)
    {
        if (url.starts_with("af://")) {
            url.remove_prefix(5); // Remove "af://"
        }

        size_t slash_pos = url.find('/');
        if (slash_pos != std::string_view::npos) {
            std::string type = std::string(url.substr(0, slash_pos));
            std::string value = std::string(url.substr(slash_pos + 1));

            if (type == "download") {
                m_afdownload_arg = value;
            }
            else if (type == "link") {
                m_aflink_arg = value;
            }
        }
        else {
            //m_aflink_arg = std::string(url); // Store entire af:// argument if no slash
        }
    }

    [[nodiscard]] bool HasGameFlag() const
    {
        return m_game;
    }

    [[nodiscard]] bool HasEditorFlag() const
    {
        return m_editor;
    }

    [[nodiscard]] bool HasHelpFlag() const
    {
        return m_help;
    }

    [[nodiscard]] bool HasAFFlag() const
    {
        return m_aflink_arg.has_value() || m_afdownload_arg.has_value();
    }

    [[nodiscard]] std::optional<std::string> GetAFLinkArg() const
    {
        return m_aflink_arg;
    }

    [[nodiscard]] std::optional<std::string> GetAFDownloadArg() const
    {
        return m_afdownload_arg;
    }

    [[nodiscard]] std::optional<std::string> GetExePath() const
    {
        return m_exe_path;
    }

    [[nodiscard]] const std::vector<std::string>& GetPassThroughArgs() const
    {
        return m_pass_through_args;
    }

private:
    bool m_game = false;
    bool m_editor = false;
    bool m_help = false;
    std::optional<std::string> m_afdownload_arg;
    std::optional<std::string> m_aflink_arg;
    std::optional<std::string> m_exe_path;
    std::vector<std::string> m_pass_through_args;
};
