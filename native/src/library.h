#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct CameraMesh;
enum class Projection { Flat, Equirect180, Equirect360, Fisheye180, Cubemap, Calibrated };
enum class Stereo { Mono, SideBySide, TopBottom, Calibrated };

struct ViewSettings {
    Projection projection = Projection::Flat;
    Stereo stereo = Stereo::Mono;
    bool swapEyes = false;
    float zoom = 1.f;
    float horizontalFov = 0.f;
    float verticalFov = 180.f;
    float centerYaw = 0.f;
    float centerPitch = 0.f;
    float convergence = 0.f;
    std::shared_ptr<const CameraMesh> mesh;
};

float fieldOfView(const ViewSettings& view);

struct LibraryEntry {
    fs::path path;
    std::wstring label;
    bool folder;
};

class Library {
public:
    explicit Library(fs::path bookmarksFile);
    void open(const fs::path& folder);
    void up();
    void toggleBookmark();
    bool currentIsBookmarked() const;
    void refreshSources();
    const fs::path& location() const { return location_; }
    const std::vector<LibraryEntry>& entries() const { return location_.empty() ? sources_ : entries_; }
    const std::vector<LibraryEntry>& sources() const { return sources_; }
    static bool isMedia(const fs::path& path);
    static bool isVideo(const fs::path& path);
    static fs::path settingsFile();

private:
    fs::path location_;
    fs::path bookmarksFile_;
    std::vector<fs::path> bookmarks_;
    std::vector<LibraryEntry> entries_, sources_;
};

std::wstring wide(const char* s);
