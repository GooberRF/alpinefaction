#include "FFLinkHelper.h"
#include <common/HttpRequest.h>
#include <common/version/version.h>
#include <random>
#include <sstream>

std::string GenerateLinkToken()
{
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15);

    std::string token;
    token.reserve(32);
    for (int i = 0; i < 32; ++i) {
        char c = dis(gen);
        token += (c < 10) ? ('0' + c) : ('a' + c - 10);
    }
    return token;
}

std::string PollFFLinkAPI(const std::string& token)
{
    std::string url = "https://link.factionfiles.com/aflauncher/v1/link_poll.php?token=" + token;

    HttpSession session("AF/" VERSION_STR);
    session.set_connect_timeout(3000);
    session.set_receive_timeout(3000);

    HttpRequest req(url, "GET", session);
    req.send();

    std::string response;
    char buf[256];
    while (true) {
        size_t bytes_read = req.read(buf, sizeof(buf) - 1);
        if (bytes_read == 0) break;
        buf[bytes_read] = '\0';
        response += buf;
    }

    return response;
}

bool ParseFFLinkResponse(const std::string& response, std::string& username, std::string& token)
{
    std::istringstream ss(response);
    std::string line1;
    std::getline(ss, line1);

    if (line1 != "found") {
        return false;
    }

    std::getline(ss, username);
    std::getline(ss, token);
    return !username.empty() && !token.empty();
}
