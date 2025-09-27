#include "gui_browser.hpp"
#include "tune.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace {

// 安全小写函数（只对 ASCII 范围做）
static inline char ascii_tolower(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

// 安全扩展名大小写无关比较
static bool ends_with_ext_ci(const std::string& name, const char* ext) {
    const size_t n = name.size();
    const size_t m = std::strlen(ext);
    if (m == 0 || n < m) return false;
    for (size_t i = 0; i < m; ++i) {
        if (ascii_tolower(name[n - m + i]) != ascii_tolower(ext[i])) return false;
    }
    return true;
}

// 排序比较：直接用 UTF-8 字节顺序，避免 strcasecmp 异常
static bool ListItemTextLess(const tsl::elm::ListItem* a, const tsl::elm::ListItem* b) {
    return a->getText() < b->getText();
}
static bool StringTextLess(const std::string& a, const std::string& b) {
    return a < b;
}

// 支持的扩展名
constexpr const char* SupportedExts[] = {
#ifdef WANT_MP3
    ".mp3",
#endif
#ifdef WANT_FLAC
    ".flac",
#endif
#ifdef WANT_WAV
    ".wav", ".wave",
#endif
};

static bool SupportsType(const char* name) {
    std::string s(name);
    for (auto ext : SupportedExts) {
        if (ends_with_ext_ci(s, ext)) return true;
    }
    return false;
}

constexpr const char* const base_path = "/music/";
char path_buffer[FS_MAX_PATH];

} // namespace


BrowserGui::BrowserGui()
    : m_fs(), has_music(), cwd("/") {
    this->m_list = new tsl::elm::List();

    Result rc = fsOpenSdCardFileSystem(&this->m_fs);
    if (R_SUCCEEDED(rc)) {
        FsDir dir;
        std::strcpy(this->cwd, base_path);
        if (R_SUCCEEDED(fsFsOpenDirectory(&this->m_fs, this->cwd, FsDirOpenMode_ReadFiles, &dir))) {
            this->has_music = true;
            fsDirClose(&dir);
        } else {
            this->cwd[1] = '\0';
        }
        this->scanCwd();
    } else {
        this->m_list->addItem(new tsl::elm::CategoryHeader("Couldn't open SdCard"));
    }
}

BrowserGui::~BrowserGui() {
    fsFsClose(&this->m_fs);
}

tsl::elm::Element* BrowserGui::createUI() {
    m_frame = new SysTuneOverlayFrame();
    m_frame->setDescription("\uE0E1 Back   \uE0E0 Add   \uE0E2 Add All");
    m_frame->setContent(this->m_list);
    return m_frame;
}

bool BrowserGui::handleInput(u64 keysDown, u64, const HidTouchState&, HidAnalogStickState, HidAnalogStickState) {
    if (keysDown & HidNpadButton_B) {
        if (this->has_music && this->cwd[7] == '\0') {
            return false;
        } else if (this->cwd[1] != '\0') {
            this->upCwd();
            return true;
        }
    } else if (keysDown & HidNpadButton_X) {
        this->addAllToPlaylist();
        return true;
    }
    return false;
}

void BrowserGui::scanCwd() {
    tsl::Gui::removeFocus();
    this->m_list->clear();

    this->m_list->addItem(new tsl::elm::CategoryHeader(this->cwd, true));

    FsDir dir;
    Result rc = fsFsOpenDirectory(&this->m_fs, this->cwd, FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, &dir);
    if (R_FAILED(rc)) {
        char result_buffer[0x20];
        std::snprintf(result_buffer, sizeof(result_buffer), "Error: %03X-%04X", R_MODULE(rc), R_DESCRIPTION(rc));
        this->m_list->addItem(new tsl::elm::ListItem("Failed to open directory."));
        this->m_list->addItem(new tsl::elm::ListItem(result_buffer));
        return;
    }
    tsl::hlp::ScopeGuard dirGuard([&] { fsDirClose(&dir); });

    std::vector<tsl::elm::ListItem*> folders, files;

    s64 count = 0;
    FsDirectoryEntry entry;
    while (R_SUCCEEDED(fsDirRead(&dir, &count, 1, &entry)) && count) {
        if (entry.type == FsDirEntryType_Dir) {
            auto item = new tsl::elm::ListItem(entry.name);
            item->setClickListener([this, item](u64 down) -> bool {
                if (down & HidNpadButton_A) {
                    std::strncat(this->cwd, item->getText().c_str(), sizeof(this->cwd) - 1);
                    std::strncat(this->cwd, "/", sizeof(this->cwd) - 1);
                    this->scanCwd();
                    return true;
                }
                return false;
            });
            folders.push_back(item);
        } else if (SupportsType(entry.name)) {
            auto item = new tsl::elm::ListItem(entry.name);
            item->setClickListener([this, item](u64 down) -> bool {
                if (down & HidNpadButton_A) {
                    std::snprintf(path_buffer, sizeof(path_buffer), "%s%s", this->cwd, item->getText().c_str());
                    Result rc = tuneEnqueue(path_buffer, TuneEnqueueType_Back);
                    if (R_SUCCEEDED(rc)) {
                        m_frame->setToast("Playlist updated", "Added 1 song.");
                    } else {
                        m_frame->setToast("Failed to add track.", "Path may contain unsupported characters.");
                    }
                    return true;
                }
                return false;
            });
            files.push_back(item);
        }
    }

    if (folders.empty() && files.empty()) {
        this->m_list->addItem(new tsl::elm::CategoryHeader("Empty..."));
        return;
    }

    if (!folders.empty()) {
        std::sort(folders.begin(), folders.end(), ListItemTextLess);
        for (auto element : folders)
            this->m_list->addItem(element);
    }
    if (!files.empty()) {
        this->m_list->addItem(new tsl::elm::CategoryHeader("Files"));
        std::sort(files.begin(), files.end(), ListItemTextLess);
        for (auto element : files)
            this->m_list->addItem(element);
    }
}

void BrowserGui::upCwd() {
    size_t length = std::strlen(this->cwd);
    if (length <= 1) return;

    for (size_t i = length - 2; i >= 0; i--) {
        if (this->cwd[i] == '/') {
            this->cwd[i + 1] = '\0';
            this->scanCwd();
            return;
        }
        if (i == 0) break; // 防止 size_t underflow
    }
}

void BrowserGui::addAllToPlaylist() {
    FsDir dir;
    Result rc = fsFsOpenDirectory(&this->m_fs, this->cwd, FsDirOpenMode_ReadFiles, &dir);
    if (R_FAILED(rc)) {
        char result_buffer[0x20];
        std::snprintf(result_buffer, sizeof(result_buffer), "Error: %03X-%04X", R_MODULE(rc), R_DESCRIPTION(rc));
        this->m_list->addItem(new tsl::elm::ListItem("Failed to open directory."));
        this->m_list->addItem(new tsl::elm::ListItem(result_buffer));
        return;
    }
    tsl::hlp::ScopeGuard dirGuard([&] { fsDirClose(&dir); });

    std::vector<std::string> file_list;
    s64 songs_added = 0;
    s64 count = 0;
    FsDirectoryEntry entry;
    while (R_SUCCEEDED(fsDirRead(&dir, &count, 1, &entry)) && count) {
        if (entry.type == FsDirEntryType_File && SupportsType(entry.name)) {
            file_list.emplace_back(entry.name);
            count++;
        }
    }

    std::sort(file_list.begin(), file_list.end(), StringTextLess);
    for (auto const& file : file_list) {
        std::snprintf(path_buffer, sizeof(path_buffer), "%s%s", this->cwd, file.c_str());
        rc = tuneEnqueue(path_buffer, TuneEnqueueType_Back);
        if (R_SUCCEEDED(rc)) songs_added++;
    }

    std::snprintf(path_buffer, sizeof(path_buffer), "Added %lld songs.", songs_added);
    m_frame->setToast("Playlist updated", path_buffer);
}
