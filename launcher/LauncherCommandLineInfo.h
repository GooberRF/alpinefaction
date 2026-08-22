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
        bool value_expected = false;
        for (unsigned i = 1; i < args.size(); ++i) {
            std::string_view arg = args[i].c_str();
            const bool is_option_value = value_expected;
            value_expected = false;
            if (arg == "-game") {
                m_game = true;
            }
            else if (arg == "-editor") {
                m_editor = true;
            }
            else if (arg == "-help" || arg == "-h") {
                m_help = true;
            }
            else if (arg == "-exe-path" && i + 1 < args.size()) {
                m_exe_path = {args[++i].c_str()};
            }
            else if (arg == "-aflink" && i + 1 < args.size()) {
                Win32xx::CString cstrArg = args[++i];
                std::string narrowArg = std::string(cstrArg);
                ParseAFLink(std::string_view(narrowArg));
            }
            else if (arg == "-play-demo" && i + 1 < args.size()) {
                m_play_demo_arg = {args[++i].c_str()};
            }
            else {
                if (arg == "-level" || arg == "-levelm" || arg == "-awpgen" || arg == "-demo") {
                    has_level_arg = true;
                    value_expected = true;
                }
                else if (arg == "-dedicated" || arg == "-ads") {
                    has_dedicated_arg = true;
                }
                else if (!is_option_value && !m_play_demo_arg.has_value() && IsDemoFilePath(arg)) {
                    // Dropping a .afd on the exe (or "Open with") passes the bare path
                    m_play_demo_arg = std::string{arg};
                    continue;
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
            else if (type == "demo") {
                m_afdemo_arg = value;
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
        return m_aflink_arg.has_value() || m_afdownload_arg.has_value() || m_afdemo_arg.has_value();
    }

    [[nodiscard]] std::optional<std::string> GetAFDemoArg() const
    {
        return m_afdemo_arg;
    }

    [[nodiscard]] std::optional<std::string> GetPlayDemoArg() const
    {
        return m_play_demo_arg;
    }

    // Arrange for the game to launch straight into playback of a downloaded demo.
    void PlayDemoAfterLaunch(const std::string& demo_path)
    {
        m_pass_through_args.emplace_back("-demo");
        m_pass_through_args.push_back(demo_path);
        m_game = true;
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
    static bool IsDemoFilePath(std::string_view arg)
    {
        if (arg.size() < 5 || arg.front() == '-') {
            return false;
        }
        const std::string_view ext = arg.substr(arg.size() - 4);
        return ext[0] == '.' && (ext[1] == 'a' || ext[1] == 'A') && (ext[2] == 'f' || ext[2] == 'F')
            && (ext[3] == 'd' || ext[3] == 'D');
    }

    bool m_game = false;
    bool m_editor = false;
    bool m_help = false;
    std::optional<std::string> m_afdownload_arg;
    std::optional<std::string> m_aflink_arg;
    std::optional<std::string> m_afdemo_arg;
    std::optional<std::string> m_play_demo_arg;
    std::optional<std::string> m_exe_path;
    std::vector<std::string> m_pass_through_args;
};
