#pragma once

#include <string>

// Generate a random 32-character hexadecimal token for FFLink
std::string GenerateLinkToken();

// Poll the FactionFiles API with the given token
// Returns the raw response string from the API
std::string PollFFLinkAPI(const std::string& token);

// Parse the FFLink API response
// Returns true if successful and populates username and token parameters
// Returns false if the response indicates no link was found
bool ParseFFLinkResponse(const std::string& response, std::string& username, std::string& token);
