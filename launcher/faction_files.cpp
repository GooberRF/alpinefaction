#include "faction_files.h"
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <windows.h>
#include <thread>
#include <chrono>
#include <filesystem>
#include <ios>
#include <xlog/xlog.h>
#include <common/version/version.h>

static const char file_info_base_url[] = "https://autodl.factionfiles.com/aflauncher/v1/fileinfo.php?id=";
static const char file_download_base_url[] = "https://autodl.factionfiles.com/aflauncher/v1/downloadfile.php?ticketid=";

FactionFilesAFLink::FactionFilesAFLink() : session_(AF_USER_AGENT_SUFFIX("Install"))
{
    session_.set_connect_timeout(2000);
    session_.set_receive_timeout(3000);
}

std::optional<FactionFilesAFLink::FileInfo> FactionFilesAFLink::get_file_info(int file_id)
{
    std::string url = std::string(file_info_base_url) + std::to_string(file_id);
    xlog::info("Fetching file info: {}", url);

    HttpRequest req{url, "GET", session_};
    req.send();

    // Read response dynamically into a string
    std::stringstream response_stream;
    char buf[4096];

    while (true) {
        size_t num_bytes_read = req.read(buf, sizeof(buf));
        if (num_bytes_read == 0)
            break;
        response_stream.write(buf, num_bytes_read);
    }

    std::string response = response_stream.str();

    // Handle empty response
    if (response.empty()) {
        xlog::error("Empty response from server for file ID {}", file_id);
        return {};
    }

    // Log the raw server response
    xlog::info("Raw response: {}", response);

    return parse_file_info(response);
}


std::optional<FactionFilesAFLink::FileInfo> FactionFilesAFLink::parse_file_info(const std::string& response)
{
    std::stringstream ss(response);
    ss.exceptions(std::ios::failbit | std::ios::badbit);

    std::string temp;
    std::getline(ss, temp);

    if (temp != "found") {
        xlog::warn("File not found on FactionFiles: {}", response);
        return {};
    }

    FileInfo info;
    std::getline(ss, info.name);
    std::getline(ss, info.author);
    std::getline(ss, info.description);

    std::getline(ss, temp);
    try {
        info.size_in_bytes = static_cast<unsigned>(std::stoul(temp));
    }
    catch (const std::exception& e) {
        xlog::error("Failed to parse file size: {} - {}", temp, e.what());
        return {};
    }

    std::getline(ss, temp);
    try {
        info.file_id = std::stoi(temp);
    }
    catch (const std::exception& e) {
        xlog::error("Failed to parse file ID: {} - {}", temp, e.what());
        return {};
    }

    std::getline(ss, info.file_type);
    if (info.file_type == "mystery") {
        xlog::warn("File was found on FactionFiles but has unknown type: {}", response);
        return {};
    }

    return info;
}


std::string FactionFilesAFLink::get_extraction_path(const std::string& game_exe_path, const std::string& file_type)
{
    std::filesystem::path base_path(game_exe_path);

    if (file_type == "map_mp" || file_type == "map_pack_mp") {
        return (base_path / "user_maps" / "multi").generic_string();
    }
    else if (file_type == "map_sp") {
        return (base_path / "user_maps" / "single").generic_string();
    }
    else if (file_type == "mod_tc") {
        return (base_path / "mods").generic_string();
    }
    else if (file_type == "mod_clientside") {
        return (base_path / "client_mods").generic_string();
    }

    return (base_path / "unknown_files").generic_string(); // Fallback path
}

bool FactionFilesAFLink::extract_zip(const std::string& zip_path, const std::string& extract_to)
{
    xlog::info("Opening ZIP file for extraction: {}", zip_path);

    unzFile archive = unzOpen(zip_path.c_str());
    if (!archive) {
        xlog::error("Failed to open ZIP: {}", zip_path);
        return false;
    }

    // Get ZIP metadata
    unz_global_info global_info;
    if (unzGetGlobalInfo(archive, &global_info) != UNZ_OK) {
        xlog::error("Failed to get ZIP info: {}", zip_path);
        unzClose(archive);
        return false;
    }

    xlog::info("Successfully opened ZIP file: {} with {} files", zip_path, global_info.number_entry);

    char buf[4096];
    char file_name[MAX_PATH];
    unz_file_info file_info;

    for (unsigned long i = 0; i < global_info.number_entry; i++) {
        if (unzGetCurrentFileInfo(archive, &file_info, file_name, sizeof(file_name), nullptr, 0, nullptr, 0) !=
            UNZ_OK) {
            xlog::error("Failed to get ZIP file info");
            break;
        }

        std::string relative_path = file_name; // Preserve directory structure
        std::string output_path = extract_to + "\\" + relative_path;

        if (relative_path.back() == '/' || relative_path.back() == '\\') {
            // If entry is a directory, create it
            std::filesystem::create_directories(output_path);
            xlog::info("Created directory: {}", output_path);
        }
        else {
            // Ensure parent directories exist
            std::filesystem::create_directories(std::filesystem::path(output_path).parent_path());

            // Extract file
            std::ofstream file(output_path, std::ios_base::binary);
            if (!file) {
                xlog::error("Failed to create output file: {}", output_path);
                break;
            }

            if (unzOpenCurrentFile(archive) != UNZ_OK) {
                xlog::error("Failed to open file inside ZIP: {}", relative_path);
                break;
            }

            int bytesRead;
            while ((bytesRead = unzReadCurrentFile(archive, buf, sizeof(buf))) > 0) {
                file.write(buf, bytesRead);
            }

            file.close();
            unzCloseCurrentFile(archive);
            xlog::info("Extracted file: {}", output_path);
        }

        if (i + 1 < global_info.number_entry) {
            if (unzGoToNextFile(archive) != UNZ_OK) {
                xlog::error("Failed to go to next file in ZIP");
                break;
            }
        }
    }

    unzClose(archive);
    xlog::info("Extraction completed to: {}", extract_to);
    return true;
}

bool FactionFilesAFLink::download_and_extract(
    int file_id, std::string file_type, std::function<bool(unsigned bytes_received, std::chrono::milliseconds duration)> progress_callback)
{
    // Load the game executable path from config
    GameConfig gameConfig;
    if (!gameConfig.load()) {
        xlog::error("Failed to load game configuration.");
        return false;
    }

    std::string game_exe_path = gameConfig.game_executable_path.value();
    if (game_exe_path.empty()) {
        xlog::error("Game executable path is empty.");
        return false;
    }

    // Get the base game directory
    std::string game_dir = game_exe_path.substr(0, game_exe_path.find_last_of("\\/"));

    // Determine paths
    std::string extract_to = get_extraction_path(game_dir, file_type);
    std::string zip_path =
        extract_to + "\\" + std::to_string(file_id) + ".zip"; // Download ZIP directly to extraction folder

    xlog::info("Downloading file: {} to {}", file_id, zip_path);

    // Ensure the directory exists before downloading
    std::filesystem::create_directories(extract_to);

    // Create HTTP request session
    HttpRequest req{std::string(file_download_base_url) + std::to_string(file_id), "GET", session_};
    req.send();

    // Open file for writing in the extraction directory
    std::ofstream outFile(zip_path, std::ios_base::binary);
    if (!outFile) {
        xlog::error("Failed to create output file: {}", zip_path);
        MessageBoxA(nullptr, ("Failed to create output file: " + zip_path).c_str(), "Error", MB_OK | MB_ICONERROR);
        return false;
    }

    // Read and write file data
    char buf[4096];
    unsigned total_bytes_read = 0;
    auto download_start = std::chrono::steady_clock::now();

    while (true) {
        size_t num_bytes_read = req.read(buf, sizeof(buf));
        if (num_bytes_read <= 0)
            break;

        outFile.write(buf, num_bytes_read);
        total_bytes_read += num_bytes_read;

        auto duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - download_start);

        if (progress_callback && !progress_callback(total_bytes_read, duration)) {
            xlog::warn("Download aborted by user.");
            outFile.close();
            std::remove(zip_path.c_str()); // Delete incomplete file
            return false;
        }
    }

    // Ensure the file is fully written and closed before extraction
    outFile.flush();
    outFile.close();

    xlog::info("Download complete: {}", zip_path);

    // Extract file in place
    xlog::info("Extracting file in place: {} to {}", zip_path, extract_to);
    bool extraction_success = extract_zip(zip_path, extract_to);

    if (!extraction_success) {
        xlog::error("Extraction failed for {}. Target path: {}", zip_path, extract_to);
        MessageBoxA(nullptr, ("Extraction failed for: " + zip_path).c_str(), "Error", MB_OK | MB_ICONERROR);
        return false;
    }

    // Delete ZIP file after successful extraction
    std::remove(zip_path.c_str());
    xlog::info("Deleted ZIP file after extraction: {}", zip_path);

    xlog::info("Extraction successful to {}", extract_to);
    return true;
}
