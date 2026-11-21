#include "vr.h"
#include "vr-ui.h"
#include "video-format.h"
#include "stereo-probe.h"
#include "thumbnails.h"
#include "ui-theme.h"
#include <commctrl.h>
#include <dbt.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <sstream>
#include <thread>
#include <windowsx.h>

namespace {
using namespace UiTheme;
constexpr wchar_t windowClass[] = L"LecteurVideoVR";
constexpr wchar_t previewClass[] = L"LecteurVideoVRApercu";
constexpr wchar_t gridClass[] = L"LecteurVideoVRGrille";
constexpr wchar_t appName[] = L"Lecteur vidéo VR";
// Windows 11 caption attributes, also used when building against an older SDK.
constexpr DWORD dwmDarkMode = 20, dwmBorderColor = 34, dwmCaptionColor = 35, dwmTextColor = 36;
constexpr int projectionCount = 5;
constexpr int stereoCount = 3;
constexpr int shortcutsInLibrary = 6;

enum class VrPage { Hidden, Library, Playback, Settings, Timeline, Format, Projection, Stereo };
enum class VrAction {
    Drives, Parent, Library, Playback, Settings, Source, Entry, Favorite, Auto, Format, Projection, Stereo, SwapEyes,
    FormatBack, ProjectionChoice, StereoChoice, Preset,
    Narrow, Widen, DepthDown, DepthUp, Play, Timeline, Rewind, Advance, PreviousFile, NextFile, Center,
    Mute, Quieter, Louder, Resume, Quit,
};

struct VrRow {
    std::wstring label;
    VrAction action;
    int entry = -1;
    bool active = false;
};

struct VrColumn {
    std::vector<VrRow> rows;
    int first = 0, count = 0, selected = 0;
    int highlighted = -1;
};

void drawLibraryCard(HDC dc, RECT card, const LibraryEntry& entry, bool focused,
                     const std::shared_ptr<const Thumbnail>& image, int labelHeight) {
    tile(dc, card, focused);
    RECT frame{card.left + 6, card.top + 6, card.right - 6, card.bottom - labelHeight};
    fill(dc, frame, background);
    if (image) {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = Thumbnail::width;
        info.bmiHeader.biHeight = -Thumbnail::height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        const LONG width = frame.right - frame.left, height = frame.bottom - frame.top;
        const LONG w = std::min(width, height * Thumbnail::width / Thumbnail::height);
        const LONG h = w * Thumbnail::height / Thumbnail::width;
        StretchDIBits(dc, frame.left + (width - w) / 2, frame.top + (height - h) / 2, w, h,
            0, 0, Thumbnail::width, Thumbnail::height, image->pixels.data(), &info, DIB_RGB_COLORS, SRCCOPY);
    } else {
        const LONG x = (frame.left + frame.right) / 2, y = (frame.top + frame.bottom) / 2;
        if (entry.folder) {
            fill(dc, {x - 15, y - 10, x - 3, y - 5}, violet);
            outline(dc, {x - 15, y - 6, x + 16, y + 12}, violet);
            fill(dc, {x - 11, y - 2, x + 12, y + 8}, raised);
        } else {
            outline(dc, {x - 15, y - 10, x + 16, y + 11}, edge);
            const POINT play[]{{x - 4, y - 5}, {x + 6, y}, {x - 4, y + 5}};
            const POINT photo[]{{x - 10, y + 6}, {x - 2, y - 4}, {x + 11, y + 6}};
            const auto oldBrush = SelectObject(dc, GetStockObject(DC_BRUSH));
            const auto oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
            SetDCBrushColor(dc, violet);
            Polygon(dc, Library::isVideo(entry.path) ? play : photo, 3);
            SelectObject(dc, oldPen);
            SelectObject(dc, oldBrush);
        }
    }
    RECT label{card.left + 8, frame.bottom, card.right - 6, card.bottom};
    DrawTextW(dc, entry.label.c_str(), -1, &label, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
}

class TestBookmarksFile {
public:
    TestBookmarksFile() {
        wchar_t temp[MAX_PATH]{}, file[MAX_PATH]{};
        const auto length = GetTempPathW(MAX_PATH, temp);
        if (!length || length >= MAX_PATH || !GetTempFileNameW(temp, L"lvr", 0, file))
            throw std::runtime_error("Impossible d’isoler le test des favoris.");
        path = file;
    }
    ~TestBookmarksFile() {
        std::error_code error;
        fs::remove(path, error);
        const fs::path state(path.wstring() + L".positions");
        fs::directory_iterator first(state, error), end;
        for (; first != end && !error; first.increment(error)) {
            std::error_code ignored;
            fs::remove(first->path(), ignored);
        }
        fs::remove(state, error);
    }
    fs::path path;
};

enum Id {
    OpenFile = 100, OpenFolder, Bookmark, Up, Home, Entries, ProjectionMode, StereoMode,
    SwapEyes, ZoomDown, ZoomUp, Center, Play, Back, Forward, Mute, EnterVr, Seek, Volume,
    Narrow, Widen, DepthDown, DepthUp, ListView, GridView, QuitPlayer,
};

std::wstring timeLabel(double seconds) {
    const auto time = static_cast<int>(std::max(0., seconds));
    wchar_t value[32]{};
    if (time >= 3600) swprintf_s(value, L"%d:%02d:%02d", time / 3600, time / 60 % 60, time % 60);
    else swprintf_s(value, L"%d:%02d", time / 60, time % 60);
    return value;
}

std::wstring pathName(const fs::path& path) {
    return path.empty() ? L"Disques et dossiers favoris" : path.wstring();
}

void writeDiagnostic(const std::wstring& message) {
    // The diagnostic modes run as a console program, without a window or capture.
    FILE* stream = stdout;
    if (!GetStdHandle(STD_OUTPUT_HANDLE) || GetStdHandle(STD_OUTPUT_HANDLE) == INVALID_HANDLE_VALUE) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) freopen_s(&stream, "CONOUT$", "w", stdout);
    }
    if (stream) {
        const auto utf8 = fs::path(message).u8string();
        std::fputs(utf8.c_str(), stream);
        std::fputc('\n', stream);
        std::fflush(stream);
    }
}

class MenuTexture {
public:
    explicit MenuTexture(ID3D11Device* device) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = VrUi::width;
        desc.Height = VrUi::height;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        checked(device->CreateTexture2D(&desc, nullptr, &texture_), "Texture de la bibliothèque");
        checked(device->CreateShaderResourceView(texture_.Get(), nullptr, &resource_), "Vue de la bibliothèque");
        device->GetImmediateContext(&context_);
    }

    ID3D11ShaderResourceView* get() const { return resource_.Get(); }

    struct AudioControls {
        int volume;
        bool muted;
        bool dragging;
        bool muteFocused;
    };

    struct LibraryGrid {
        const std::vector<LibraryEntry>& entries;
        const Thumbnails& thumbnails;
        bool grid;
        int first;
        int button;
    };

    void update(const std::wstring& title, const std::wstring& subtitle, const VrColumn& files,
                const VrColumn* sources, double progress, bool dragging, bool playback,
                std::optional<AudioControls> audio, std::optional<LibraryGrid> libraryGrid, bool quitFocused) {
        const bool compact = audio.has_value();
        HDC dc = CreateCompatibleDC(nullptr);
        if (!dc) throw std::runtime_error("Création du menu VR impossible.");
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = VrUi::width;
        info.bmiHeader.biHeight = -VrUi::height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* pixels = nullptr;
        HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!bitmap) { DeleteDC(dc); throw std::runtime_error("Image du menu VR introuvable."); }
        auto oldBitmap = SelectObject(dc, bitmap);
        auto bodyFont = font(-26, FW_NORMAL, L"Segoe UI");
        auto titleFont = font(compact ? -30 : -34, FW_SEMIBOLD);
        auto captionFont = font(-22, FW_MEDIUM);
        auto oldFont = SelectObject(dc, bodyFont);
        SetBkMode(dc, TRANSPARENT);
        const int bottom = compact ? VrUi::timelineHeight : VrUi::height;
        fill(dc, {0, 0, VrUi::width, VrUi::height}, background);
        gradient(dc, {0, 0, VrUi::width, bottom}, raised, background);
        outline(dc, {0, 0, VrUi::width, bottom}, violet);
        rule(dc, 0, VrUi::width, 0, 3);
        const auto line = [&](const std::wstring& text, int top, COLORREF color, int left = 32, int right = VrUi::width - 28) {
            RECT bounds{left, top, right, top + 38};
            SetTextColor(dc, color);
            DrawTextW(dc, text.c_str(), -1, &bounds, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX | DT_VCENTER);
        };
        const auto drawTrack = [&](int x1, int x2, int y, double fraction, bool active, COLORREF color) {
            const int handle = x1 + static_cast<int>((x2 - x1) * fraction);
            fill(dc, {x1, y - 4, x2, y + 4}, track);
            fill(dc, {x1, y - 2, handle, y + 2}, color);
            const int radius = active ? 9 : 6;
            fill(dc, {handle - radius, y - radius, handle + radius, y + radius}, color);
            fill(dc, {handle - 2, y - radius + 2, handle + 2, y + radius - 2}, textColor);
        };
        const auto drawQuit = [&](VrUi::Surface surface) {
            const auto bounds = VrUi::quitButtonRect(surface);
            tile(dc, bounds, quitFocused);
            drawQuitIcon(dc, bounds.left + 12, (bounds.top + bounds.bottom) / 2 - 9, quitFocused);
            auto label = bounds;
            label.left += 39;
            DrawTextW(dc, L"Quitter", -1, &label, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        };
        const auto draw = [&](const VrColumn& column, int left, int right) {
            for (int i = 0; i < static_cast<int>(column.rows.size()); ++i) {
                const auto& row = column.rows[i];
                const int y = VrUi::firstY + i * VrUi::rowStep;
                const bool highlighted = column.first + i == column.highlighted;
                tile(dc, {left, y, right, y + VrUi::rowHeight}, highlighted, row.active);
                if (row.action == VrAction::Timeline) {
                    const int start = left + 18, end = right - 18;
                    drawTrack(start, end, y + 32, progress, dragging, accent);
                    SelectObject(dc, captionFont);
                    line(row.label, y - 4, textColor, start, end);
                    SelectObject(dc, bodyFont);
                } else {
                    if (row.action == VrAction::Quit) drawQuitIcon(dc, left + 14, y + 11, highlighted);
                    line(row.label, y, textColor, left + (row.action == VrAction::Quit ? 43 : 16), right - 12);
                }
            }
        };
        if (audio) {
            const int left = VrUi::timelineTrackStart, right = VrUi::width - left;
            SelectObject(dc, titleFont);
            line(title, 16, textColor, left);
            RECT time{left, 16, right, 54};
            SetTextColor(dc, accent);
            DrawTextW(dc, subtitle.c_str(), -1, &time, DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            SelectObject(dc, bodyFont);
            drawTrack(left, right, VrUi::timelineTrackY, progress, dragging, accent);
            rule(dc, left, right, 119);
            line(L"Volume " + std::to_wstring(audio->volume) + L" %", VrUi::volumeTrackY - 19,
                textColor, left, VrUi::volumeTrackStart - 16);
            drawTrack(VrUi::volumeTrackStart, VrUi::volumeTrackEnd, VrUi::volumeTrackY,
                audio->volume / 100., audio->dragging, violet);
            SelectObject(dc, captionFont);
            tile(dc, {VrUi::muteLeft, VrUi::muteTop, VrUi::muteRight, VrUi::muteBottom},
                audio->muteFocused, audio->muted);
            line(audio->muted ? L"Rétablir le son" : L"Sourdine", VrUi::volumeTrackY - 19,
                textColor, VrUi::muteLeft + 12, VrUi::muteRight - 8);
            drawQuit(VrUi::Surface::Timeline);
        } else {
            SelectObject(dc, titleFont);
            line(title, 14, textColor);
            rule(dc, VrUi::margin, VrUi::width - VrUi::margin, 57);
            SelectObject(dc, bodyFont);
            const int filesStart = sources ? VrUi::filesLeft : VrUi::margin;
            if (sources) {
                fill(dc, {VrUi::margin, 103, VrUi::sourcesRight, 522}, background);
                line(L"Disques et favoris", 60, accent, VrUi::margin + 12, VrUi::sourcesRight - 10);
                draw(*sources, VrUi::margin, VrUi::sourcesRight);
            }
            line(subtitle, 60, muted, filesStart + 12);
            if (libraryGrid) {
                SelectObject(dc, captionFont);
                for (const bool grid : {false, true}) {
                    auto button = VrUi::viewButtonRect(grid);
                    const bool active = grid == libraryGrid->grid;
                    const bool hovered = libraryGrid->button == static_cast<int>(grid);
                    tile(dc, button, hovered, active);
                    drawViewIcon(dc, button.left + 10, button.top + 10, grid, active || hovered ? accent : violet);
                    button.left += 30;
                    DrawTextW(dc, grid ? L"Grille" : L"Liste", -1, &button, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
                }
                if (libraryGrid->grid) {
                    for (int i = 0; i < static_cast<int>(files.rows.size()); ++i) {
                        const auto& row = files.rows[i];
                        const auto bounds = VrUi::shortcutRect(i);
                        tile(dc, bounds, i == files.highlighted);
                        auto label = bounds;
                        label.left += 9;
                        label.right -= 4;
                        DrawTextW(dc, row.label.c_str(), -1, &label,
                            DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
                    }
                    const int end = std::min(static_cast<int>(libraryGrid->entries.size()),
                        libraryGrid->first + VrUi::gridColumns * VrUi::gridRows);
                    for (int i = libraryGrid->first; i < end; ++i) {
                        drawLibraryCard(dc, VrUi::cardRect(i - libraryGrid->first), libraryGrid->entries[i],
                            i + shortcutsInLibrary == files.highlighted, libraryGrid->thumbnails.get(i), 28);
                    }
                    if (end > libraryGrid->first) {
                        RECT count{VrUi::filesLeft, 545, VrUi::quitButtonRect(VrUi::Surface::Full).left - 12, 585};
                        const auto label = std::to_wstring(libraryGrid->first + 1) + L"–" +
                            std::to_wstring(end) + L" / " + std::to_wstring(libraryGrid->entries.size());
                        SetTextColor(dc, muted);
                        DrawTextW(dc, label.c_str(), -1, &count, DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
                    }
                }
                SelectObject(dc, bodyFont);
            }
            if (!libraryGrid || !libraryGrid->grid) draw(files, filesStart, VrUi::width - VrUi::margin);
            rule(dc, VrUi::margin, VrUi::width - VrUi::margin, 536);
            SelectObject(dc, captionFont);
            const bool showQuit = sources || playback;
            const int hintRight = showQuit ? VrUi::quitButtonRect(VrUi::Surface::Full).left - 12
                : VrUi::width - VrUi::margin;
            line(dragging ? L"Relâche la gâchette pour reprendre à cet endroit"
                : playback ? L"Gâchette maintenue : déplacer le curseur   ·   B : retour"
                : L"Pointe et confirme avec la gâchette ou A     B : retour", 547, accent, 32, hintRight);
            if (showQuit) drawQuit(VrUi::Surface::Full);
        }
        GdiFlush();
        context_->UpdateSubresource(texture_.Get(), 0, nullptr, pixels, VrUi::width * 4, 0);
        SelectObject(dc, oldFont);
        SelectObject(dc, oldBitmap);
        DeleteObject(bodyFont);
        DeleteObject(titleFont);
        DeleteObject(captionFont);
        DeleteObject(bitmap);
        DeleteDC(dc);
    }

private:
    ComPtr<ID3D11Texture2D> texture_;
    ComPtr<ID3D11ShaderResourceView> resource_;
    ComPtr<ID3D11DeviceContext> context_;
};

class Player {
public:
    explicit Player(bool silent = false, fs::path bookmarksFile = Library::settingsFile()) : library_(bookmarksFile) {
        try { vr_ = std::make_unique<Vr>(); }
        catch (const std::exception& error) { vrError_ = wide(error.what()); }
        renderer_ = std::make_unique<Renderer>(vr_ ? &vr_->adapter() : nullptr);
        resumeDirectory_ = silent ? fs::path(bookmarksFile.wstring() + L".positions")
            : bookmarksFile.parent_path() / L"positions";
        media_ = std::make_unique<Media>(renderer_->device(), silent, resumeDirectory_);
        menuTexture_ = std::make_unique<MenuTexture>(renderer_->device());
    }

    ~Player() {
        if (vr_) vr_->close();
        if (font_) DeleteObject(font_);
        if (buttonFont_) DeleteObject(buttonFont_);
        if (headingFont_) DeleteObject(headingFont_);
        if (brush_) DeleteObject(brush_);
        if (panelBrush_) DeleteObject(panelBrush_);
    }

    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM w, LPARAM l) {
        if (message == WM_NCCREATE) {
            auto* player = reinterpret_cast<Player*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(player));
            player->window_ = window;
        }
        auto* player = reinterpret_cast<Player*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (!player) return DefWindowProcW(window, message, w, l);
        try { return player->message(message, w, l); }
        catch (const std::exception& error) {
            if (message == WM_CREATE) {
                player->startupError_ = wide(error.what());
                return -1;
            }
            player->show(wide(error.what()));
            return 0;
        }
    }

    void run(HINSTANCE instance, int showCommand, const fs::path& initial, bool test = false, bool startInVr = true) {
        INITCOMMONCONTROLSEX common{sizeof(common), ICC_BAR_CLASSES};
        InitCommonControlsEx(&common);
        WNDCLASSEXW window{sizeof(window)};
        window.hInstance = instance;
        window.lpfnWndProc = windowProc;
        window.lpszClassName = windowClass;
        window.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        if (!RegisterClassExW(&window)) throw std::runtime_error("Classe de la fenêtre introuvable.");
        window.lpfnWndProc = [](HWND preview, UINT message, WPARAM w, LPARAM l) -> LRESULT {
            auto* player = reinterpret_cast<Player*>(GetWindowLongPtrW(GetParent(preview), GWLP_USERDATA));
            if (player && message == WM_LBUTTONDOWN) {
                SetCapture(preview);
                player->drag_ = POINT{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
                return 0;
            }
            if (player && message == WM_LBUTTONUP) { ReleaseCapture(); return 0; }
            if (player && message == WM_MOUSEMOVE && GetCapture() == preview) {
                POINT position{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
                if (GET_KEYSTATE_WPARAM(w) & MK_SHIFT) player->roll_ += (position.x - player->drag_.x) * .006f;
                else {
                    player->yaw_ -= (position.x - player->drag_.x) * .006f;
                    player->pitch_ = std::clamp(player->pitch_ - (position.y - player->drag_.y) * .006f, -1.55f, 1.55f);
                }
                player->drag_ = position;
                return 0;
            }
            if (player && message == WM_MOUSEWHEEL) {
                player->zoom(GET_WHEEL_DELTA_WPARAM(w) > 0 ? 1.10f : .91f);
                return 0;
            }
            if (message == WM_ERASEBKGND) return 1;
            return DefWindowProcW(preview, message, w, l);
        };
        window.lpszClassName = previewClass;
        window.hIcon = nullptr;
        if (!RegisterClassExW(&window)) throw std::runtime_error("Classe de l’aperçu vidéo introuvable.");
        window.style = CS_DBLCLKS;
        window.lpszClassName = gridClass;
        window.lpfnWndProc = [](HWND grid, UINT message, WPARAM w, LPARAM l) -> LRESULT {
            auto* player = reinterpret_cast<Player*>(GetWindowLongPtrW(GetParent(grid), GWLP_USERDATA));
            try {
                if (player && player->grid_ == grid) return player->gridMessage(message, w, l);
            } catch (const std::exception& error) {
                player->show(wide(error.what()));
                return 0;
            }
            return DefWindowProcW(grid, message, w, l);
        };
        if (!RegisterClassExW(&window)) throw std::runtime_error("Classe de la grille vidéo introuvable.");
        if (!CreateWindowExW(0, windowClass, appName, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                CW_USEDEFAULT, CW_USEDEFAULT, 1160, 760, nullptr, nullptr, instance, this))
            throw std::runtime_error(startupError_.empty() ? "Création de la fenêtre du lecteur impossible." : fs::path(startupError_).u8string());
        RECT area{};
        GetClientRect(window_, &area);
        layout(area.right, area.bottom);
        if (test) {
            if (library_.entries().empty()) throw std::runtime_error("Aucun lecteur détecté dans la bibliothèque.");
            VrUi::ConfirmPress press;
            const auto rayTo = [](float x, int row) {
                const float y = (VrUi::firstY + row * VrUi::rowStep + VrUi::rowHeight / 2.f) / VrUi::height;
                return VrUi::aim(XMVectorSet((x - .5f) * VrUi::panelWidth, (.5f - y) * VrUi::panelHeight, 2, 0),
                    XMVectorSet(0, 0, -1, 0));
            };
            setPage(VrPage::Library);
            if (!vrMenu() || displayedSources_.rows.empty()) throw std::runtime_error("Les disques sont absents du panneau gauche.");
            const auto root = library_.sources()[0].path;
            const auto diskPointer = rayTo(.16f, 0);
            vrPoint({diskPointer});
            if (pointedRow_ != 0 || !pointedSource_) throw std::runtime_error("Le disque pointé n’est pas surligné.");
            vrPoint({diskPointer, press.update(true, false)});
            if (library_.location() != root) throw std::runtime_error("Le bouton A n’a pas confirmé le disque pointé.");
            if (press.update(true, true) || press.update(false, true) || press.update(false, false))
                throw std::runtime_error("Deux boutons ont provoqué une confirmation supplémentaire.");
            open(initial.parent_path());
            const int favorite = shortcutsInLibrary - 1;
            selected_ = favorite;
            vrMenu();
            const auto favoritePointer = [&] {
                return rayTo(.66f, favorite - displayedFiles_.first);
            };
            vrPoint({favoritePointer(), true, VrUi::Trigger::Pressed});
            if (!library_.currentIsBookmarked()) throw std::runtime_error("Le bouton Favoris de la bibliothèque a échoué.");
            vrMenu();
            if (std::none_of(displayedSources_.rows.begin(), displayedSources_.rows.end(), [](const auto& row) {
                    return row.label.rfind(L"★", 0) == 0;
                })) throw std::runtime_error("Le dossier favori n’apparaît pas dans la colonne de gauche.");
            vrPoint({favoritePointer(), true});
            if (library_.currentIsBookmarked()) throw std::runtime_error("Le retrait du dossier favori a échoué.");
            const auto& entries = library_.entries();
            const auto found = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) { return entry.path == initial; });
            if (found == entries.end()) throw std::runtime_error("Le média de test est absent de la bibliothèque.");
            const int index = shortcutsInLibrary + static_cast<int>(found - entries.begin());
            selected_ = index;
            if (!vrMenu()) throw std::runtime_error("La texture du menu VR est absente.");
            const auto pointAt = [](RECT area, VrUi::Surface surface = VrUi::Surface::Full) {
                return XMFLOAT2{(area.left + area.right) / (2.f * VrUi::width),
                    (area.top + area.bottom) / (2.f * VrUi::surfacePixels(surface))};
            };
            vrPoint({pointAt(VrUi::viewButtonRect(true)), true});
            if (!gridView_ || !vrMenu() || !displayedGrid_)
                throw std::runtime_error("Le bouton Grille n’a pas changé l’affichage.");
            const int entry = index - shortcutsInLibrary;
            vrPoint({pointAt(VrUi::cardRect(entry - displayedFirstCard_))});
            if (pointedRow_ != index) throw std::runtime_error("La carte VR ne pointe pas vers le bon fichier.");
            SendMessageW(grid_, WM_KEYDOWN, VK_HOME, 0);
            if (gridSelection_ != 0) throw std::runtime_error("Le clavier ne sélectionne pas la première carte Windows.");
            if (thumbnails_.get(entry)) throw std::runtime_error("La grille a attendu la miniature avant de s’afficher.");
            const auto waitForThumbnail = [&](int target) {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
                while (!thumbnails_.get(target) && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(15));
                return thumbnails_.get(target);
            };
            if (Library::isVideo(initial)) {
                updateThumbnails(true, false);
                if (!waitForThumbnail(entry)) throw std::runtime_error("La miniature visible n’a pas été générée.");
                const int last = displayedFirstCard_ + VrUi::gridColumns * VrUi::gridRows;
                for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
                    if ((i < displayedFirstCard_ || i >= last) && thumbnails_.get(i))
                        throw std::runtime_error("Une miniature hors de la grille a été calculée.");
                }
            }
            if (entries.size() >= 24) {
                selected_ = shortcutsInLibrary;
                sourceFocus_ = false;
                menuDirty_ = true;
                vrMenu();
                vrPoint({diskPointer});
                vrCommand(VrCommand::FocusFiles);
                for (int i = 0; i < VrUi::gridRows; ++i) {
                    vrPoint({diskPointer});
                    vrCommand(VrCommand::Next);
                    vrMenu();
                }
                if (displayedFirstCard_ != VrUi::gridColumns)
                    throw std::runtime_error("Le joystick n’a pas fait défiler la grille VR.");

                RECT bounds{};
                GetWindowRect(grid_, &bounds);
                MSG wheel{item(GridView), WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA),
                    MAKELPARAM(bounds.left + 12, bounds.top + 12)};
                dispatch(wheel);
                if (gridScroll_ != desktopCardHeight)
                    throw std::runtime_error("La molette n’a pas fait défiler la grille Windows.");
                SendMessageW(grid_, WM_VSCROLL, SB_BOTTOM, 0);
                const int last = static_cast<int>(entries.size()) - 1;
                updateThumbnails(false, true);
                if (Library::isVideo(entries[last].path) && !waitForThumbnail(last))
                    throw std::runtime_error("Une miniature en bas de la grille Windows est absente.");
                selected_ = index;
            }
            vrPoint({pointAt(VrUi::viewButtonRect(false)), true});
            if (gridView_ || !vrMenu() || displayedGrid_)
                throw std::runtime_error("Le bouton Liste n’a pas rétabli l’affichage.");
            if (entries.size() >= 24) {
                const auto list = item(Entries);
                SendMessageW(list, LB_SETTOPINDEX, 12, 0);
                const auto top = static_cast<int>(SendMessageW(list, LB_GETTOPINDEX, 0, 0));
                command(GridView);
                RECT bounds{};
                GetClientRect(grid_, &bounds);
                const auto [first, end] = desktopVisible(bounds.bottom);
                if (top < first || top >= end)
                    throw std::runtime_error("Le passage de la liste à la grille a perdu la position.");
                updateThumbnails(false, true);
                if (Library::isVideo(entries[top].path) && !waitForThumbnail(top))
                    throw std::runtime_error("La miniature après le passage de la liste à la grille est absente.");
                command(ListView);
                vrMenu();
            }
            const int row = index - displayedFiles_.first;
            vrPoint({rayTo(.66f, row), press.update(false, true), VrUi::Trigger::Pressed});
            if (file_ != initial || page_ != VrPage::Hidden)
                throw std::runtime_error("La gâchette n’a pas sélectionné le fichier.");
            const auto waitFor = [&](auto ready, int seconds) {
                const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
                do {
                    tick();
                    if (ready()) return true;
                    std::this_thread::sleep_for(std::chrono::milliseconds(15));
                } while (media_->error().empty() && std::chrono::steady_clock::now() < until);
                return false;
            };
            if (!waitFor([&] { return target_ && media_->ready(); }, 12))
                throw std::runtime_error("L’aperçu ou la lecture de la fenêtre masquée n’a pas démarré.");
            if (media_->duration() >= 20.) {
                vrCommand(VrCommand::Select);
                if (!waitFor([&] { return media_->paused(); }, 2)) throw std::runtime_error("La commande Pause a échoué.");
                vrCommand(VrCommand::Advance);
                if (!waitFor([&] { return media_->time() >= 9.5; }, 2)) throw std::runtime_error("La commande Avance a échoué.");
                vrCommand(VrCommand::Rewind);
                if (!waitFor([&] { return media_->time() < 2.; }, 2)) throw std::runtime_error("La commande Recul a échoué.");
                vrCommand(VrCommand::Select);
                if (!waitFor([&] { return !media_->paused(); }, 2)) throw std::runtime_error("La commande Lecture a échoué.");
                vrCommand(VrCommand::Timeline);
                if (page_ != VrPage::Timeline || !vrMenu() || !displayedFiles_.rows.empty())
                    throw std::runtime_error("La gâchette n’a pas ouvert la barre flottante.");
                const auto aimAtControl = [](float u, int y) {
                    const auto surface = VrUi::Surface::Timeline;
                    const float v = static_cast<float>(y) / VrUi::timelineHeight;
                    const auto origin = XMVectorSet((u - .5f) * VrUi::surfaceWidth(surface), (.5f - v) * VrUi::surfaceHeight(surface), 2, 0);
                    return VrUi::aim(origin, XMVectorSet(0, 0, -1, 0), true, surface);
                };
                const auto start = aimAtControl(.20f, VrUi::timelineTrackY);
                const auto end = aimAtControl(.74f, VrUi::timelineTrackY);
                if (!end) throw std::runtime_error("Le pointeur de test ne touche pas la chronologie.");
                vrPoint({start, true, VrUi::Trigger::Pressed});
                vrPoint({end, false, VrUi::Trigger::Held});
                if (!timelineDrag_ || std::abs(*timelineDrag_ - VrUi::timelinePosition(end->x)) > .001 || media_->time() > 3.)
                    throw std::runtime_error("Le curseur ne suit pas le pointeur sans déplacer la lecture.");
                const double expected = *timelineDrag_ * media_->duration();
                vrPoint({std::nullopt, false, VrUi::Trigger::Released});
                if (!waitFor([&] { return std::abs(media_->time() - expected) < .75; }, 2))
                    throw std::runtime_error("La lecture n’a pas rejoint la position choisie.");
                const auto volumeStart = aimAtControl(.5f, VrUi::volumeTrackY);
                const auto volumeEnd = aimAtControl(.35f, VrUi::volumeTrackY);
                const auto muteButton = aimAtControl((VrUi::muteLeft + VrUi::muteRight) / (2.f * VrUi::width), VrUi::volumeTrackY);
                if (!volumeStart || !volumeEnd || !muteButton)
                    throw std::runtime_error("Le pointeur de test ne touche pas les commandes audio.");
                const int initialVolume = volume_;
                vrPoint({volumeStart, true, VrUi::Trigger::Pressed});
                vrPoint({volumeEnd, false, VrUi::Trigger::Held});
                const auto expectedVolume = VrUi::volumePosition(volumeEnd->x);
                if (!volumeDrag_ || timelineDrag_ || media_->volume() != expectedVolume)
                    throw std::runtime_error("Le volume ne suit pas le pointeur pendant le glissement.");
                vrPoint({std::nullopt, false, VrUi::Trigger::Released});
                const bool initiallyMuted = media_->muted();
                vrPoint({muteButton, true, VrUi::Trigger::Pressed});
                if (volumeDrag_ || media_->muted() == initiallyMuted || page_ != VrPage::Timeline || !vrMenu())
                    throw std::runtime_error("Le bouton de sourdine n’a pas fonctionné dans la barre flottante.");
                vrPoint({muteButton, false, VrUi::Trigger::Released});
                vrPoint({muteButton, true, VrUi::Trigger::Pressed});
                vrPoint({muteButton, false, VrUi::Trigger::Released});
                if (media_->muted() != initiallyMuted || media_->volume() != expectedVolume)
                    throw std::runtime_error("Le son n’a pas été rétabli au même volume.");
                volume(initialVolume);
                vrPoint({start, true, VrUi::Trigger::Pressed});
                vrCommand(VrCommand::Back);
                if (page_ != VrPage::Hidden || timelineDrag_) throw std::runtime_error("B n’a pas masqué la barre et annulé le glissement.");
                const auto resumeAt = media_->time();
                media_.reset();
                media_ = std::make_unique<Media>(renderer_->device(), true, resumeDirectory_);
                load(initial);
                if (!waitFor([&] { return media_->ready() && std::abs(media_->time() - resumeAt) < 1.; }, 4))
                    throw std::runtime_error("La vidéo rouverte n’a pas repris à sa dernière position.");
            }
            vrCommand(VrCommand::Back);
            if (page_ != VrPage::Library) throw std::runtime_error("Le bouton B n’a pas rouvert la bibliothèque.");
            selected_ = 4;
            vrCommand(VrCommand::Select);
            if (page_ != VrPage::Format || !vrMenu()) throw std::runtime_error("Le menu Format vidéo est introuvable.");
            selected_ = 1;
            vrCommand(VrCommand::Select);
            if (page_ != VrPage::Hidden || view_.projection != Projection::Equirect180 || view_.stereo != Stereo::SideBySide)
                throw std::runtime_error("Le préréglage 180° 3D n’a pas appliqué les deux réglages.");
            vrCommand(VrCommand::Back);
            selected_ = 4;
            vrCommand(VrCommand::Select);
            selected_ = 4;
            vrCommand(VrCommand::Select);
            if (page_ != VrPage::Projection) throw std::runtime_error("La liste des projections est introuvable.");
            selected_ = 4;
            vrCommand(VrCommand::Select);
            if (view_.projection != Projection::Fisheye180 || view_.stereo != Stereo::SideBySide)
                throw std::runtime_error("Le choix de projection a modifié la disposition des yeux.");
            vrCommand(VrCommand::Back);
            selected_ = 2;
            vrCommand(VrCommand::Select);
            if (page_ != VrPage::Playback || !vrMenu()) throw std::runtime_error("Le bouton A n’a pas ouvert les commandes.");
            vrCommand(VrCommand::Back);
            if (page_ != VrPage::Library) throw std::runtime_error("Le bouton B n’a pas quitté les commandes.");
            vrCommand(VrCommand::Back);
            if (library_.location() != initial.parent_path().parent_path()) throw std::runtime_error("Le bouton B n’a pas ouvert le dossier parent.");
            open(initial.parent_path());
            std::vector<fs::path> videos;
            for (const auto& entry : library_.entries()) {
                if (!entry.folder && Library::isVideo(entry.path)) videos.push_back(entry.path);
            }
            const auto current = std::find(videos.begin(), videos.end(), initial);
            if (current != videos.end() && current + 1 != videos.end()) {
                setPage(VrPage::Hidden);
                VrUi::FileGesture gesture;
                if (gesture.update(1, true) != 1 || !gesture.consumesSeek() || gesture.update(1, true) ||
                    gesture.update(0, true) || !gesture.consumesSeek())
                    throw std::runtime_error("La combinaison gâchette et joystick se répète ou laisse passer le saut de temps.");
                vrCommand(VrCommand::NextFile);
                if (file_ != *(current + 1)) throw std::runtime_error("La combinaison vers la droite a ouvert le mauvais fichier.");
                gesture.update(0, false);
                if (gesture.consumesSeek() || gesture.update(-1, true) != -1)
                    throw std::runtime_error("La combinaison vers la gauche ne se réarme pas.");
                vrCommand(VrCommand::PreviousFile);
                if (file_ != initial) throw std::runtime_error("La combinaison vers la gauche a ouvert le mauvais fichier.");
            }
            MSG close{};
            const auto checkQuit = [&](VrPage page, VrUi::Surface surface, VrUi::Trigger trigger) {
                setPage(page);
                if (!vrMenu()) throw std::runtime_error("Le menu du bouton Quitter est introuvable.");
                const auto pointer = pointAt(VrUi::quitButtonRect(surface), surface);
                vrPoint({pointer});
                if (!quitFocused_ || PeekMessageW(&close, window_, WM_CLOSE, WM_CLOSE, PM_REMOVE))
                    throw std::runtime_error("Le survol du bouton Quitter a échoué.");
                vrPoint({pointer, true, trigger});
                if (!PeekMessageW(&close, window_, WM_CLOSE, WM_CLOSE, PM_REMOVE))
                    throw std::runtime_error("Le bouton Quitter du casque n’a pas demandé la fermeture.");
            };
            checkQuit(VrPage::Library, VrUi::Surface::Full, VrUi::Trigger::Pressed);
            checkQuit(VrPage::Playback, VrUi::Surface::Full, VrUi::Trigger::Pressed);
            checkQuit(VrPage::Timeline, VrUi::Surface::Timeline, VrUi::Trigger::Pressed);
            checkQuit(VrPage::Timeline, VrUi::Surface::Timeline, VrUi::Trigger::Idle);
            SendMessageW(item(QuitPlayer), BM_CLICK, 0, 0);
            if (!PeekMessageW(&close, window_, WM_CLOSE, WM_CLOSE, PM_REMOVE))
                throw std::runtime_error("Le bouton Quitter de la fenêtre n’a pas demandé la fermeture.");
            DispatchMessageW(&close);
            if (IsWindow(window_)) throw std::runtime_error("La fenêtre du lecteur est encore ouverte.");
            return;
        }
        ShowWindow(window_, showCommand);
        UpdateWindow(window_);
        if (!initial.empty()) {
            try {
                if (fs::is_directory(initial)) open(fs::absolute(initial));
                else load(initial);
            }
            catch (const std::exception& failure) { show(wide(failure.what())); }
        }
        if (startInVr) {
            startupUntil_ = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            autoStarting_ = true;
            tryAutomaticVr();
        }
        MSG msg{};
        for (;;) {
            if (vr_ && vr_->active()) {
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    if (msg.message == WM_QUIT) return;
                    dispatch(msg);
                }
                if (vr_->active() && !renderVr()) MsgWaitForMultipleObjectsEx(0, nullptr, 8, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            } else {
                if (GetMessageW(&msg, nullptr, 0, 0) <= 0) return;
                dispatch(msg);
            }
        }
    }

private:
    HWND window_ = nullptr, preview_ = nullptr, title_ = nullptr, location_ = nullptr, name_ = nullptr, status_ = nullptr;
    HWND clock_ = nullptr, geometry_ = nullptr;
    HWND grid_ = nullptr;
    HFONT font_ = nullptr, buttonFont_ = nullptr, headingFont_ = nullptr;
    HBRUSH brush_ = nullptr, panelBrush_ = nullptr;
    Library library_;
    Thumbnails thumbnails_;
    std::unique_ptr<Vr> vr_;
    std::unique_ptr<Renderer> renderer_;
    std::unique_ptr<Media> media_;
    std::unique_ptr<MenuTexture> menuTexture_;
    ComPtr<IDXGISwapChain1> swapchain_;
    ComPtr<ID3D11RenderTargetView> target_;
    UINT width_ = 0, height_ = 0;
    ViewSettings view_;
    VideoFormat detected_;
    fs::path file_, resumeDirectory_;
    std::wstring vrError_, startupError_;
    float yaw_ = 0, pitch_ = 0, roll_ = 0;
    POINT drag_{};
    bool seeking_ = false, menuDirty_ = true, autoStarting_ = false;
    bool automaticProjection_ = true, automaticStereo_ = true;
    VrPage page_ = VrPage::Hidden;
    VrPage formatReturnPage_ = VrPage::Library;
    VrPage displayedPage_ = VrPage::Hidden;
    VrColumn displayedFiles_, displayedSources_;
    bool gridView_ = false, listCurrent_ = false, displayedGrid_ = false, thumbnailViewportDirty_ = false;
    int gridSelection_ = -1, gridWheel_ = 0, gridScroll_ = 0, displayedFirstCard_ = 0, pointedView_ = -1;
    static constexpr int desktopColumns = 2, desktopCardHeight = 106, desktopGap = 6;
    std::optional<double> timelineDrag_;
    std::optional<XMFLOAT2> gridPointer_;
    bool volumeDrag_ = false, muteFocused_ = false, quitFocused_ = false;
    bool sourceFocus_ = true, joystickFocus_ = false, pointedSource_ = false;
    int selected_ = 0, sourceSelected_ = 0, pointedRow_ = -1, librarySelection_ = 0, volume_ = 100;
    unsigned stereoSamples_ = 0;
    double lastStereoPosition_ = -1.;
    std::wstring vrNotice_;
    std::chrono::steady_clock::time_point startupUntil_{}, retryAt_{}, menuRefreshAt_{}, stereoSampleAt_{};
    unsigned timerCount_ = 0;

    HWND item(int id) const { return GetDlgItem(window_, id); }

    HWND control(const wchar_t* kind, const wchar_t* label, DWORD style, int id = 0) {
        auto result = CreateWindowExW(0, kind, label, WS_CHILD | WS_VISIBLE | style,
            0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
        if (!result) throw std::runtime_error("Création d’un contrôle du lecteur impossible.");
        SendMessageW(result, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
        return result;
    }

    void create() {
        font_ = font(-17, FW_NORMAL, L"Segoe UI");
        buttonFont_ = font(-16, FW_NORMAL, L"Segoe UI");
        headingFont_ = font(-24, FW_SEMIBOLD);
        brush_ = CreateSolidBrush(background);
        panelBrush_ = CreateSolidBrush(panelColor);
        const BOOL darkCaption = TRUE;
        DwmSetWindowAttribute(window_, dwmDarkMode, &darkCaption, sizeof(darkCaption));
        DwmSetWindowAttribute(window_, dwmCaptionColor, &background, sizeof(background));
        DwmSetWindowAttribute(window_, dwmTextColor, &textColor, sizeof(textColor));
        DwmSetWindowAttribute(window_, dwmBorderColor, &edge, sizeof(edge));
        title_ = control(L"STATIC", appName, SS_LEFT);
        SendMessageW(title_, WM_SETFONT, reinterpret_cast<WPARAM>(headingFont_), FALSE);
        location_ = control(L"STATIC", L"", SS_PATHELLIPSIS);
        name_ = control(L"STATIC", L"Ouvre un fichier ou choisis un disque", SS_ENDELLIPSIS);
        status_ = control(L"STATIC", L"", SS_ENDELLIPSIS);
        clock_ = control(L"STATIC", L"0:00 / 0:00", SS_RIGHT);
        geometry_ = control(L"STATIC", L"", SS_LEFT);
        for (const auto& entry : {
            std::pair<int, const wchar_t*>{OpenFile, L"Ouvrir un fichier"}, {OpenFolder, L"Choisir un dossier"},
            {Bookmark, L"★ Favori"}, {Up, L"↑ Dossier parent"}, {Home, L"Disques"},
            {ListView, L"Liste"}, {GridView, L"Grille"},
            {QuitPlayer, L"Quitter"},
            {ZoomDown, L"−"}, {ZoomUp, L"+"}, {Center, L"Recentrer"}, {Play, L"Lire / Pause"},
            {Back, L"−10 s"}, {Forward, L"+10 s"}, {Mute, L"Son"}, {EnterVr, L"Entrer en VR"},
            {Narrow, L"Champ −"}, {Widen, L"Champ +"}, {DepthDown, L"3D −"}, {DepthUp, L"3D +"},
        }) {
            auto button = control(L"BUTTON", entry.second, WS_TABSTOP | BS_OWNERDRAW, entry.first);
            SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(buttonFont_), FALSE);
            styleButton(button, entry.first == EnterVr, false);
        }
        auto swap = control(L"BUTTON", L"Inverser les yeux", WS_TABSTOP | BS_AUTOCHECKBOX, SwapEyes);
        SendMessageW(swap, WM_SETFONT, reinterpret_cast<WPARAM>(buttonFont_), FALSE);
        styleButton(swap, false, true);
        control(L"LISTBOX", L"", WS_TABSTOP | WS_VSCROLL | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS |
            LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, Entries);
        grid_ = control(gridClass, L"Grille de la bibliothèque", WS_TABSTOP | WS_VSCROLL);
        ShowWindow(grid_, SW_HIDE);
        styleViewButton(item(ListView), false, true);
        styleViewButton(item(GridView), true, false);
        styleQuitButton(item(QuitPlayer));
        for (const auto id : {ProjectionMode, StereoMode}) {
            styleCombo(control(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS, id));
        }
        for (int i = 0; i < projectionCount; ++i)
            SendDlgItemMessageW(window_, ProjectionMode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(projectionName(static_cast<Projection>(i))));
        for (int i = 0; i < stereoCount; ++i)
            SendDlgItemMessageW(window_, StereoMode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(stereoName(static_cast<Stereo>(i))));
        SendDlgItemMessageW(window_, ProjectionMode, CB_INSERTSTRING, 0, reinterpret_cast<LPARAM>(L"Auto"));
        SendDlgItemMessageW(window_, StereoMode, CB_INSERTSTRING, 0, reinterpret_cast<LPARAM>(L"Auto"));
        control(TRACKBAR_CLASSW, L"", WS_TABSTOP | TBS_NOTICKS, Seek);
        control(TRACKBAR_CLASSW, L"", WS_TABSTOP | TBS_NOTICKS, Volume);
        SendDlgItemMessageW(window_, Seek, TBM_SETRANGE, TRUE, MAKELPARAM(0, 1000));
        SendDlgItemMessageW(window_, Volume, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        SendDlgItemMessageW(window_, Volume, TBM_SETPOS, TRUE, 100);
        preview_ = control(previewClass, L"", 0);
        ComPtr<IDXGIDevice> device;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory2> factory;
        checked(renderer_->device()->QueryInterface(IID_PPV_ARGS(&device)), "Aperçu Direct3D");
        checked(device->GetAdapter(&adapter), "Carte graphique de l’aperçu");
        checked(adapter->GetParent(IID_PPV_ARGS(&factory)), "Service de l’aperçu");
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = desc.Height = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferCount = 2;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        checked(factory->CreateSwapChainForHwnd(renderer_->device(), preview_, &desc, nullptr, nullptr, &swapchain_),
            "Création de l’aperçu vidéo");
        factory->MakeWindowAssociation(window_, DXGI_MWA_NO_ALT_ENTER);
        DragAcceptFiles(window_, TRUE);
        refreshLibrary();
        refreshSettings();
        show(vr_ ? L"Rift détecté. Ouvre un média et entre en VR." : vrError_);
        SetTimer(window_, 1, 15, nullptr);
    }

    void layout(int w, int h) {
        if (!preview_) return;
        constexpr int sidebar = 305;
        const int right = sidebar + 28;
        const int available = std::max(1, w - right - 16);
        const auto put = [](HWND child, int x, int y, int width, int height) { MoveWindow(child, x, y, width, height, TRUE); };
        put(title_, 16, 13, 171, 28);
        put(item(OpenFile), right, 10, 145, 31);
        put(item(OpenFolder), right + 153, 10, 154, 31);
        put(item(EnterVr), w - 151, 10, 135, 31);
        put(item(Home), 16, 52, 76, 29);
        put(item(Up), 98, 52, 124, 29);
        put(item(Bookmark), 228, 52, 93, 29);
        put(location_, 16, 90, sidebar, 26);
        put(item(ListView), 190, 10, 62, 31);
        put(item(GridView), 258, 10, 63, 31);
        put(item(Entries), 16, 120, sidebar, std::max(1, h - 154));
        put(grid_, 16, 120, sidebar, std::max(1, h - 154));
        put(name_, right, 49, available, 26);
        put(preview_, right, 77, available, std::max(1, h - 232));
        put(item(Seek), right, h - 150, std::max(1, available - 115), 26);
        put(clock_, w - 125, h - 148, 107, 26);
        put(item(Play), right, h - 117, 96, 28);
        put(item(Back), right + 101, h - 117, 64, 28);
        put(item(Forward), right + 170, h - 117, 64, 28);
        put(item(Mute), right + 239, h - 117, 53, 28);
        put(item(Volume), right + 298, h - 117, 70, 27);
        put(item(Center), right + 373, h - 117, 90, 28);
        put(item(ZoomDown), right + 470, h - 117, 31, 28);
        put(item(ZoomUp), right + 506, h - 117, 31, 28);
        put(item(ProjectionMode), right, h - 85, 158, 190);
        put(item(StereoMode), right + 163, h - 85, 172, 180);
        put(item(SwapEyes), right + 340, h - 86, 144, 28);
        put(item(Narrow), right, h - 53, 82, 25);
        put(item(Widen), right + 88, h - 53, 82, 25);
        put(item(DepthDown), right + 178, h - 53, 55, 25);
        put(item(DepthUp), right + 239, h - 53, 55, 25);
        put(geometry_, right + 309, h - 51, std::max(1, available - 309), 24);
        put(status_, 16, h - 26, w - 140, 22);
        put(item(QuitPlayer), w - 116, h - 26, 100, 24);
        InvalidateRect(window_, nullptr, TRUE);
    }

    void populateList() {
        if (listCurrent_) return;
        const auto& entries = library_.entries();
        SendDlgItemMessageW(window_, Entries, WM_SETREDRAW, FALSE, 0);
        SendDlgItemMessageW(window_, Entries, LB_RESETCONTENT, 0, 0);
        SendDlgItemMessageW(window_, Entries, LB_INITSTORAGE, entries.size(), entries.size() * 80);
        for (const auto& entry : entries) {
            const auto label = (entry.folder ? L"▸  " : L"    ") + entry.label;
            SendDlgItemMessageW(window_, Entries, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }
        SendDlgItemMessageW(window_, Entries, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(item(Entries), nullptr, TRUE);
        listCurrent_ = true;
    }

    void refreshLibrary() {
        thumbnails_.reset();
        listCurrent_ = false;
        gridSelection_ = -1;
        gridWheel_ = 0;
        scrollGrid(0);
        if (!gridView_) populateList();
        SetWindowTextW(location_, pathName(library_.location()).c_str());
        EnableWindow(item(Bookmark), !library_.location().empty());
        librarySelection_ = library_.location().empty() || library_.entries().empty() ? 0 : shortcutsInLibrary;
        if (page_ == VrPage::Library) {
            selected_ = library_.location().empty() ? 0 : librarySelection_;
            sourceFocus_ = library_.location().empty();
            invalidatePointer();
        }
        menuDirty_ = true;
    }

    void setLibraryView(bool grid) {
        if (gridView_ == grid) return;
        const auto list = item(Entries);
        const bool focus = GetFocus() == (grid ? list : grid_);
        gridView_ = grid;
        styleViewButton(item(ListView), false, !grid);
        styleViewButton(item(GridView), true, grid);
        if (grid) {
            gridSelection_ = listCurrent_ ? static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0)) : -1;
            const int first = static_cast<int>(SendMessageW(list, LB_GETTOPINDEX, 0, 0));
            ShowWindow(list, SW_HIDE);
            ShowWindow(grid_, SW_SHOW);
            scrollGrid(first / desktopColumns * desktopCardHeight);
        } else {
            ShowWindow(grid_, SW_HIDE);
            populateList();
            ShowWindow(list, SW_SHOW);
            SendMessageW(list, LB_SETCURSEL, gridSelection_, 0);
            SendMessageW(list, LB_SETTOPINDEX, gridScroll_ / desktopCardHeight * desktopColumns, 0);
            thumbnails_.show({}, library_.entries());
        }
        if (focus) SetFocus(grid ? grid_ : list);
        invalidatePointer();
    }

    void scrollGrid(int position) {
        RECT area{};
        GetClientRect(grid_, &area);
        const int rows = (static_cast<int>(library_.entries().size()) + desktopColumns - 1) / desktopColumns;
        const int height = rows * desktopCardHeight + desktopGap;
        gridScroll_ = std::clamp(position, 0, std::max(0, height - static_cast<int>(area.bottom)));
        SCROLLINFO scroll{sizeof(scroll), SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL};
        scroll.nMax = height - 1;
        scroll.nPage = area.bottom;
        scroll.nPos = gridScroll_;
        SetScrollInfo(grid_, SB_VERT, &scroll, TRUE);
        InvalidateRect(grid_, nullptr, FALSE);
        thumbnailViewportDirty_ = true;
    }

    RECT desktopCard(int index, int width) const {
        const int step = (width - desktopGap) / desktopColumns;
        const int left = desktopGap + index % desktopColumns * step;
        const int top = desktopGap + index / desktopColumns * desktopCardHeight - gridScroll_;
        return {left, top, left + step - desktopGap, top + desktopCardHeight - desktopGap};
    }

    std::pair<int, int> desktopVisible(int height) const {
        const int count = static_cast<int>(library_.entries().size());
        const int first = gridScroll_ / desktopCardHeight * desktopColumns;
        const int end = (gridScroll_ + height + desktopCardHeight - 1) / desktopCardHeight * desktopColumns;
        return {std::min(count, first), std::min(count, end)};
    }

    void paintGrid(HDC dc, RECT area) {
        fill(dc, area, panelColor);
        const auto oldFont = SelectObject(dc, font_);
        const auto [first, end] = desktopVisible(area.bottom);
        for (int i = first; i < end; ++i) {
            drawLibraryCard(dc, desktopCard(i, area.right), library_.entries()[i], i == gridSelection_,
                thumbnails_.get(i), 22);
        }
        SelectObject(dc, oldFont);
    }

    LRESULT gridMessage(UINT event, WPARAM w, LPARAM l) {
        RECT area{};
        GetClientRect(grid_, &area);
        switch (event) {
        case WM_SIZE: scrollGrid(gridScroll_); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            auto dc = BeginPaint(grid_, &paint);
            const auto buffer = CreateCompatibleDC(dc);
            const auto bitmap = buffer && area.right > 0 && area.bottom > 0
                ? CreateCompatibleBitmap(dc, area.right, area.bottom) : nullptr;
            if (bitmap) {
                const auto old = SelectObject(buffer, bitmap);
                paintGrid(buffer, area);
                BitBlt(dc, 0, 0, area.right, area.bottom, buffer, 0, 0, SRCCOPY);
                SelectObject(buffer, old);
                DeleteObject(bitmap);
            } else paintGrid(dc, area);
            if (buffer) DeleteDC(buffer);
            EndPaint(grid_, &paint);
            return 0;
        }
        case WM_PRINTCLIENT: paintGrid(reinterpret_cast<HDC>(w), area); return 0;
        case WM_VSCROLL: {
            SCROLLINFO scroll{sizeof(scroll), SIF_TRACKPOS};
            GetScrollInfo(grid_, SB_VERT, &scroll);
            int position = gridScroll_;
            switch (LOWORD(w)) {
            case SB_LINEUP: position -= desktopCardHeight; break;
            case SB_LINEDOWN: position += desktopCardHeight; break;
            case SB_PAGEUP: position -= area.bottom; break;
            case SB_PAGEDOWN: position += area.bottom; break;
            case SB_THUMBTRACK: case SB_THUMBPOSITION: position = scroll.nTrackPos; break;
            case SB_TOP: position = 0; break;
            case SB_BOTTOM: position = INT_MAX; break;
            }
            scrollGrid(position);
            return 0;
        }
        case WM_MOUSEWHEEL:
            gridWheel_ += GET_WHEEL_DELTA_WPARAM(w);
            if (const int steps = gridWheel_ / WHEEL_DELTA) {
                gridWheel_ %= WHEEL_DELTA;
                scrollGrid(gridScroll_ - steps * desktopCardHeight);
            }
            return 0;
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
            SetFocus(grid_);
            const POINT pointer{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            const auto [first, end] = desktopVisible(area.bottom);
            for (int i = first; i < end; ++i) {
                const auto card = desktopCard(i, area.right);
                if (!PtInRect(&card, pointer)) continue;
                gridSelection_ = i;
                InvalidateRect(grid_, nullptr, FALSE);
                if (event == WM_LBUTTONDBLCLK) activateEntry(i);
                break;
            }
            return 0;
        }
        case WM_KEYDOWN: {
            const int count = static_cast<int>(library_.entries().size());
            if (!count) break;
            if (w == VK_RETURN) {
                activateEntry(gridSelection_);
                return 0;
            }
            int next = gridSelection_;
            switch (w) {
            case VK_LEFT: --next; break;
            case VK_RIGHT: ++next; break;
            case VK_UP: next -= desktopColumns; break;
            case VK_DOWN: next += desktopColumns; break;
            case VK_HOME: next = 0; break;
            case VK_END: next = count - 1; break;
            case VK_PRIOR: next -= std::max(1L, area.bottom / desktopCardHeight) * desktopColumns; break;
            case VK_NEXT: next += std::max(1L, area.bottom / desktopCardHeight) * desktopColumns; break;
            default: return DefWindowProcW(grid_, event, w, l);
            }
            gridSelection_ = std::clamp(gridSelection_ < 0 ? 0 : next, 0, count - 1);
            const auto card = desktopCard(gridSelection_, area.right);
            scrollGrid(gridScroll_ + std::min(0L, card.top) + std::max(0L, card.bottom - area.bottom));
            return 0;
        }
        }
        return DefWindowProcW(grid_, event, w, l);
    }

    void refreshSettings(bool updateAutoLabels = false) {
        if (updateAutoLabels) {
            const auto projection = std::wstring(L"Auto · ") + projectionName(detected_.view.projection);
            const auto stereo = std::wstring(L"Auto · ") + stereoName(detected_.view.stereo);
            for (auto item : {std::pair<int, std::wstring>{ProjectionMode, projection}, {StereoMode, stereo}}) {
                SendDlgItemMessageW(window_, item.first, CB_DELETESTRING, 0, 0);
                SendDlgItemMessageW(window_, item.first, CB_INSERTSTRING, 0, reinterpret_cast<LPARAM>(item.second.c_str()));
            }
        }
        SendDlgItemMessageW(window_, ProjectionMode, CB_SETCURSEL, automaticProjection_ ? 0 : 1 + static_cast<int>(view_.projection), 0);
        SendDlgItemMessageW(window_, StereoMode, CB_SETCURSEL, automaticStereo_ ? 0 : 1 + static_cast<int>(view_.stereo), 0);
        CheckDlgButton(window_, SwapEyes, view_.swapEyes ? BST_CHECKED : BST_UNCHECKED);
        const bool stereo = view_.stereo != Stereo::Mono;
        const bool field = view_.projection != Projection::Flat && view_.projection != Projection::Cubemap &&
            view_.projection != Projection::Calibrated;
        EnableWindow(item(SwapEyes), stereo);
        EnableWindow(item(Narrow), field);
        EnableWindow(item(Widen), field);
        EnableWindow(item(DepthDown), stereo && view_.projection != Projection::Cubemap);
        EnableWindow(item(DepthUp), stereo && view_.projection != Projection::Cubemap);
        auto readout = field ? L"Champ " + std::to_wstring(std::lround(fieldOfView(view_))) + L"°" : std::wstring{};
        if (stereo && view_.projection != Projection::Cubemap) {
            wchar_t percent[24]{};
            swprintf_s(percent, L"3D %+.1f %%", 100. * view_.convergence);
            if (field) readout += L"   ";
            readout += percent;
        }
        SetWindowTextW(geometry_, readout.c_str());
        menuDirty_ = true;
    }

    void applyProjection(int index) {
        automaticProjection_ = index == 0;
        view_.projection = automaticProjection_ ? detected_.view.projection : static_cast<Projection>(index - 1);
        view_.horizontalFov = automaticProjection_ ? detected_.view.horizontalFov : 0.f;
        view_.verticalFov = automaticProjection_ ? detected_.view.verticalFov : 180.f;
        view_.centerYaw = automaticProjection_ ? detected_.view.centerYaw : 0.f;
        view_.centerPitch = automaticProjection_ ? detected_.view.centerPitch : 0.f;
        view_.mesh = automaticProjection_ ? detected_.view.mesh : nullptr;
    }

    void applyStereo(int index) {
        automaticStereo_ = index == 0;
        view_.stereo = automaticStereo_ ? detected_.view.stereo : static_cast<Stereo>(index - 1);
        view_.swapEyes = automaticStereo_ && detected_.view.swapEyes;
        view_.convergence = 0;
    }

    void adjustField(float delta) {
        if (view_.projection == Projection::Flat || view_.projection == Projection::Cubemap ||
            view_.projection == Projection::Calibrated) return;
        const float maximum = view_.projection == Projection::Fisheye180 ? 220.f : 360.f;
        view_.horizontalFov = std::clamp(fieldOfView(view_) + delta, 60.f, maximum);
        refreshSettings();
    }

    void adjustDepth(float delta) {
        if (view_.stereo == Stereo::Mono || view_.projection == Projection::Cubemap) return;
        view_.convergence = std::clamp(view_.convergence + delta, -.03f, .03f);
        refreshSettings();
    }

    void restartStereoScan() {
        stereoSamples_ = 0;
        lastStereoPosition_ = -1.;
        stereoSampleAt_ = {};
    }

    void seek(double seconds, bool absolute = false) {
        media_->seek(seconds, absolute);
        if ((automaticProjection_ || automaticStereo_) && !(detected_.layoutKnown && detected_.projectionKnown)) {
            restartStereoScan();
            stereoSampleAt_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
        }
    }

    void show(const std::wstring& message) {
        if (status_) SetWindowTextW(status_, message.c_str());
    }

    void open(const fs::path& path) {
        library_.open(path);
        refreshLibrary();
    }

    void load(const fs::path& file) {
        const auto path = fs::absolute(file);
        media_->load(path);
        timelineDrag_.reset();
        volumeDrag_ = false;
        if (path != file_) {
            automaticProjection_ = true;
            automaticStereo_ = true;
        }
        file_ = path;
        vrNotice_.clear();
        detected_ = detectFormat(path);
        restartStereoScan();
        if (automaticProjection_) applyProjection(0);
        if (automaticStereo_) applyStereo(0);
        refreshSettings(true);
        SetWindowTextW(name_, path.filename().c_str());
        show(detected_.cameraMesh && !detected_.view.mesh ? L"Calibration illisible. Choisis la projection et la 3D au besoin." : L"");
        menuDirty_ = true;
    }

    void activateEntry(int index) {
        if (index < 0 || index >= static_cast<int>(library_.entries().size())) return;
        auto entry = library_.entries()[index];
        if (entry.folder) open(entry.path);
        else { load(entry.path); setPage(VrPage::Hidden); }
    }

    void pick(bool folder) {
        ComPtr<IFileOpenDialog> dialog;
        checked(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)), "Sélecteur Windows");
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
        dialog->SetTitle(folder ? L"Choisir un dossier de vidéos" : L"Ouvrir un média");
        const auto result = dialog->Show(window_);
        if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return;
        checked(result, "Ouverture du sélecteur Windows");
        ComPtr<IShellItem> selection;
        checked(dialog->GetResult(&selection), "Fichier sélectionné");
        PWSTR path = nullptr;
        checked(selection->GetDisplayName(SIGDN_FILESYSPATH, &path), "Chemin du média");
        fs::path selected(path);
        CoTaskMemFree(path);
        if (folder) open(selected);
        else load(selected);
    }

    void center() {
        yaw_ = pitch_ = roll_ = 0;
        view_.zoom = 1;
        if (vr_) vr_->recenter();
    }

    void zoom(float factor) { view_.zoom = std::clamp(view_.zoom * factor, .3f, 3.f); }

    void setPage(VrPage next) {
        if (page_ == next) return;
        if (page_ == VrPage::Library) librarySelection_ = selected_;
        page_ = next;
        timelineDrag_.reset();
        volumeDrag_ = false;
        selected_ = next == VrPage::Library ? librarySelection_ : 0;
        sourceFocus_ = next == VrPage::Library && library_.location().empty();
        joystickFocus_ = false;
        invalidatePointer();
    }

    void invalidatePointer() {
        displayedPage_ = VrPage::Hidden;
        pointedRow_ = -1;
        pointedView_ = -1;
        gridPointer_.reset();
        muteFocused_ = false;
        quitFocused_ = false;
        menuDirty_ = true;
        thumbnailViewportDirty_ = true;
    }

    void startVr() {
        if (!vr_) vr_ = std::make_unique<Vr>();
        vr_->start(*renderer_);
        setPage(file_.empty() ? VrPage::Library : VrPage::Hidden);
        SetWindowTextW(item(EnterVr), L"Quitter la VR");
        show(L"Session VR ouverte. Enfile le Rift pour utiliser la manette.");
    }

    void tryAutomaticVr() {
        const auto now = std::chrono::steady_clock::now();
        if (!autoStarting_ || now < retryAt_) return;
        try {
            startVr();
            autoStarting_ = false;
        } catch (const std::exception& error) {
            autoStarting_ = now < startupUntil_;
            retryAt_ = now + std::chrono::seconds(1);
            show((autoStarting_ ? L"En attente du Rift… " : L"Le Rift n’a pas démarré. ") + wide(error.what()));
        }
    }

    void vrToggle() {
        autoStarting_ = false;
        if (!vr_ || !vr_->active()) { startVr(); return; }
        vr_->close();
        setPage(VrPage::Hidden);
        show(L"Session VR fermée.");
        SetWindowTextW(item(EnterVr), L"Entrer en VR");
    }

    void command(int id, int notification = 0) {
        switch (id) {
        case OpenFile: pick(false); break;
        case OpenFolder: pick(true); break;
        case Bookmark:
            if (!library_.location().empty()) {
                library_.toggleBookmark();
                const bool saved = library_.currentIsBookmarked();
                vrNotice_ = saved ? L"Dossier ajouté aux favoris." : L"Dossier retiré des favoris.";
                sourceSelected_ = std::min(sourceSelected_, std::max(0, static_cast<int>(library_.sources().size()) - 1));
                invalidatePointer();
                show(vrNotice_);
            }
            break;
        case Up: library_.up(); refreshLibrary(); break;
        case Home: open({}); break;
        case ListView: setLibraryView(false); break;
        case GridView: setLibraryView(true); break;
        case QuitPlayer: PostMessageW(window_, WM_CLOSE, 0, 0); break;
        case Entries:
            if (notification == LBN_DBLCLK)
                activateEntry(static_cast<int>(SendDlgItemMessageW(window_, Entries, LB_GETCURSEL, 0, 0)));
            break;
        case ProjectionMode:
            if (notification == CBN_SELCHANGE) {
                applyProjection(static_cast<int>(SendDlgItemMessageW(window_, ProjectionMode, CB_GETCURSEL, 0, 0)));
                refreshSettings();
            }
            break;
        case StereoMode:
            if (notification == CBN_SELCHANGE) {
                applyStereo(static_cast<int>(SendDlgItemMessageW(window_, StereoMode, CB_GETCURSEL, 0, 0)));
                refreshSettings();
            }
            break;
        case SwapEyes: view_.swapEyes = IsDlgButtonChecked(window_, SwapEyes) == BST_CHECKED; menuDirty_ = true; break;
        case ZoomDown: zoom(.90f); break;
        case ZoomUp: zoom(1.10f); break;
        case Narrow: adjustField(-10); break;
        case Widen: adjustField(10); break;
        case DepthDown: adjustDepth(-.005f); break;
        case DepthUp: adjustDepth(.005f); break;
        case Center: center(); break;
        case Play: if (!file_.empty()) media_->togglePause(); break;
        case Back: seek(-10); break;
        case Forward: seek(10); break;
        case Mute: media_->mute(); menuDirty_ = true; break;
        case EnterVr: vrToggle(); break;
        }
    }

    bool shortcuts(const MSG& msg) {
        if (msg.hwnd == item(Entries) && msg.wParam == VK_RETURN) {
            command(Entries, LBN_DBLCLK);
            return true;
        }
        if (msg.hwnd != window_ && msg.hwnd != preview_) return false;
        switch (msg.wParam) {
        case VK_SPACE: command(Play); break;
        case VK_LEFT: command(Back); break;
        case VK_RIGHT: command(Forward); break;
        case 'R': command(Center); break;
        case 'M': command(Mute); break;
        default: return false;
        }
        return true;
    }

    std::vector<VrRow> vrRows() const {
        const auto stereoLabel = std::wstring(automaticStereo_ ? L"3D auto   ·   " : L"3D   ·   ") + stereoName(view_.stereo);
        if (page_ == VrPage::Format) {
            return {
                {L"Auto   ·   détecter la projection et la 3D", VrAction::Preset, 0},
                {L"180° 3D   ·   côte à côte", VrAction::Preset, 1},
                {L"180° fisheye 3D   ·   côte à côte", VrAction::Preset, 2},
                {L"360°   ·   mono", VrAction::Preset, 3},
                {std::wstring(L"Projection   ›   ") + projectionName(view_.projection), VrAction::Projection},
                {L"Disposition   ›   " + stereoLabel, VrAction::Stereo},
                {view_.swapEyes ? L"Permuter les yeux   ·   inversés" : L"Permuter les yeux", VrAction::SwapEyes},
                {L"← Retour", VrAction::FormatBack},
            };
        }
        if (page_ == VrPage::Projection) {
            return {{L"Auto", VrAction::ProjectionChoice, 0}, {L"Écran plat", VrAction::ProjectionChoice, 1},
                {L"180° équirectangulaire", VrAction::ProjectionChoice, 2}, {L"360° équirectangulaire", VrAction::ProjectionChoice, 3},
                {L"180° fisheye", VrAction::ProjectionChoice, 4}, {L"Cube 3 × 2", VrAction::ProjectionChoice, 5},
                {L"← Formats vidéo", VrAction::FormatBack}};
        }
        if (page_ == VrPage::Stereo) {
            return {{L"Auto", VrAction::StereoChoice, 0}, {L"Mono", VrAction::StereoChoice, 1},
                {L"3D côte à côte (SBS)", VrAction::StereoChoice, 2}, {L"3D haut / bas (TB)", VrAction::StereoChoice, 3},
                {L"← Formats vidéo", VrAction::FormatBack}};
        }
        if (page_ == VrPage::Playback) {
            const auto duration = media_->duration();
            const double position = timelineDrag_ ? *timelineDrag_ * duration : media_->time();
            return {
                {L"← Bibliothèque", VrAction::Library},
                {media_->paused() ? L"Lire" : L"Mettre en pause", VrAction::Play},
                {timeLabel(position) + L" / " + timeLabel(duration), VrAction::Timeline},
                {L"Format vidéo", VrAction::Format},
                {L"Reculer de 10 secondes", VrAction::Rewind}, {L"Avancer de 10 secondes", VrAction::Advance},
                {L"Vidéo précédente du dossier", VrAction::PreviousFile}, {L"Vidéo suivante du dossier", VrAction::NextFile},
                {L"Recentrer la vue", VrAction::Center}, {L"Couper ou rétablir le son", VrAction::Mute},
                {L"Baisser le volume   ·   " + std::to_wstring(volume_) + L" %", VrAction::Quieter},
                {L"Monter le volume   ·   " + std::to_wstring(volume_) + L" %", VrAction::Louder},
                {L"Masquer le menu", VrAction::Resume},
            };
        }
        if (page_ == VrPage::Settings) {
            const auto fov = L"   ·   " + std::to_wstring(std::lround(fieldOfView(view_))) + L"°";
            wchar_t depth[24]{};
            swprintf_s(depth, L"   ·   %+.1f %%", 100. * view_.convergence);
            return {
                {L"← Bibliothèque", VrAction::Library}, {L"★ Ajouter ou retirer le dossier des favoris", VrAction::Favorite},
                {L"Réappliquer la détection automatique", VrAction::Auto},
                {L"Format vidéo   ·   " + std::wstring(projectionName(view_.projection)), VrAction::Format},
                {view_.swapEyes ? L"Ordre des yeux   ·   Inversé" : L"Ordre des yeux   ·   Normal", VrAction::SwapEyes},
                {L"Réduire le champ" + fov, VrAction::Narrow}, {L"Agrandir le champ" + fov, VrAction::Widen},
                {std::wstring(L"Convergence 3D −") + depth, VrAction::DepthDown},
                {std::wstring(L"Convergence 3D +") + depth, VrAction::DepthUp}, {L"Quitter le lecteur", VrAction::Quit},
            };
        }
        std::vector<VrRow> rows{{L"Accueil", VrAction::Drives}, {L"↑ Dossier parent", VrAction::Parent},
            {L"Commandes vidéo", VrAction::Playback}, {L"Réglages de lecture", VrAction::Settings},
            {L"Format vidéo", VrAction::Format}};
        if (library_.location().empty()) return rows;
        rows.push_back({library_.currentIsBookmarked() ? L"★ Retirer ce dossier des favoris"
            : L"☆ Ajouter ce dossier aux favoris", VrAction::Favorite});
        return rows;
    }

    VrRow vrEntryRow(int index) const {
        const auto& entry = library_.entries()[index];
        return {(entry.folder ? L"▸  " : L"     ") + entry.label, VrAction::Entry, index};
    }

    int vrEntryCount() const {
        return page_ == VrPage::Library && !library_.location().empty()
            ? static_cast<int>(library_.entries().size()) : 0;
    }

    static VrColumn visibleRows(const std::vector<VrRow>& rows, int selected) {
        const int count = static_cast<int>(rows.size());
        const int first = VrUi::firstRow(count, selected);
        return {{rows.begin() + first, rows.begin() + std::min(count, first + VrUi::rows)}, first, count, selected};
    }

    VrColumn visibleFiles(bool grid) const {
        auto rows = vrRows();
        if (grid) {
            rows[1].label = L"↑ Parent";
            rows[3].label = L"Réglages";
            rows[5].label = rows[5].label.front() == L'★' ? L"★ Favori" : L"☆ Favori";
            return {std::move(rows), 0, shortcutsInLibrary + vrEntryCount(), selected_};
        }
        const int shortcuts = static_cast<int>(rows.size());
        const int count = shortcuts + vrEntryCount();
        const int first = VrUi::firstRow(count, selected_);
        VrColumn visible{{}, first, count, selected_};
        for (int i = first; i < std::min(count, first + VrUi::rows); ++i) {
            visible.rows.push_back(i < shortcuts ? std::move(rows[i]) : vrEntryRow(i - shortcuts));
        }
        return visible;
    }

    std::vector<VrRow> sourceRows() const {
        std::vector<VrRow> rows;
        const auto& sources = library_.sources();
        for (int i = 0; i < static_cast<int>(sources.size()); ++i)
            rows.push_back({sources[i].label, VrAction::Source, i});
        return rows;
    }

    void adjacentFile(int direction) {
        if (file_.empty()) return;
        if (library_.location() != file_.parent_path()) open(file_.parent_path());
        const auto& rows = library_.entries();
        const auto current = std::find_if(rows.begin(), rows.end(), [&](const auto& row) { return row.path == file_; });
        if (current != rows.end()) {
            for (int i = static_cast<int>(current - rows.begin()) + direction;
                 i >= 0 && i < static_cast<int>(rows.size()); i += direction) {
                if (!rows[i].folder && Library::isVideo(rows[i].path)) { activateEntry(i); return; }
            }
        }
        vrNotice_ = direction < 0 ? L"Le dossier n’a pas de vidéo précédente." : L"Le dossier n’a pas de vidéo suivante.";
    }

    void volume(int value) {
        const int percent = std::clamp(value, 0, 100);
        if (percent == volume_) return;
        media_->volume(percent);
        volume_ = percent;
        SendDlgItemMessageW(window_, Volume, TBM_SETPOS, TRUE, volume_);
        menuDirty_ = true;
    }

    void closeFormat() {
        if (page_ == VrPage::Format) setPage(formatReturnPage_);
        else setPage(VrPage::Format);
    }

    void revealVideoFormat() {
        refreshSettings(true);
        setPage(VrPage::Hidden);
    }

    void activateVrRow(const VrRow& row, float pointerX = -1.f) {
        vrNotice_.clear();
        switch (row.action) {
        case VrAction::Drives: command(Home); break;
        case VrAction::Parent: command(Up); break;
        case VrAction::Library: setPage(VrPage::Library); break;
        case VrAction::Playback:
            if (file_.empty()) vrNotice_ = L"Choisis une vidéo dans la bibliothèque.";
            else setPage(VrPage::Playback);
            break;
        case VrAction::Settings: setPage(VrPage::Settings); break;
        case VrAction::Source:
            if (row.entry >= 0 && row.entry < static_cast<int>(library_.sources().size())) {
                const auto path = library_.sources()[row.entry].path;
                sourceSelected_ = row.entry;
                open(path);
            }
            break;
        case VrAction::Entry: activateEntry(row.entry); break;
        case VrAction::Favorite: command(Bookmark); break;
        case VrAction::Auto: applyProjection(0); applyStereo(0); restartStereoScan(); refreshSettings(); break;
        case VrAction::Format:
            if (file_.empty()) vrNotice_ = L"Ouvre une vidéo pour régler son format.";
            else {
                formatReturnPage_ = page_;
                setPage(VrPage::Format);
            }
            break;
        case VrAction::Projection: setPage(VrPage::Projection); break;
        case VrAction::Stereo: setPage(VrPage::Stereo); break;
        case VrAction::FormatBack: closeFormat(); break;
        case VrAction::ProjectionChoice:
            applyProjection(row.entry);
            if (row.entry == 0) restartStereoScan();
            revealVideoFormat();
            break;
        case VrAction::StereoChoice:
            applyStereo(row.entry);
            if (row.entry == 0) restartStereoScan();
            revealVideoFormat();
            break;
        case VrAction::Preset: {
            constexpr std::pair<int, int> presets[] = {{0, 0}, {2, 2}, {4, 2}, {3, 1}};
            const auto& [projection, stereo] = presets[row.entry];
            applyProjection(projection);
            applyStereo(stereo);
            if (row.entry == 0) restartStereoScan();
            revealVideoFormat();
            break;
        }
        case VrAction::SwapEyes: view_.swapEyes = !view_.swapEyes; refreshSettings(); break;
        case VrAction::Narrow: command(Narrow); break;
        case VrAction::Widen: command(Widen); break;
        case VrAction::DepthDown: command(DepthDown); break;
        case VrAction::DepthUp: command(DepthUp); break;
        case VrAction::Play: command(Play); break;
        case VrAction::Timeline:
            if (pointerX >= 0) {
                seek(VrUi::timelinePosition(pointerX) * media_->duration(), true);
            }
            break;
        case VrAction::Rewind: command(Back); break;
        case VrAction::Advance: command(Forward); break;
        case VrAction::PreviousFile: adjacentFile(-1); break;
        case VrAction::NextFile: adjacentFile(1); break;
        case VrAction::Center: center(); break;
        case VrAction::Mute: command(Mute); break;
        case VrAction::Quieter: volume(volume_ - 10); break;
        case VrAction::Louder: volume(volume_ + 10); break;
        case VrAction::Resume: setPage(VrPage::Hidden); break;
        case VrAction::Quit: command(QuitPlayer); break;
        }
        menuDirty_ = true;
    }

    void vrFailure(const std::exception& failure) {
        vrNotice_ = wide(failure.what());
        show(vrNotice_);
        menuDirty_ = true;
    }

    void vrCommand(VrCommand input) {
        try {
            if ((timelineDrag_ || volumeDrag_) && input != VrCommand::Back) return;
            switch (input) {
            case VrCommand::Timeline:
                if (page_ == VrPage::Hidden && !file_.empty()) {
                    vrNotice_.clear();
                    setPage(VrPage::Timeline);
                }
                break;
            case VrCommand::Back:
                vrNotice_.clear();
                menuDirty_ = true;
                if (page_ == VrPage::Timeline) setPage(VrPage::Hidden);
                else if (page_ == VrPage::Format || page_ == VrPage::Projection || page_ == VrPage::Stereo) closeFormat();
                else if (page_ != VrPage::Library) setPage(VrPage::Library);
                else if (!library_.location().empty()) command(Up);
                else if (media_->ready()) setPage(VrPage::Hidden);
                break;
            case VrCommand::Recenter: center(); break;
            case VrCommand::Rewind: command(Back); break;
            case VrCommand::Advance: command(Forward); break;
            case VrCommand::PreviousFile: adjacentFile(-1); break;
            case VrCommand::NextFile: adjacentFile(1); break;
            case VrCommand::FocusSources:
            case VrCommand::FocusFiles:
                if (page_ == VrPage::Library) {
                    const bool left = input == VrCommand::FocusSources;
                    if (gridView_ && !library_.location().empty() && !sourceFocus_) {
                        const int column = selected_ % VrUi::gridColumns;
                        if (left && column == 0) sourceFocus_ = true;
                        else if (left) --selected_;
                        else if (column < VrUi::gridColumns - 1)
                            selected_ = std::min(selected_ + 1, shortcutsInLibrary + vrEntryCount() - 1);
                    } else sourceFocus_ = left;
                    joystickFocus_ = true;
                    menuDirty_ = true;
                }
                break;
            case VrCommand::Previous:
            case VrCommand::Next: {
                const bool sources = page_ == VrPage::Library && sourceFocus_;
                const int count = sources ? static_cast<int>(library_.sources().size())
                    : static_cast<int>(vrRows().size()) + vrEntryCount();
                auto& index = sources ? sourceSelected_ : selected_;
                const bool rowNavigation = !sources && gridView_ && page_ == VrPage::Library && !library_.location().empty();
                const int step = rowNavigation ? VrUi::gridColumns : 1;
                const bool up = input == VrCommand::Previous;
                if (!rowNavigation || (up ? index >= step : index / step < (count - 1) / step))
                    index = std::clamp(index + (up ? -step : step), 0, std::max(0, count - 1));
                joystickFocus_ = true;
                menuDirty_ = true;
                break;
            }
            case VrCommand::Select:
                if (page_ == VrPage::Hidden || page_ == VrPage::Timeline) command(Play);
                else {
                    const bool sources = page_ == VrPage::Library && sourceFocus_;
                    const auto rows = sources ? sourceRows() : vrRows();
                    const int index = sources ? sourceSelected_ : selected_;
                    if (index >= 0 && index < static_cast<int>(rows.size())) activateVrRow(rows[index]);
                    else if (!sources && index >= shortcutsInLibrary && index - shortcutsInLibrary < vrEntryCount())
                        activateVrRow(vrEntryRow(index - shortcutsInLibrary));
                }
                break;
            }
        } catch (const std::exception& failure) { vrFailure(failure); }
    }

    void beginTimelineDrag(float x) {
        const auto duration = media_->duration();
        if (std::isfinite(duration) && duration > 0.) {
            timelineDrag_ = VrUi::timelinePosition(x);
            vrNotice_.clear();
            menuDirty_ = true;
        }
    }

    void vrPoint(const VrUi::Pointer& event) {
        try {
            const auto& pointer = event.uv;
            if (event.trigger == VrUi::Trigger::Cancelled) {
                timelineDrag_.reset();
                volumeDrag_ = false;
                muteFocused_ = false;
                quitFocused_ = false;
                pointedRow_ = -1;
                menuDirty_ = true;
                return;
            }
            if (timelineDrag_) {
                if (pointer) {
                    const auto position = VrUi::timelinePosition(pointer->x);
                    if (std::abs(position - *timelineDrag_) * VrUi::width >= 1.) menuDirty_ = true;
                    timelineDrag_ = position;
                }
                if (event.trigger == VrUi::Trigger::Released) {
                    const double time = *timelineDrag_ * media_->duration();
                    timelineDrag_.reset();
                    menuDirty_ = true;
                    seek(time, true);
                }
                return;
            }
            if (volumeDrag_) {
                if (event.trigger == VrUi::Trigger::Released) {
                    volumeDrag_ = false;
                    menuDirty_ = true;
                }
                if (pointer) volume(VrUi::volumePosition(pointer->x));
                return;
            }
            const bool samePage = displayedPage_ == page_ && page_ != VrPage::Hidden;
            const bool showQuit = page_ == VrPage::Library || page_ == VrPage::Playback || page_ == VrPage::Timeline;
            const auto surface = page_ == VrPage::Timeline ? VrUi::Surface::Timeline : VrUi::Surface::Full;
            const bool overQuit = pointer && samePage && showQuit &&
                VrUi::hitRect(pointer->x, pointer->y, VrUi::quitButtonRect(surface), surface);
            if (quitFocused_ != overQuit) {
                quitFocused_ = overQuit;
                menuDirty_ = true;
            }
            if (event.confirm && overQuit) {
                command(QuitPlayer);
                return;
            }
            if (page_ == VrPage::Timeline) {
                const bool overMute = pointer && VrUi::hitsMute(pointer->x, pointer->y);
                if (muteFocused_ != overMute) {
                    muteFocused_ = overMute;
                    menuDirty_ = true;
                }
                if (event.confirm && event.trigger == VrUi::Trigger::Idle) command(Play);
                if (event.confirm && event.trigger == VrUi::Trigger::Pressed) {
                    if (pointer && VrUi::hitsTimeline(pointer->x, pointer->y)) beginTimelineDrag(pointer->x);
                    else if (pointer && VrUi::hitsVolume(pointer->x, pointer->y)) {
                        volume(VrUi::volumePosition(pointer->x));
                        volumeDrag_ = true;
                        menuDirty_ = true;
                    } else if (overMute) command(Mute);
                    else setPage(VrPage::Hidden);
                }
                return;
            }
            const bool library = page_ == VrPage::Library;
            if (displayedGrid_ && pointer) {
                if (gridPointer_ && (std::abs(pointer->x - gridPointer_->x) > .04f ||
                    std::abs(pointer->y - gridPointer_->y) > .04f)) {
                    if (joystickFocus_) menuDirty_ = true;
                    joystickFocus_ = false;
                }
                if (!joystickFocus_ || !gridPointer_) gridPointer_ = pointer;
            }
            int viewButton = -1;
            if (pointer && samePage && library && !library_.location().empty()) {
                for (const bool grid : {false, true}) {
                    if (VrUi::hitRect(pointer->x, pointer->y, VrUi::viewButtonRect(grid))) viewButton = grid;
                }
            }
            if (pointedView_ != viewButton) {
                pointedView_ = viewButton;
                menuDirty_ = true;
            }
            if (event.confirm && viewButton >= 0) {
                setLibraryView(viewButton == 1);
                return;
            }
            const bool sources = library && pointer && pointer->x * VrUi::width < VrUi::sourcesRight;
            const auto& column = sources ? displayedSources_ : displayedFiles_;
            const int left = library && !sources ? VrUi::filesLeft : VrUi::margin;
            const int right = sources ? VrUi::sourcesRight : VrUi::width - VrUi::margin;
            const bool cards = displayedGrid_ && !sources;
            int hit = -1;
            if (pointer && samePage) {
                if (cards) {
                    for (int i = 0; i < shortcutsInLibrary; ++i) {
                        if (VrUi::hitRect(pointer->x, pointer->y, VrUi::shortcutRect(i))) hit = i;
                    }
                    for (int i = 0; i < VrUi::gridColumns * VrUi::gridRows; ++i) {
                        const int index = displayedFirstCard_ + i;
                        if (index < vrEntryCount() && VrUi::hitRect(pointer->x, pointer->y, VrUi::cardRect(i)))
                            hit = shortcutsInLibrary + index;
                    }
                } else {
                    hit = VrUi::hitRow(pointer->x, pointer->y, column.count, column.selected, left, right);
                }
            }
            if (pointedRow_ != hit || (hit >= 0 && pointedSource_ != sources)) {
                pointedRow_ = hit;
                pointedSource_ = sources;
                menuDirty_ = true;
            }
            if (hit >= 0 && library && (!displayedGrid_ || !joystickFocus_ || event.confirm)) {
                sourceFocus_ = sources;
                if (event.confirm) joystickFocus_ = false;
            }
            if (event.confirm && hit >= 0) {
                const auto row = cards && hit >= shortcutsInLibrary
                    ? vrEntryRow(hit - shortcutsInLibrary) : column.rows[hit - column.first];
                if (row.action == VrAction::Timeline && event.trigger == VrUi::Trigger::Pressed) {
                    beginTimelineDrag(pointer->x);
                } else activateVrRow(row, pointer->x);
            } else if (event.confirm && !pointer && joystickFocus_) {
                vrCommand(VrCommand::Select);
            }
        } catch (const std::exception& failure) { vrFailure(failure); }
    }

    ID3D11ShaderResourceView* vrMenu() {
        if (page_ == VrPage::Hidden) return nullptr;
        const auto now = std::chrono::steady_clock::now();
        const bool compact = page_ == VrPage::Timeline;
        const bool playback = compact || page_ == VrPage::Playback;
        if (!menuDirty_ && (!playback || now < menuRefreshAt_)) return menuTexture_->get();
        const auto title = playback ? L"Commandes vidéo" : page_ == VrPage::Settings ? L"Réglages de lecture"
            : page_ == VrPage::Projection ? L"Projection" : page_ == VrPage::Stereo ? L"Disposition 3D"
            : page_ == VrPage::Format ? L"Format vidéo" : L"Bibliothèque";
        const bool library = page_ == VrPage::Library;
        auto subtitle = library && library_.location().empty() ? std::wstring(L"Choisis un disque à gauche")
            : library || file_.empty() ? pathName(library_.location()) : file_.filename().wstring();
        if (page_ == VrPage::Format || page_ == VrPage::Projection || page_ == VrPage::Stereo) {
            subtitle = std::wstring(projectionName(view_.projection)) + L"   ·   " + stereoName(view_.stereo);
        }
        if (!vrNotice_.empty()) subtitle = vrNotice_;
        const double duration = playback ? media_->duration() : 0;
        const double progress = timelineDrag_.value_or(duration > 0 ? std::clamp(media_->time() / duration, 0., 1.) : 0.);
        if (compact) {
            const double position = timelineDrag_ ? *timelineDrag_ * duration : media_->time();
            subtitle = timeLabel(position) + L" / " + timeLabel(duration);
        }
        const bool viewButtons = library && !library_.location().empty();
        const bool grid = viewButtons && gridView_;
        const int firstCard = grid ? VrUi::firstCard(vrEntryCount(), selected_ - shortcutsInLibrary) : 0;
        if (displayedGrid_ != grid || displayedFirstCard_ != firstCard) thumbnailViewportDirty_ = true;
        displayedGrid_ = grid;
        displayedFirstCard_ = firstCard;
        displayedFiles_ = compact ? VrColumn{} : visibleFiles(displayedGrid_);
        for (auto& row : displayedFiles_.rows) {
            switch (row.action) {
            case VrAction::ProjectionChoice:
                row.active = row.entry == (automaticProjection_ ? 0 : 1 + static_cast<int>(view_.projection));
                break;
            case VrAction::StereoChoice:
                row.active = row.entry == (automaticStereo_ ? 0 : 1 + static_cast<int>(view_.stereo));
                break;
            case VrAction::SwapEyes: row.active = view_.swapEyes; break;
            case VrAction::Mute: row.active = media_->muted(); break;
            default: break;
            }
        }
        displayedSources_ = library ? visibleRows(sourceRows(), sourceSelected_) : VrColumn{};
        const bool usePointer = pointedRow_ >= 0 && (!displayedGrid_ || !joystickFocus_);
        const bool highlightSources = usePointer ? pointedSource_ : library && sourceFocus_;
        const auto highlighted = usePointer ? pointedRow_ : joystickFocus_ ? (highlightSources ? sourceSelected_ : selected_) : -1;
        displayedFiles_.highlighted = highlightSources ? -1 : highlighted;
        displayedSources_.highlighted = highlightSources ? highlighted : -1;
        menuTexture_->update(title, subtitle, displayedFiles_, library ? &displayedSources_ : nullptr,
            progress, timelineDrag_.has_value(), playback,
            compact ? std::optional(MenuTexture::AudioControls{volume_, media_->muted(), volumeDrag_, muteFocused_}) : std::nullopt,
            viewButtons ? std::optional(MenuTexture::LibraryGrid{library_.entries(), thumbnails_,
                displayedGrid_, displayedFirstCard_, pointedView_}) : std::nullopt, quitFocused_);
        displayedPage_ = page_;
        menuDirty_ = false;
        menuRefreshAt_ = now + std::chrono::milliseconds(250);
        return menuTexture_->get();
    }

    void dispatch(const MSG& msg) {
        try {
            if (msg.message == WM_MOUSEWHEEL && gridView_ && !IsIconic(window_)) {
                RECT bounds{};
                GetWindowRect(grid_, &bounds);
                const POINT cursor{GET_X_LPARAM(msg.lParam), GET_Y_LPARAM(msg.lParam)};
                if (PtInRect(&bounds, cursor)) {
                    SendMessageW(grid_, msg.message, msg.wParam, msg.lParam);
                    return;
                }
            }
            if (msg.message == WM_KEYDOWN && shortcuts(msg)) return;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        } catch (const std::exception& error) {
            show(wide(error.what()));
        }
    }

    void preview() {
        RECT area{};
        GetClientRect(preview_, &area);
        const auto w = static_cast<UINT>(area.right), h = static_cast<UINT>(area.bottom);
        if (w == 0 || h == 0 || IsIconic(window_)) return;
        if (!target_ || width_ != w || height_ != h) {
            renderer_->context()->ClearState();
            target_.Reset();
            checked(swapchain_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0), "Redimensionnement de l’aperçu");
            ComPtr<ID3D11Texture2D> back;
            checked(swapchain_->GetBuffer(0, IID_PPV_ARGS(&back)), "Image de l’aperçu");
            checked(renderer_->device()->CreateRenderTargetView(back.Get(), nullptr, &target_), "Surface de l’aperçu");
            width_ = w;
            height_ = h;
        }
        Camera camera;
        const float aspect = float(w) / h;
        camera.bottom = -.60f; camera.top = .60f;
        camera.left = -.60f * aspect; camera.right = .60f * aspect;
        XMStoreFloat4(&camera.rotation, XMQuaternionRotationRollPitchYaw(pitch_, yaw_, roll_));
        renderer_->draw(target_.Get(), w, h, camera, *media_, view_, {0, 0, 0, 1}, 0, false);
        const HRESULT result = swapchain_->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
        if (result != DXGI_ERROR_WAS_STILL_DRAWING) checked(result, "Affichage de l’aperçu");
    }

    void updateMedia() {
        media_->tick();
        constexpr int retryDelayMs[] = {650, 1350, 2000, 2500, 3000, 3500, 4500, 6000, 7000, 9000, 11000, 12000, 14000, 16000};
        if (!media_->ready() || stereoSamples_ >= std::size(retryDelayMs) ||
            (detected_.layoutKnown && detected_.projectionKnown) || (!automaticProjection_ && !automaticStereo_)) return;
        const auto now = std::chrono::steady_clock::now();
        if (now < stereoSampleAt_ || !Library::isVideo(file_)) return;
        const double position = media_->time();
        if (lastStereoPosition_ >= 0. && std::abs(position - lastStereoPosition_) < .2) {
            stereoSampleAt_ = now + std::chrono::milliseconds(400);
            return;
        }
        lastStereoPosition_ = position;
        stereoSampleAt_ = now + std::chrono::milliseconds(retryDelayMs[stereoSamples_++]);
        const bool changedLayout = applyDecoderStereo(detected_, media_->sourceStereo());
        if (detected_.layoutKnown && detected_.view.stereo == Stereo::Mono) return;
        const auto pixels = renderer_->luma(*media_, stereoProbeWidth, stereoProbeHeight);
        const bool recognized = inspectStereoFrame(detected_, pixels, media_->aspect());
        if (!changedLayout && !recognized) return;
        if (automaticProjection_) applyProjection(0);
        if (automaticStereo_ && (view_.stereo != detected_.view.stereo || view_.swapEyes != detected_.view.swapEyes)) applyStereo(0);
        refreshSettings(true);
    }

    bool renderVr() {
        bool synchronized = false;
        try {
            updateMedia();
            synchronized = vr_->frame(*renderer_, *media_, view_,
                [&](auto input) { vrCommand(input); }, [&] {
                    if (page_ == VrPage::Hidden) return VrUi::Surface::None;
                    return page_ == VrPage::Timeline ? VrUi::Surface::Timeline : VrUi::Surface::Full;
                },
                [&](const auto& pointer) { vrPoint(pointer); }, [&] { return vrMenu(); });
        } catch (const std::exception& error) {
            vr_->close();
            show(wide(error.what()));
        }
        if (!vr_->active()) {
            setPage(VrPage::Hidden);
            SetWindowTextW(item(EnterVr), L"Entrer en VR");
        }
        return synchronized;
    }

    void tick() {
        tryAutomaticVr();
        const bool immersive = vr_ && vr_->active();
        if (!immersive) updateMedia();
        if (!immersive || timerCount_ % 2 == 0) preview();
        const bool refreshStatus = ++timerCount_ % 8 == 0;
        const bool viewportChanged = std::exchange(thumbnailViewportDirty_, false);
        if (refreshStatus || viewportChanged)
            updateThumbnails(immersive, IsWindowVisible(grid_) && !IsIconic(window_));
        if (!refreshStatus) return;
        const auto error = media_->error();
        if (!error.empty()) {
            auto message = L"Lecture impossible. " + error;
            show(message);
            if (immersive && page_ == VrPage::Hidden) setPage(VrPage::Library);
            if (vrNotice_ != message) {
                vrNotice_ = std::move(message);
                menuDirty_ = true;
            }
        }
        const auto duration = media_->duration(), current = media_->time();
        if (!seeking_) SendDlgItemMessageW(window_, Seek, TBM_SETPOS, TRUE, duration > 0 ? static_cast<int>(1000 * current / duration) : 0);
        SetWindowTextW(clock_, (timeLabel(current) + L" / " + timeLabel(duration)).c_str());
    }

    void updateThumbnails(bool immersive, bool desktop) {
        if (!gridView_) return;
        std::vector<int> indices;
        const auto& entries = library_.entries();
        if (immersive && displayedPage_ == VrPage::Library && displayedGrid_) {
            const int end = std::min(static_cast<int>(entries.size()),
                displayedFirstCard_ + VrUi::gridColumns * VrUi::gridRows);
            for (int i = displayedFirstCard_; i < end; ++i) indices.push_back(i);
        }
        if (desktop && !library_.location().empty()) {
            RECT area{};
            GetClientRect(grid_, &area);
            const auto [first, end] = desktopVisible(area.bottom);
            for (int i = first; i < end; ++i) {
                const auto card = desktopCard(i, area.right);
                if (card.bottom > 0 && card.top < area.bottom &&
                    std::find(indices.begin(), indices.end(), i) == indices.end()) indices.push_back(i);
            }
        }
        thumbnails_.show(indices, entries);
        if (thumbnails_.changed()) {
            InvalidateRect(grid_, nullptr, FALSE);
            if (displayedPage_ == VrPage::Library && displayedGrid_) menuDirty_ = true;
        }
    }

    LRESULT message(UINT event, WPARAM w, LPARAM l) {
        switch (event) {
        case WM_CREATE: create(); return 0;
        case WM_SIZE: layout(LOWORD(l), HIWORD(l)); return 0;
        case WM_GETMINMAXINFO: reinterpret_cast<MINMAXINFO*>(l)->ptMinTrackSize = {925, 550}; return 0;
        case WM_COMMAND: command(LOWORD(w), HIWORD(w)); return 0;
        case WM_TIMER: tick(); return 0;
        case WM_DEVICECHANGE:
            if (w == DBT_DEVNODES_CHANGED || w == DBT_DEVICEARRIVAL || w == DBT_DEVICEREMOVECOMPLETE) {
                library_.refreshSources();
                sourceSelected_ = std::min(sourceSelected_, std::max(0, static_cast<int>(library_.sources().size()) - 1));
                if (library_.location().empty()) refreshLibrary();
                else invalidatePointer();
            }
            return TRUE;
        case WM_HSCROLL:
            if (reinterpret_cast<HWND>(l) == item(Seek)) {
                seeking_ = LOWORD(w) == TB_THUMBTRACK;
                if (!seeking_) seek(media_->duration() * SendDlgItemMessageW(window_, Seek, TBM_GETPOS, 0, 0) / 1000., true);
            } else if (reinterpret_cast<HWND>(l) == item(Volume)) {
                volume(static_cast<int>(SendDlgItemMessageW(window_, Volume, TBM_GETPOS, 0, 0)));
            }
            return 0;
        case WM_DROPFILES: {
            auto drop = reinterpret_cast<HDROP>(w);
            std::wstring file(DragQueryFileW(drop, 0, nullptr, 0) + 1, L'\0');
            DragQueryFileW(drop, 0, file.data(), static_cast<UINT>(file.size()));
            DragFinish(drop);
            file.pop_back();
            if (fs::is_directory(file)) open(file);
            else load(file);
            return 0;
        }
        case WM_DRAWITEM: {
            const auto& item = *reinterpret_cast<const DRAWITEMSTRUCT*>(l);
            if (item.CtlType == ODT_BUTTON) paintButton(item);
            else paintChoice(item);
            return TRUE;
        }
        case WM_MEASUREITEM: {
            auto& item = *reinterpret_cast<MEASUREITEMSTRUCT*>(l);
            item.itemHeight = item.CtlType == ODT_LISTBOX ? 30 : 24;
            return TRUE;
        }
        case WM_NOTIFY: {
            const auto& header = *reinterpret_cast<const NMHDR*>(l);
            if (header.code == NM_CUSTOMDRAW && (header.idFrom == Seek || header.idFrom == Volume)) {
                return paintTrack(*reinterpret_cast<const NMCUSTOMDRAW*>(l), header.idFrom == Volume ? violet : accent);
            }
            break;
        }
        case WM_ERASEBKGND: {
            RECT area{};
            GetClientRect(window_, &area);
            const auto dc = reinterpret_cast<HDC>(w);
            FillRect(dc, &area, brush_);
            rule(dc, 16, area.right - 16, 44);
            fill(dc, {326, 52, 327, area.bottom - 35}, edge);
            outline(dc, {15, 119, 322, area.bottom - 33}, edge);
            outline(dc, {332, 76, area.right - 15, area.bottom - 154}, violet);
            rule(dc, 16, area.right - 16, area.bottom - 28, 1);
            return 1;
        }
        case WM_CTLCOLORSTATIC: {
            auto dc = reinterpret_cast<HDC>(w);
            const auto label = reinterpret_cast<HWND>(l);
            SetTextColor(dc, label == clock_ || label == title_ ? accent :
                label == status_ || label == location_ || label == geometry_ ? muted : textColor);
            SetBkColor(dc, background);
            return reinterpret_cast<LRESULT>(brush_);
        }
        case WM_CTLCOLORLISTBOX: {
            const auto dc = reinterpret_cast<HDC>(w);
            SetTextColor(dc, textColor);
            SetBkColor(dc, panelColor);
            return reinterpret_cast<LRESULT>(panelBrush_);
        }
        case WM_DESTROY: KillTimer(window_, 1); PostQuitMessage(0); return 0;
        }
        return DefWindowProcW(window_, event, w, l);
    }
};

int testMedia(const fs::path& file) {
    Renderer renderer;
    Media media(renderer.device(), true);
    media.load(file);
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    while (!media.ready() && media.error().empty() && std::chrono::steady_clock::now() < limit) {
        media.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    if (!media.ready()) throw std::runtime_error("Aucune image décodée : " + fs::path(media.error()).u8string());
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = 64;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> image, read;
    ComPtr<ID3D11RenderTargetView> target;
    checked(renderer.device()->CreateTexture2D(&desc, nullptr, &image), "Test de la texture");
    checked(renderer.device()->CreateRenderTargetView(image.Get(), nullptr, &target), "Test de l’aperçu");
    desc.BindFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    checked(renderer.device()->CreateTexture2D(&desc, nullptr, &read), "Test de la lecture graphique");
    const auto pixel = [&](const ViewSettings& view, int eye) {
        renderer.draw(target.Get(), 64, 64, {}, media, view, {0, 0, 0, 1}, eye, false);
        renderer.context()->CopyResource(read.Get(), image.Get());
        D3D11_MAPPED_SUBRESOURCE pixels{};
        checked(renderer.context()->Map(read.Get(), 0, D3D11_MAP_READ, 0, &pixels), "Test des pixels");
        const auto* rgb = static_cast<const uint8_t*>(pixels.pData) + 32 * pixels.RowPitch + 32 * 4;
        int value = (rgb[0] << 16) | (rgb[1] << 8) | rgb[2];
        renderer.context()->Unmap(read.Get(), 0);
        return value;
    };
    const auto color = [&](Projection projection, Stereo stereo, bool swap, int eye) {
        return pixel({projection, stereo, swap, 1}, eye);
    };
    std::wostringstream result;
    result << file.filename().wstring() << L" decoded, aspect=" << media.aspect() << L", pixels(hex)=" << std::hex
        << color(Projection::Flat, Stereo::Mono, false, 0) << L" "
        << color(Projection::Equirect180, Stereo::SideBySide, false, 0) << L" "
        << color(Projection::Equirect180, Stereo::SideBySide, false, 1) << L" "
        << color(Projection::Equirect180, Stereo::SideBySide, true, 0) << L" "
        << color(Projection::Equirect360, Stereo::TopBottom, false, 0) << L" "
        << color(Projection::Equirect360, Stereo::TopBottom, false, 1) << L" "
        << color(Projection::Fisheye180, Stereo::Mono, false, 0);
    auto automatic = detectFormat(file);
    applyDecoderStereo(automatic, media.sourceStereo());
    if (!automatic.layoutKnown || !automatic.projectionKnown) {
        inspectStereoFrame(automatic, renderer.luma(media, stereoProbeWidth, stereoProbeHeight), media.aspect());
    }
    result << L" | Auto " << projectionName(automatic.view.projection) << L" · " << stereoName(automatic.view.stereo)
        << L" | yeux(hex)=" << pixel(automatic.view, 0) << L" " << pixel(automatic.view, 1);
    writeDiagnostic(result.str());
    return 0;
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    int count = 0;
    auto args = CommandLineToArgvW(GetCommandLineW(), &count);
    std::vector<std::wstring> options;
    if (args) {
        options.assign(args + 1, args + count);
        LocalFree(args);
    }
    const bool startInVr = options.empty() || options[0] != L"--bureau";
    if (!startInVr) options.erase(options.begin());
    const bool diagnostic = !options.empty() && options[0].rfind(L"--", 0) == 0;
    HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int result = 0;
    try {
        checked(com, "Initialisation de Windows");
        if (diagnostic) {
            if (options[0] == L"--probe") {
                Vr vr;
                Renderer renderer(&vr.adapter());
                writeDiagnostic(L"OpenXR : " + wide(vr.runtime().c_str()) + L" · casque et adaptateur Direct3D détectés");
            } else if (options[0] == L"--self-test" && options.size() == 2) {
                result = testMedia(options[1]);
            } else if (options[0] == L"--self-test-ui" && options.size() == 2) {
                TestBookmarksFile bookmarks;
                Player player(true, bookmarks.path);
                player.run(instance, SW_HIDE, fs::absolute(options[1]), true);
                writeDiagnostic(L"Tests sans casque : liste, défilement de la grille, miniatures, favoris, lecture, format VR et fermeture.");
            } else if (options[0] == L"--inspect-format" && options.size() == 2 && fs::is_regular_file(options[1])) {
                const auto format = detectFormat(options[1]);
                std::wostringstream report;
                report << projectionName(format.view.projection) << L", " << stereoName(format.view.stereo)
                    << L", champ=" << fieldOfView(format.view) << L" x " << format.view.verticalFov
                    << L", yeux inversés=" << format.view.swapEyes << L", calibration=" << format.cameraMesh;
                writeDiagnostic(report.str());
            } else throw std::runtime_error("Options : --probe, --self-test fichier, --self-test-ui fichier, --inspect-format fichier");
        } else {
            Player player;
            player.run(instance, show, options.empty() ? fs::path() : fs::path(options[0]), false, startInVr);
        }
    } catch (const std::exception& error) {
        if (diagnostic) writeDiagnostic(wide(error.what()));
        else MessageBoxW(nullptr, wide(error.what()).c_str(), appName, MB_ICONERROR);
        result = 1;
    }
    if (SUCCEEDED(com)) CoUninitialize();
    return result;
}
