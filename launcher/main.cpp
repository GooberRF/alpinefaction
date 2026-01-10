#include "LauncherApp.h"
#include <common/version/version.h>
#include <common/error/error-utils.h>
#include <xlog/xlog.h>
#include <xlog/FileAppender.h>
#include <xlog/Win32Appender.h>
#include <xlog/ConsoleAppender.h>
#include <crash_handler_stub.h>
#include <format>

void InitLogging()
{
    auto app_data_dir = Win32xx::GetAppDataPath();
    auto af_data_dir = app_data_dir + "\\Alpine Faction";
    CreateDirectoryA(af_data_dir, nullptr);
    auto log_file_path = af_data_dir + "\\AlpineFactionLauncher.log";
    xlog::LoggerConfig::get()
        .add_appender<xlog::FileAppender>(log_file_path.GetString(), false)
        .add_appender<xlog::ConsoleAppender>()
        .add_appender<xlog::Win32Appender>();
    xlog::info("Alpine Faction Launcher {} ({}), {} {}", VERSION_STR, VERSION_CODE, __DATE__, __TIME__);
}

void InitCrashHandler()
{
    auto app_data_dir = Win32xx::GetAppDataPath();
    CrashHandlerConfig config;
    std::snprintf(config.log_file, std::size(config.log_file), "%s\\Alpine Faction\\AlpineFactionLauncher.log", app_data_dir.c_str());
    std::snprintf(config.output_dir, std::size(config.output_dir), "%s\\Alpine Faction", app_data_dir.c_str());
    std::snprintf(config.app_name, std::size(config.app_name), "AlpineFactionLauncher");
    config.add_known_module("AlpineFactionLauncher");
    CrashHandlerStubInstall(config);
}

int APIENTRY WinMain(HINSTANCE, HINSTANCE, LPSTR, int) try
{
    InitLogging();
    InitCrashHandler();

    // Start Win32++
    LauncherApp app;

    // Run the application
    return app.Run(); // NOLINT(clang-analyzer-core.StackAddressEscape)
}
// catch all unhandled CException types
catch (const Win32xx::CException &e) {
    // Display the exception and quit
    std::string msg = std::format("{}\nerror {}: {}", e.GetText(), e.GetError(), e.GetErrorString());
    MessageBox(nullptr, msg.c_str(), nullptr, MB_ICONERROR | MB_OK);

    return -1;
}
// catch all unhandled std::exception types
catch (const std::exception& e) {
    std::string msg = std::format("Fatal error: {}", generate_message_for_exception(e));
    MessageBox(nullptr, msg.c_str(), nullptr, MB_ICONERROR | MB_OK);
    return -1;
}
