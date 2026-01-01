#include "UpdateChecker.h"
#include <common/HttpRequest.h>
#include <shellapi.h>
#include <xlog/xlog.h>

#define BUILDNUM "6" // build number - tick up during release process
// 1 = dev/beta builds
// 2 = 1.0.0 (Maple)
// 3 = 1.1.0 (Tamarack)
// 4 = 1.2.0 (Willow)
// 5 = unused
// 6 = 1.2.1 (Willow)
// 7 = 1.3.0 (TBD)

void UpdateChecker::CheckForUpdates()
{
    // Construct update check URL
    std::string update_url = "https://update.alpinefaction.com/update_check.php?current=" BUILDNUM;
    xlog::info("Checking for updates...");

    // Create an HttpSession
    HttpSession session("Alpine Faction v1.3.0 Update");

    try {
        HttpRequest req(update_url, "GET", session);

        // Set a reasonable timeout for the request
        session.set_connect_timeout(3000);
        session.set_receive_timeout(3000);

        req.send();

        // Read response
        std::string response;
        char buf[128];

        while (true) {
            size_t bytesRead = req.read(buf, sizeof(buf) - 1);
            if (bytesRead == 0)
                break;
            buf[bytesRead] = '\0';
            response += buf;
        }

        // Trim whitespace (just in case)
        response.erase(response.find_last_not_of(" \n\r\t") + 1);

        //xlog::info("Update check response: {}", response);

        // If response is empty or "0", assume no update
        if (response.empty() || response == "0") {
            xlog::info("No update available.");
            return;
        }

        // If response is "1", notify the user
        if (response == "1") {
            std::string update_message =
                "An update for Alpine Faction is available! Click OK to visit the download page.";

            int result =
                MessageBoxA(nullptr, update_message.c_str(), "Update Available", MB_OKCANCEL | MB_ICONINFORMATION);

            if (result == IDOK) {
                // Open update redirect page in default browser
                std::string redirect_url = "https://update.alpinefaction.com/update_redirect.php?current=" BUILDNUM;
                ShellExecuteA(nullptr, "open", redirect_url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
        }
    }
    catch (const std::exception& e) {
        // Log the error and assume no update
        xlog::warn("Update check failed: {}. Assuming no update available.", e.what());
        return;
    }
}
