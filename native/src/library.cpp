#include "library.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <string_view>

std::wstring wide(const char* s) {
    if (!s || !*s) return {};
    int length = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (!length) return L"Erreur inconnue";
    std::wstring result(length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, result.data(), length);
    result.pop_back();
    return result;
}

fs::path Library::settingsFile() {
    PWSTR directory = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &directory)))
        throw std::runtime_error("Impossible de trouver le dossier de configuration Windows.");
    fs::path result = fs::path(directory) / L"Horizon VR" / L"bibliotheque.txt";
    CoTaskMemFree(directory);
    return result;
}

Library::Library(fs::path bookmarksFile) : bookmarksFile_(std::move(bookmarksFile)) {
    std::ifstream input(bookmarksFile_, std::ios::binary);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty()) bookmarks_.emplace_back(wide(line.c_str()));
    }
    open({});
}

namespace {
bool samePath(const fs::path& first, const fs::path& second) {
    return CompareStringOrdinal(first.c_str(), -1, second.c_str(), -1, TRUE) == CSTR_EQUAL;
}

std::wstring lowercaseExtension(const fs::path& path) {
    auto result = path.extension().wstring();
    for (auto& c : result) if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
    return result;
}
}

bool Library::isVideo(const fs::path& path) {
    static const std::wstring_view videoExtensions[] = {
        L".mp4", L".m4v", L".mkv", L".webm", L".mov", L".avi", L".wmv", L".asf", L".flv",
        L".mpeg", L".mpg", L".vob", L".ogv", L".ogg", L".ts", L".m2ts", L".mts", L".mxf",
        L".3gp", L".3g2", L".rm", L".rmvb", L".divx", L".f4v", L".m2v", L".mjpeg",
        L".hevc", L".h264", L".av1"
    };
    return std::find(std::begin(videoExtensions), std::end(videoExtensions), lowercaseExtension(path)) != std::end(videoExtensions);
}

bool Library::isMedia(const fs::path& path) {
    static const std::wstring_view imageExtensions[] = {
        L".jpg", L".jpeg", L".png", L".webp", L".bmp", L".gif", L".tif", L".tiff", L".avif", L".heic", L".jxl"
    };
    return isVideo(path) ||
        std::find(std::begin(imageExtensions), std::end(imageExtensions), lowercaseExtension(path)) != std::end(imageExtensions);
}

void Library::refreshSources() {
    std::vector<LibraryEntry> result;
    const DWORD drives = GetLogicalDrives();
    for (int index = 0; index < 26; ++index) {
        if (!(drives & (1u << index))) continue;
        std::wstring drive{static_cast<wchar_t>(L'A' + index), L':', L'\\'};
        const auto type = GetDriveTypeW(drive.c_str());
        if (type == DRIVE_CDROM) continue;
        std::wstring description = drive;
        wchar_t label[MAX_PATH]{};
        DWORD errorMode = 0;
        if ((type == DRIVE_FIXED || type == DRIVE_REMOVABLE) && SetThreadErrorMode(SEM_FAILCRITICALERRORS, &errorMode)) {
            if (GetVolumeInformationW(drive.c_str(), label, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0) && *label)
                description += L"  " + std::wstring(label);
            SetThreadErrorMode(errorMode, nullptr);
        }
        result.push_back({drive, description, true});
    }
    for (const auto& saved : bookmarks_) {
        auto label = saved.filename().wstring();
        if (label.empty()) label = saved.wstring();
        else if (saved.has_root_name()) label += L"  ·  " + saved.root_name().wstring();
        result.push_back({saved, L"★  " + label, true});
    }
    sources_ = std::move(result);
}

void Library::open(const fs::path& folder) {
    std::vector<LibraryEntry> result;
    if (folder.empty()) refreshSources();
    else {
        std::error_code error;
        fs::directory_iterator iterator(folder, error), end;
        if (error) throw std::system_error(error, "Impossible d’ouvrir ce dossier");
        while (iterator != end) {
            const auto& item = *iterator;
            std::error_code typeError;
            bool directory = item.is_directory(typeError);
            if (directory || (!typeError && isMedia(item.path()) && item.is_regular_file(typeError))) {
                result.push_back({item.path(), item.path().filename().wstring(), directory});
            }
            iterator.increment(error);
            if (error) throw std::system_error(error, "Impossible de parcourir ce dossier");
        }
        std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
            if (a.folder != b.folder) return a.folder;
            return StrCmpLogicalW(a.label.c_str(), b.label.c_str()) < 0;
        });
    }
    entries_ = std::move(result);
    location_ = folder;
}

void Library::up() {
    if (location_.empty() || location_ == location_.root_path()) open({});
    else open(location_.parent_path());
}

bool Library::currentIsBookmarked() const {
    return !location_.empty() && std::any_of(bookmarks_.begin(), bookmarks_.end(), [&](const auto& path) {
        return samePath(path, location_);
    });
}

void Library::toggleBookmark() {
    if (location_.empty()) return;
    auto next = bookmarks_;
    const auto existing = std::find_if(next.begin(), next.end(), [&](const auto& p) {
        return samePath(p, location_);
    });
    if (existing == next.end()) next.push_back(location_);
    else next.erase(existing);

    fs::create_directories(bookmarksFile_.parent_path());
    auto temp = bookmarksFile_;
    temp += L".tmp";
    {
        std::ofstream output(temp, std::ios::binary | std::ios::trunc);
        for (const auto& path : next) output << path.u8string() << '\n';
        output.close();
        if (!output) throw std::runtime_error("Impossible d’enregistrer les dossiers favoris.");
    }
    if (!MoveFileExW(temp.c_str(), bookmarksFile_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Impossible de remplacer le fichier des dossiers favoris.");
    bookmarks_ = std::move(next);
    refreshSources();
}
