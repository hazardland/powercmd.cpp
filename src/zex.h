// MODULE: zex
// Purpose : ZEX dual-panel explorer using a dedicated console screen buffer
// Exports : struct Entry | struct Panel | struct ZexState | zex_toggle()
// Depends : common.h, terminal.h, info.h

#include <set>
#include <map>
#include <cstdint>
#include <shellapi.h>

struct Entry {
    std::wstring name;
    bool         is_dir    = false;
    bool         is_hidden = false;
    uint64_t     size      = 0;
    FILETIME     modified  = {};
};

struct Panel {
    std::wstring       cwd;
    std::vector<Entry> all_entries;
    std::vector<Entry> entries;
    std::wstring       filter;
    int                filter_cursor = 0;
    int                cursor = 0;
    int                scroll = 0;
    std::set<std::wstring> selected;
    int                sort_mode = 0;
    bool               active = false;
};

enum zex_sort_mode {
    zex_sort_name = 0,
    zex_sort_ext,
    zex_sort_size,
    zex_sort_time,
    zex_sort_count,
};

enum zex_focus_mode {
    zex_focus_panel,
    zex_focus_filter,
    zex_focus_dialog,
};

enum zex_dialog_kind {
    zex_dialog_none,
    zex_dialog_mkdir,
    zex_dialog_copy,
    zex_dialog_move,
    zex_dialog_recycle,
    zex_dialog_delete,
    zex_dialog_overwrite,
    zex_dialog_info,
    zex_dialog_progress,
};

struct ZexDialog {
    zex_dialog_kind kind         = zex_dialog_none;
    bool         visible      = false;
    std::wstring title;
    std::wstring summary;
    std::wstring detail;
    std::wstring input_label;
    std::wstring input_value;
    int          input_cursor = 0;
    int          progress_current = 0;
    int          progress_total   = 0;
};

enum zex_copy_task_kind {
    zex_copy_task_mkdir,
    zex_copy_task_file,
    zex_copy_task_rmdir,
};

struct ZexCopyTask {
    zex_copy_task_kind kind = zex_copy_task_file;
    std::wstring       src;
    std::wstring       dst;
    std::wstring       source_key;
};

struct ZexCopyState {
    bool                     active         = false;
    bool                     move_mode      = false;
    bool                     overwrite_all  = false;
    bool                     overwrite_once = false;
    bool                     source_left    = true;
    std::vector<std::wstring> sources;
    std::vector<ZexCopyTask> tasks;
    std::map<std::wstring, size_t> pending;
    size_t                   index          = 0;
};

struct ZexDeleteState {
    bool                     active        = false;
    bool                     recycle_mode  = true;
    bool                     source_left   = true;
    std::vector<std::wstring> sources;
    size_t                   index         = 0;
};

struct ZexState {
    Panel               left;
    Panel               right;
    zex_focus_mode      focus         = zex_focus_panel;
    bool                filter_replace = false;
    std::wstring        jump_buffer;
    ULONGLONG           jump_tick     = 0;
    ZexDialog           dialog;
    ZexCopyState        copy;
    ZexDeleteState      del;
    HANDLE              zex_buf       = NULL;
    HANDLE              shell_buf     = NULL;
    CONSOLE_CURSOR_INFO shell_cursor  = {25, TRUE};
    DWORD               shell_out_mode = 0;
    int                 view_width    = 0;
    int                 view_height   = 0;
    bool                ready         = false;
};

// -- ZEX color config ---------------------------------------------------------
// ZEX uses direct console attributes in its private screen buffer, so these are
// WORD attributes instead of ANSI escape strings.
#define ZEX_BORDER_ACTIVE   (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY)
#define ZEX_BORDER_INACTIVE (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE)
#define ZEX_PATH            (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY)
#define ZEX_FILTER          (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY)
#define ZEX_FILTER_BG       (BACKGROUND_BLUE | BACKGROUND_INTENSITY)
#define ZEX_CURSOR_BG       (BACKGROUND_RED | BACKGROUND_GREEN | BACKGROUND_INTENSITY)
#define ZEX_CURSOR_SELECTED (ZEX_CURSOR_BG | FOREGROUND_BLUE | FOREGROUND_INTENSITY)
#define ZEX_SELECTED        (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY)
#define ZEX_DOTDOT          (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE)
#define ZEX_STATUSBAR       (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE)
#define ZEX_STATUS_KEY      (BACKGROUND_BLUE | BACKGROUND_INTENSITY)
#define ZEX_STATUS_TEXT     (BACKGROUND_INTENSITY)
#define ZEX_BADGE           (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY)
#define ZEX_DIALOG_BORDER   ZEX_BORDER_ACTIVE
#define ZEX_DIALOG_FILL     ZEX_STATUSBAR
#define ZEX_DIALOG_TITLE    ZEX_PATH
#define ZEX_DIALOG_TEXT     ZEX_PATH
#define ZEX_DIALOG_LABEL    ZEX_BADGE
#define ZEX_DIALOG_INPUT    ZEX_PATH
#define ZEX_DIALOG_CURSOR   (BACKGROUND_BLUE | BACKGROUND_INTENSITY)
#define ZEX_COLOR_DIR       (FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY)
#define ZEX_COLOR_EXE       (FOREGROUND_GREEN | FOREGROUND_INTENSITY)
#define ZEX_COLOR_ARCHIVE   (FOREGROUND_RED | FOREGROUND_INTENSITY)
#define ZEX_COLOR_IMAGE     (FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_INTENSITY)
#define ZEX_COLOR_MEDIA     (FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY)
#define ZEX_COLOR_HIDDEN    (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE)
#define ZEX_PROG_LOW        ZEX_COLOR_DIR
#define ZEX_PROG_MID        ZEX_BADGE
#define ZEX_PROG_HIGH       ZEX_COLOR_ARCHIVE
// -----------------------------------------------------------------------------

static ZexState g_zex;

int edit_file(const std::string& path);
int view_file(const std::string& path);

static int zex_total_rows(const Panel& panel);
static int zex_visible_rows();
static void zex_clamp_panel(Panel& panel);
static bool zex_panel_has_filter_row(const Panel& panel);
static bool zex_any_filter_row();
static int zex_separator_y();
static int zex_entries_y();
static void zex_jump_clear();
static void zex_sync_panels_from_shell();
static void zex_focus_entry_name(Panel& panel, const std::wstring& name);
static Panel& zex_active_panel();
static Panel& zex_inactive_panel();
static std::vector<std::wstring> zex_copy_sources();
static bool zex_copy_build_tasks(const std::vector<std::wstring>& sources, const std::wstring& raw_dest,
    std::vector<ZexCopyTask>& tasks, std::map<std::wstring, size_t>& pending, std::wstring& error);
static bool zex_move_build_tasks(const std::vector<std::wstring>& sources, const std::wstring& raw_dest,
    std::vector<ZexCopyTask>& tasks, std::map<std::wstring, size_t>& pending, std::wstring& error);
static void zex_copy_run();
static void zex_delete_run();
static void zex_copy_refresh_panels();

static const ULONGLONG ZEX_JUMP_TIMEOUT_MS = 1000;

static bool zex_filter_focused() {
    return g_zex.focus == zex_focus_filter;
}

static bool zex_dialog_focused() {
    return g_zex.focus == zex_focus_dialog && g_zex.dialog.visible;
}

static bool zex_read_view_size(HANDLE buf, int& width, int& height) {
    CONSOLE_SCREEN_BUFFER_INFO csbi = {};
    if (!buf || buf == INVALID_HANDLE_VALUE) return false;
    if (!GetConsoleScreenBufferInfo(buf, &csbi)) return false;
    width = csbi.srWindow.Right - csbi.srWindow.Left + 1;
    height = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    return width > 0 && height > 0;
}

static void zex_cache_view_size() {
    int width = 0, height = 0;
    if (zex_read_view_size(g_zex.zex_buf, width, height)) {
        g_zex.view_width = width;
        g_zex.view_height = height;
    }
}

static bool zex_sync_view() {
    CONSOLE_SCREEN_BUFFER_INFO csbi = {};
    if (!g_zex.zex_buf || g_zex.zex_buf == INVALID_HANDLE_VALUE) return false;
    if (!GetConsoleScreenBufferInfo(g_zex.zex_buf, &csbi)) return false;

    int view_width = csbi.srWindow.Right - csbi.srWindow.Left + 1;
    int view_height = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    if (view_width <= 0 || view_height <= 0) return false;

    if (view_width > csbi.dwSize.X || view_height > csbi.dwSize.Y) {
        COORD grown = {
            (SHORT)std::max<int>(csbi.dwSize.X, view_width),
            (SHORT)std::max<int>(csbi.dwSize.Y, view_height)
        };
        SetConsoleScreenBufferSize(g_zex.zex_buf, grown);
    }

    bool changed = view_width != g_zex.view_width || view_height != g_zex.view_height;
    g_zex.view_width = view_width;
    g_zex.view_height = view_height;
    return changed;
}

static std::wstring zex_current_dir() {
    wchar_t buf[MAX_PATH] = {};
    GetCurrentDirectoryW(MAX_PATH, buf);
    return buf;
}

static std::wstring zex_display_path(const std::wstring& path) {
    std::wstring out = path;
    std::replace(out.begin(), out.end(), L'\\', L'/');
    return out;
}

static std::wstring zex_lower(const std::wstring& s) {
    std::wstring out = s;
    std::transform(out.begin(), out.end(), out.begin(), ::towlower);
    return out;
}

static std::wstring zex_fit(const std::wstring& s, int width) {
    if (width <= 0) return L"";
    if ((int)s.size() <= width) return s;
    if (width <= 3) return std::wstring(width, L'.');
    return s.substr(0, width - 3) + L"...";
}

static std::wstring zex_trim(const std::wstring& s) {
    size_t start = 0;
    while (start < s.size() && iswspace(s[start]))
        start++;
    size_t end = s.size();
    while (end > start && iswspace(s[end - 1]))
        end--;
    return s.substr(start, end - start);
}

static std::wstring zex_glob_pattern(const std::wstring& dir) {
    if (dir.empty()) return L"*";
    wchar_t last = dir.back();
    if (last == L'\\' || last == L'/') return dir + L"*";
    return dir + L"\\*";
}

static std::wstring zex_join_path(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    wchar_t last = dir.back();
    if (last == L'\\' || last == L'/') return dir + name;
    return dir + L"\\" + name;
}

static bool zex_is_drive_root(const std::wstring& path) {
    if (path.size() != 3) return false;
    return iswalpha(path[0]) && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/');
}

static std::wstring zex_parent_dir(const std::wstring& path) {
    if (path.empty()) return path;

    std::wstring norm = path;
    std::replace(norm.begin(), norm.end(), L'/', L'\\');
    while (norm.size() > 3 && !norm.empty() && norm.back() == L'\\')
        norm.pop_back();
    if (zex_is_drive_root(norm)) return norm;

    size_t slash = norm.find_last_of(L'\\');
    if (slash == std::wstring::npos) return norm;
    if (slash == 2 && iswalpha(norm[0]) && norm[1] == L':')
        return norm.substr(0, 3);
    if (slash == 0)
        return norm.substr(0, 1);
    return norm.substr(0, slash);
}

static std::wstring zex_leaf_name(const std::wstring& path) {
    if (path.empty()) return L"";
    std::wstring norm = path;
    std::replace(norm.begin(), norm.end(), L'/', L'\\');
    while (norm.size() > 3 && !norm.empty() && norm.back() == L'\\')
        norm.pop_back();
    size_t slash = norm.find_last_of(L'\\');
    if (slash == std::wstring::npos) return norm;
    return norm.substr(slash + 1);
}

static std::wstring zex_entry_ext(const std::wstring& name) {
    size_t dot = name.rfind(L'.');
    if (dot == std::wstring::npos || dot == 0 || dot + 1 >= name.size())
        return L"";
    return zex_lower(name.substr(dot + 1));
}

static std::wstring zex_native_path(const std::wstring& path) {
    std::wstring out = path;
    std::replace(out.begin(), out.end(), L'/', L'\\');
    return out;
}

static bool zex_is_absolute_path(const std::wstring& path) {
    if (path.size() > 1 && path[1] == L':')
        return true;
    if (path.size() > 1 && path[0] == L'\\' && path[1] == L'\\')
        return true;
    if (!path.empty() && (path[0] == L'\\' || path[0] == L'/'))
        return true;
    return false;
}

static bool zex_trailing_slash(const std::wstring& path) {
    return !path.empty() && (path.back() == L'\\' || path.back() == L'/');
}

static DWORD zex_path_attrs(const std::wstring& path) {
    return GetFileAttributesW(path.c_str());
}

static bool zex_path_exists(const std::wstring& path) {
    return zex_path_attrs(path) != INVALID_FILE_ATTRIBUTES;
}

static bool zex_path_is_dir(const std::wstring& path) {
    DWORD attrs = zex_path_attrs(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static bool zex_same_path(const std::wstring& a, const std::wstring& b) {
    wchar_t a_buf[MAX_PATH] = {};
    wchar_t b_buf[MAX_PATH] = {};
    DWORD a_len = GetFullPathNameW(a.c_str(), MAX_PATH, a_buf, NULL);
    DWORD b_len = GetFullPathNameW(b.c_str(), MAX_PATH, b_buf, NULL);
    if (a_len == 0 || b_len == 0 || a_len >= MAX_PATH || b_len >= MAX_PATH)
        return zex_lower(zex_native_path(a)) == zex_lower(zex_native_path(b));
    return zex_lower(a_buf) == zex_lower(b_buf);
}

static bool zex_path_starts_with(const std::wstring& child, const std::wstring& parent) {
    std::wstring c = zex_lower(zex_native_path(child));
    std::wstring p = zex_lower(zex_native_path(parent));
    if (!p.empty() && p.back() != L'\\') p += L'\\';
    return c.size() >= p.size() && c.substr(0, p.size()) == p;
}

static bool zex_ensure_dir(const std::wstring& path) {
    if (path.empty()) return false;
    std::wstring norm = zex_native_path(path);
    if (zex_path_is_dir(norm)) return true;

    std::wstring parent = zex_parent_dir(norm);
    if (!parent.empty() && parent != norm && !zex_path_is_dir(parent)) {
        if (!zex_ensure_dir(parent)) return false;
    }

    if (CreateDirectoryW(norm.c_str(), NULL))
        return true;
    return GetLastError() == ERROR_ALREADY_EXISTS && zex_path_is_dir(norm);
}

static std::wstring zex_last_error_text(DWORD code) {
    wchar_t* buf = NULL;
    DWORD len = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, code, 0, (LPWSTR)&buf, 0, NULL);
    std::wstring out = len ? std::wstring(buf, len) : L"Operation failed";
    if (buf) LocalFree(buf);
    while (!out.empty() && (out.back() == L'\r' || out.back() == L'\n' || out.back() == L' '))
        out.pop_back();
    return out;
}

static bool zex_replace_file(const std::wstring& src, const std::wstring& dst) {
    DWORD attrs = zex_path_attrs(dst);
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) {
        SetFileAttributesW(dst.c_str(), attrs & ~(FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM));
    }
    if (zex_path_exists(dst) && !DeleteFileW(dst.c_str()))
        return false;
    return CopyFileW(src.c_str(), dst.c_str(), TRUE) != 0;
}

static bool zex_move_file(const std::wstring& src, const std::wstring& dst, bool overwrite) {
    DWORD flags = MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH;
    if (overwrite)
        flags |= MOVEFILE_REPLACE_EXISTING;

    if (MoveFileExW(src.c_str(), dst.c_str(), flags))
        return true;

    if (!overwrite)
        return false;

    DWORD attrs = zex_path_attrs(dst);
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)))
        SetFileAttributesW(dst.c_str(), attrs & ~(FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM));
    if (zex_path_exists(dst) && !DeleteFileW(dst.c_str()))
        return false;
    return MoveFileExW(src.c_str(), dst.c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH) != 0;
}

static bool zex_has_glob(const std::wstring& pattern) {
    return pattern.find(L'*') != std::wstring::npos || pattern.find(L'?') != std::wstring::npos;
}

static bool zex_glob_match(const std::wstring& text, const std::wstring& pattern) {
    size_t ti = 0, pi = 0;
    size_t star = std::wstring::npos;
    size_t match = 0;

    while (ti < text.size()) {
        if (pi < pattern.size() && (pattern[pi] == L'?' || pattern[pi] == text[ti])) {
            ti++;
            pi++;
            continue;
        }
        if (pi < pattern.size() && pattern[pi] == L'*') {
            star = pi++;
            match = ti;
            continue;
        }
        if (star != std::wstring::npos) {
            pi = star + 1;
            ti = ++match;
            continue;
        }
        return false;
    }

    while (pi < pattern.size() && pattern[pi] == L'*')
        pi++;
    return pi == pattern.size();
}

static std::wstring zex_entry_key(const Panel& panel, const Entry& entry) {
    return zex_join_path(panel.cwd, entry.name);
}

static std::wstring zex_sort_mode_label(int sort_mode) {
    switch (sort_mode) {
    case zex_sort_ext:  return L"Ext";
    case zex_sort_size: return L"Size";
    case zex_sort_time: return L"Time";
    default:            return L"Name";
    }
}

static bool zex_entry_less(const Entry& a, const Entry& b, int sort_mode) {
    std::wstring la = zex_lower(a.name);
    std::wstring lb = zex_lower(b.name);
    if (sort_mode == zex_sort_ext) {
        std::wstring ea = zex_entry_ext(a.name);
        std::wstring eb = zex_entry_ext(b.name);
        if (ea != eb) return ea < eb;
        if (la != lb) return la < lb;
        return a.name < b.name;
    }
    if (sort_mode == zex_sort_size) {
        if (a.size != b.size) return a.size > b.size;
        if (la != lb) return la < lb;
        return a.name < b.name;
    }
    if (sort_mode == zex_sort_time) {
        LONG hi = CompareFileTime(&a.modified, &b.modified);
        if (hi != 0) return hi > 0;
        if (la != lb) return la < lb;
        return a.name < b.name;
    }
    if (la != lb) return la < lb;
    return a.name < b.name;
}

static void zex_apply_filter(Panel& panel) {
    panel.entries.clear();
    if (panel.filter.empty()) {
        panel.entries = panel.all_entries;
    } else {
        std::wstring needle = zex_lower(panel.filter);
        for (const Entry& entry : panel.all_entries) {
            std::wstring name = zex_lower(entry.name);
            bool match = zex_has_glob(needle)
                ? zex_glob_match(name, needle)
                : name.find(needle) != std::wstring::npos;
            if (match)
                panel.entries.push_back(entry);
        }
    }
}

static void zex_load_entries(Panel& panel, bool reset_cursor, bool clear_selection = true) {
    panel.all_entries.clear();
    panel.entries.clear();
    if (clear_selection)
        panel.selected.clear();

    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW(zex_glob_pattern(panel.cwd).c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        std::vector<Entry> dirs;
        std::vector<Entry> files;
        do {
            std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;

            bool is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            bool hidden = (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0 ||
                          (!name.empty() && name[0] == L'.');
            uint64_t size = is_dir ? 0 : (((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow);
            Entry entry = {name, is_dir, hidden, size, fd.ftLastWriteTime};
            (is_dir ? dirs : files).push_back(entry);
        } while (FindNextFileW(h, &fd));
        FindClose(h);

        auto cmp = [&](const Entry& a, const Entry& b) {
            return zex_entry_less(a, b, panel.sort_mode);
        };
        std::sort(dirs.begin(), dirs.end(), cmp);
        std::sort(files.begin(), files.end(), cmp);

        panel.all_entries.reserve(dirs.size() + files.size());
        panel.all_entries.insert(panel.all_entries.end(), dirs.begin(), dirs.end());
        panel.all_entries.insert(panel.all_entries.end(), files.begin(), files.end());
    }

    if (!clear_selection) {
        std::set<std::wstring> valid;
        for (const Entry& entry : panel.all_entries)
            valid.insert(zex_entry_key(panel, entry));

        for (auto it = panel.selected.begin(); it != panel.selected.end();) {
            if (!valid.count(*it)) it = panel.selected.erase(it);
            else ++it;
        }
    }

    zex_apply_filter(panel);
    if (reset_cursor) {
        panel.cursor = 0;
        panel.scroll = 0;
    }
    zex_clamp_panel(panel);
}

static WORD zex_entry_color(const Entry& e) {
    if (e.is_hidden) return ZEX_COLOR_HIDDEN;
    if (e.is_dir) return ZEX_COLOR_DIR;

    size_t dot = e.name.rfind(L'.');
    if (dot == std::wstring::npos) return ZEX_PATH;

    std::wstring ext = zex_lower(e.name.substr(dot));
    if (ext == L".exe" || ext == L".bat" || ext == L".cmd" || ext == L".ps1" || ext == L".msi")
        return ZEX_COLOR_EXE;
    if (ext == L".zip" || ext == L".tar" || ext == L".gz" || ext == L".tgz" ||
        ext == L".bz2" || ext == L".xz" || ext == L".7z" || ext == L".rar" ||
        ext == L".cab" || ext == L".iso")
        return ZEX_COLOR_ARCHIVE;
    if (ext == L".jpg" || ext == L".jpeg" || ext == L".png" || ext == L".gif" ||
        ext == L".bmp" || ext == L".svg" || ext == L".webp" || ext == L".ico")
        return ZEX_COLOR_IMAGE;
    if (ext == L".mp3" || ext == L".wav" || ext == L".ogg" || ext == L".flac" ||
        ext == L".aac" || ext == L".m4a" || ext == L".mp4" || ext == L".mkv" ||
        ext == L".avi" || ext == L".mov" || ext == L".webm")
        return ZEX_COLOR_MEDIA;
    return ZEX_PATH;
}

static int zex_total_rows(const Panel& panel) {
    return 1 + (int)panel.entries.size(); // ".." + entries
}

static bool zex_panel_has_filter_row(const Panel& panel) {
    return !panel.filter.empty() || (panel.active && zex_filter_focused());
}

static bool zex_any_filter_row() {
    return zex_panel_has_filter_row(g_zex.left) || zex_panel_has_filter_row(g_zex.right);
}

static int zex_separator_y() {
    return zex_any_filter_row() ? 3 : 2;
}

static int zex_entries_y() {
    return zex_separator_y() + 1;
}

static int zex_visible_rows() {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (!GetConsoleScreenBufferInfo(g_zex.zex_buf, &csbi)) return 0;
    int height = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    return std::max(0, height - (zex_entries_y() + 2));
}

static void zex_clamp_panel(Panel& panel) {
    int total = std::max(1, zex_total_rows(panel));
    if (panel.cursor < 0) panel.cursor = 0;
    if (panel.cursor >= total) panel.cursor = total - 1;

    int visible = std::max(1, zex_visible_rows());
    if (panel.scroll > panel.cursor) panel.scroll = panel.cursor;
    if (panel.cursor >= panel.scroll + visible) panel.scroll = panel.cursor - visible + 1;
    int max_scroll = std::max(0, total - visible);
    if (panel.scroll < 0) panel.scroll = 0;
    if (panel.scroll > max_scroll) panel.scroll = max_scroll;
}

static void zex_init_state() {
    if (g_zex.ready) return;

    std::wstring start = zex_current_dir();
    g_zex.left.cwd = start;
    g_zex.right.cwd = start;
    g_zex.left.active = true;
    g_zex.right.active = false;
    zex_load_entries(g_zex.left, true);
    zex_load_entries(g_zex.right, true);
    g_zex.ready = true;
}

static void zex_close_buffer() {
    if (g_zex.zex_buf && g_zex.zex_buf != INVALID_HANDLE_VALUE) {
        CloseHandle(g_zex.zex_buf);
    }
    g_zex.zex_buf = NULL;
    g_zex.view_width = 0;
    g_zex.view_height = 0;
}

static bool zex_resize_buffer(int width, int height) {
    if (width < 20 || height < 8) return false;

    zex_close_buffer();

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    g_zex.zex_buf = CreateConsoleScreenBuffer(
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &sa,
        CONSOLE_TEXTMODE_BUFFER,
        NULL
    );
    if (!g_zex.zex_buf || g_zex.zex_buf == INVALID_HANDLE_VALUE) return false;

    SetConsoleMode(g_zex.zex_buf, g_zex.shell_out_mode);

    COORD size = {(SHORT)width, (SHORT)height};
    SetConsoleScreenBufferSize(g_zex.zex_buf, size);

    SMALL_RECT rect = {0, 0, (SHORT)(width - 1), (SHORT)(height - 1)};
    SetConsoleWindowInfo(g_zex.zex_buf, TRUE, &rect);

    CONSOLE_CURSOR_INFO hidden = {1, FALSE};
    SetConsoleCursorInfo(g_zex.zex_buf, &hidden);
    g_zex.view_width = width;
    g_zex.view_height = height;
    return true;
}

static bool zex_ensure_buffer() {
    if (!g_zex.shell_buf) g_zex.shell_buf = out_h;
    if (g_zex.shell_buf == NULL || g_zex.shell_buf == INVALID_HANDLE_VALUE) return false;
    GetConsoleMode(g_zex.shell_buf, &g_zex.shell_out_mode);

    int width = 0, height = 0;
    if (!zex_read_view_size(g_zex.shell_buf, width, height)) return false;
    if (!zex_resize_buffer(width, height)) return false;
    return true;
}

static void zex_put(std::vector<wchar_t>& chars, std::vector<WORD>& attrs,
    int width, int height, int x, int y, wchar_t ch, WORD attr) {
    if (x < 0 || y < 0 || x >= width || y >= height) return;
    size_t idx = (size_t)y * width + x;
    chars[idx] = ch;
    attrs[idx] = attr;
}

static void zex_fill(std::vector<wchar_t>& chars, std::vector<WORD>& attrs,
    int width, int height, int x, int y, int len, wchar_t ch, WORD attr) {
    for (int i = 0; i < len; i++)
        zex_put(chars, attrs, width, height, x + i, y, ch, attr);
}

static void zex_text(std::vector<wchar_t>& chars, std::vector<WORD>& attrs,
    int width, int height, int x, int y, const std::wstring& text, WORD attr) {
    for (int i = 0; i < (int)text.size(); i++)
        zex_put(chars, attrs, width, height, x + i, y, text[i], attr);
}

static std::wstring zex_status_pad(const std::wstring& label) {
    const int width = 4;
    std::wstring padded = L" " + label + L" ";
    if ((int)padded.size() >= width) return padded;
    return padded + std::wstring(width - (int)padded.size(), L' ');
}

static int zex_status_item(std::vector<wchar_t>& chars, std::vector<WORD>& attrs,
    int width, int height, int x, int y, const std::wstring& key, const std::wstring& label) {
    if (x >= width) return x;

    zex_text(chars, attrs, width, height, x, y, key, ZEX_STATUS_KEY);
    x += (int)key.size();
    std::wstring padded = zex_status_pad(label);
    zex_text(chars, attrs, width, height, x, y, padded, ZEX_STATUS_TEXT);
    x += (int)padded.size();
    if (x < width) {
        zex_put(chars, attrs, width, height, x, y, L' ', ZEX_STATUSBAR);
        x++;
    }
    return x;
}

static void zex_box(std::vector<wchar_t>& chars, std::vector<WORD>& attrs,
    int width, int height, int left, int top, int box_w, int box_h, WORD border_attr, WORD fill_attr) {
    if (box_w <= 1 || box_h <= 1) return;
    for (int y = top; y < top + box_h; y++)
        zex_fill(chars, attrs, width, height, left, y, box_w, L' ', fill_attr);

    zex_put(chars, attrs, width, height, left, top, L'\x250C', border_attr);
    zex_put(chars, attrs, width, height, left + box_w - 1, top, L'\x2510', border_attr);
    zex_put(chars, attrs, width, height, left, top + box_h - 1, L'\x2514', border_attr);
    zex_put(chars, attrs, width, height, left + box_w - 1, top + box_h - 1, L'\x2518', border_attr);
    if (box_w > 2) {
        zex_fill(chars, attrs, width, height, left + 1, top, box_w - 2, L'\x2500', border_attr);
        zex_fill(chars, attrs, width, height, left + 1, top + box_h - 1, box_w - 2, L'\x2500', border_attr);
    }
    for (int y = top + 1; y < top + box_h - 1; y++) {
        zex_put(chars, attrs, width, height, left, y, L'\x2502', border_attr);
        zex_put(chars, attrs, width, height, left + box_w - 1, y, L'\x2502', border_attr);
    }
}

static std::wstring zex_dialog_tail(const std::wstring& value, int width, int cursor, int& start_out) {
    start_out = 0;
    if (width <= 0) return L"";
    if ((int)value.size() <= width) return value;
    start_out = std::max(0, cursor - width + 1);
    if (start_out + width > (int)value.size())
        start_out = (int)value.size() - width;
    return value.substr(start_out, width);
}

static void zex_draw_dialog(std::vector<wchar_t>& chars, std::vector<WORD>& attrs, int width, int height) {
    if (!g_zex.dialog.visible) return;

    int dialog_w = std::min(width, std::max(36, std::min(width - 4, 64)));
    bool input_dialog = g_zex.dialog.kind == zex_dialog_copy ||
        g_zex.dialog.kind == zex_dialog_move ||
        g_zex.dialog.kind == zex_dialog_mkdir;
    int dialog_h = std::min(height, input_dialog ? 8 : 7);
    if (dialog_w <= 2 || dialog_h <= 2) return;

    int left = std::max(0, (width - dialog_w) / 2);
    int top  = std::max(0, (height - dialog_h) / 2);
    zex_box(chars, attrs, width, height, left, top, dialog_w, dialog_h, ZEX_DIALOG_BORDER, ZEX_DIALOG_FILL);

    zex_text(chars, attrs, width, height, left + 2, top, zex_fit(g_zex.dialog.title, dialog_w - 4), ZEX_DIALOG_TITLE);
    zex_text(chars, attrs, width, height, left + 2, top + 2,
        zex_fit(g_zex.dialog.summary, dialog_w - 4), ZEX_DIALOG_TEXT);
    if (!g_zex.dialog.detail.empty()) {
        zex_text(chars, attrs, width, height, left + 2, top + 3,
            zex_fit(g_zex.dialog.detail, dialog_w - 4), ZEX_DIALOG_TEXT);
    }

    int footer_x = left + 2;
    int footer_y = top + dialog_h - 2;
    if (input_dialog) {
        zex_text(chars, attrs, width, height, left + 2, top + 3,
            zex_fit(g_zex.dialog.input_label, dialog_w - 4), ZEX_DIALOG_LABEL);

        int input_x = left + 2;
        int input_y = top + 4;
        int input_w = std::max(1, dialog_w - 4);
        zex_fill(chars, attrs, width, height, input_x, input_y, input_w, L' ', ZEX_DIALOG_INPUT);

        int start = 0;
        std::wstring visible = zex_dialog_tail(g_zex.dialog.input_value, input_w, g_zex.dialog.input_cursor, start);
        zex_text(chars, attrs, width, height, input_x, input_y, visible, ZEX_DIALOG_INPUT);

        int cursor_x = input_x + std::max(0, g_zex.dialog.input_cursor - start);
        if (cursor_x >= input_x + input_w) cursor_x = input_x + input_w - 1;
        wchar_t cursor_ch = L' ';
        if (g_zex.dialog.input_cursor >= start &&
            g_zex.dialog.input_cursor < start + (int)visible.size()) {
            cursor_ch = visible[g_zex.dialog.input_cursor - start];
        }
        zex_put(chars, attrs, width, height, cursor_x, input_y, cursor_ch, ZEX_DIALOG_CURSOR);

        footer_x = zex_status_item(chars, attrs, width, height, footer_x, footer_y, L"ENTER", L"Ok");
        zex_status_item(chars, attrs, width, height, footer_x, footer_y, L"ESC", L"Cancel");
    } else if (g_zex.dialog.kind == zex_dialog_overwrite) {
        footer_x = zex_status_item(chars, attrs, width, height, footer_x, footer_y, L"ENTER", L"Yes");
        footer_x = zex_status_item(chars, attrs, width, height, footer_x, footer_y, L"CTRL+ENTER", L"All");
        zex_status_item(chars, attrs, width, height, footer_x, footer_y, L"ESC", L"Cancel");
    } else if (g_zex.dialog.kind == zex_dialog_recycle || g_zex.dialog.kind == zex_dialog_delete) {
        footer_x = zex_status_item(chars, attrs, width, height, footer_x, footer_y, L"ENTER",
            g_zex.dialog.kind == zex_dialog_recycle ? L"Recycle" : L"Delete");
        zex_status_item(chars, attrs, width, height, footer_x, footer_y, L"ESC", L"Cancel");
    } else if (g_zex.dialog.kind == zex_dialog_progress) {
        int bar_x = left + 2;
        int bar_y = top + 4;
        int bar_w = std::max(8, dialog_w - 4);
        int total = std::max(1, g_zex.dialog.progress_total);
        int current = std::max(0, std::min(g_zex.dialog.progress_current, total));
        int filled = (bar_w * current) / total;
        zex_fill(chars, attrs, width, height, bar_x, bar_y, bar_w, L' ', ZEX_DIALOG_INPUT);
        if (filled > 0)
            zex_fill(chars, attrs, width, height, bar_x, bar_y, filled, L' ', ZEX_DIALOG_CURSOR);

        std::wstring percent = std::to_wstring((current * 100) / total) + L"%";
        int percent_x = bar_x + std::max(0, (bar_w - (int)percent.size()) / 2);
        zex_text(chars, attrs, width, height, percent_x, bar_y, percent, ZEX_DIALOG_TEXT);
        zex_text(chars, attrs, width, height, left + 2, footer_y,
            zex_fit(L"Working...", dialog_w - 4), ZEX_DIALOG_TEXT);
    } else {
        footer_x = zex_status_item(chars, attrs, width, height, footer_x, footer_y, L"ENTER", L"Close");
        zex_status_item(chars, attrs, width, height, footer_x, footer_y, L"ESC", L"Close");
    }
}

static void zex_draw_panel(std::vector<wchar_t>& chars, std::vector<WORD>& attrs,
    int width, int height, const Panel& panel, int left, int right) {
    int inner_x = left + 1;
    int inner_w = std::max(0, right - left - 1);
    if (inner_w <= 0) return;

    std::wstring path = zex_fit(zex_display_path(panel.cwd), inner_w);
    WORD head_attr = panel.active ? ZEX_PATH : ZEX_BORDER_INACTIVE;
    zex_text(chars, attrs, width, height, inner_x, 1, path, head_attr);
    std::wstring sort_badge = L"[" + zex_sort_mode_label(panel.sort_mode) + L"]";
    sort_badge = zex_fit(sort_badge, inner_w);
    int sort_x = inner_x + std::max(0, inner_w - (int)sort_badge.size());
    zex_text(chars, attrs, width, height, sort_x, 1, sort_badge, panel.active ? ZEX_BADGE : ZEX_BORDER_INACTIVE);

    if (zex_any_filter_row()) {
        int filter_y = zex_separator_y() - 1;
        WORD filter_attr = panel.active ? ZEX_PATH : ZEX_STATUSBAR;
        bool filter_focus = panel.active && zex_filter_focused();
        if (filter_focus) {
            int start = 0;
            std::wstring visible = zex_dialog_tail(panel.filter, inner_w, panel.filter_cursor, start);
            zex_text(chars, attrs, width, height, inner_x, filter_y, visible, filter_attr);

            int cursor_x = inner_x + std::max(0, panel.filter_cursor - start);
            if (cursor_x >= inner_x + inner_w) cursor_x = inner_x + inner_w - 1;
            wchar_t cursor_ch = L' ';
            if (panel.filter_cursor >= start &&
                panel.filter_cursor < start + (int)visible.size()) {
                cursor_ch = visible[panel.filter_cursor - start];
            }
            zex_put(chars, attrs, width, height, cursor_x, filter_y, cursor_ch, ZEX_DIALOG_CURSOR);
        } else {
            std::wstring filter = panel.filter.empty() ? L"*" : panel.filter;
            zex_text(chars, attrs, width, height, inner_x, filter_y, zex_fit(filter, inner_w), filter_attr);
        }
    }

    int visible = std::max(0, zex_visible_rows());
    for (int row = 0; row < visible; row++) {
        int y = zex_entries_y() + row;
        int idx = panel.scroll + row;
        bool is_cursor = panel.active && idx == panel.cursor;
        if (is_cursor)
            zex_fill(chars, attrs, width, height, inner_x, y, inner_w, L' ', ZEX_CURSOR_BG);

        if (idx >= zex_total_rows(panel)) continue;

        std::wstring prefix = L"  ";
        std::wstring name;
        WORD name_attr = ZEX_DOTDOT;

        if (idx == 0) {
            name = L"..";
        } else {
            const Entry& e = panel.entries[idx - 1];
            name = e.name + (e.is_dir ? L"/" : L"");
            name_attr = panel.selected.count(zex_entry_key(panel, e)) ? ZEX_SELECTED : zex_entry_color(e);
        }

        std::wstring line = zex_fit(prefix + name, inner_w);
        for (int i = 0; i < (int)line.size(); i++) {
            WORD attr = (i < 2) ? ZEX_PATH : name_attr;
            if (is_cursor) {
                bool selected_entry = idx > 0 && panel.selected.count(zex_entry_key(panel, panel.entries[idx - 1]));
                attr = selected_entry ? ZEX_CURSOR_SELECTED : ZEX_CURSOR_BG;
            }
            zex_put(chars, attrs, width, height, inner_x + i, y, line[i], attr);
        }
    }

    if (!panel.selected.empty()) {
        std::wstring badge = L"[" + std::to_wstring(panel.selected.size()) + L" selected]";
        badge = zex_fit(badge, inner_w);
        int badge_x = inner_x + std::max(0, inner_w - (int)badge.size());
        int badge_y = height - 3;
        int badge_idx = panel.scroll + (badge_y - zex_entries_y());
        bool badge_on_cursor = panel.active && badge_idx == panel.cursor;
        WORD badge_attr = badge_on_cursor ? ZEX_CURSOR_BG : ZEX_BADGE;
        zex_text(chars, attrs, width, height, badge_x, badge_y, badge, badge_attr);
    }
}

static void zex_present(const std::vector<wchar_t>& chars, const std::vector<WORD>& attrs, int width, int height) {
    if (width <= 0 || height <= 0) return;

    std::vector<CHAR_INFO> buffer((size_t)width * height);
    for (size_t i = 0; i < buffer.size(); i++) {
        buffer[i].Char.UnicodeChar = chars[i];
        buffer[i].Attributes = attrs[i];
    }

    COORD buf_size = {(SHORT)width, (SHORT)height};
    COORD buf_pos = {0, 0};
    SMALL_RECT rect = {0, 0, (SHORT)(width - 1), (SHORT)(height - 1)};
    WriteConsoleOutputW(g_zex.zex_buf, buffer.data(), buf_size, buf_pos, &rect);
}

static void zex_draw() {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (!GetConsoleScreenBufferInfo(g_zex.zex_buf, &csbi)) return;

    int width  = csbi.srWindow.Right  - csbi.srWindow.Left + 1;
    int height = csbi.srWindow.Bottom - csbi.srWindow.Top  + 1;
    if (width <= 0 || height <= 0) return;

    std::vector<wchar_t> chars((size_t)width * height, L' ');
    std::vector<WORD> attrs((size_t)width * height, 0);

    if (width < 20 || height < 8) {
        std::wstring msg = L"ZEX needs a larger console";
        int x = std::max(0, (width - (int)msg.size()) / 2);
        int y = std::max(0, height / 2);
        zex_text(chars, attrs, width, height, x, y, zex_fit(msg, width), ZEX_PATH);
        zex_present(chars, attrs, width, height);
        return;
    }

    int split = (width - 1) / 2;
    int bottom = height - 2;
    int sep_y = zex_separator_y();
    WORD left_border  = g_zex.left.active  ? ZEX_BORDER_ACTIVE : ZEX_BORDER_INACTIVE;
    WORD right_border = g_zex.right.active ? ZEX_BORDER_ACTIVE : ZEX_BORDER_INACTIVE;
    WORD mid_border   = ZEX_BORDER_INACTIVE;

    zex_put(chars, attrs, width, height, 0, 0, L'┌', left_border);
    zex_fill(chars, attrs, width, height, 1, 0, std::max(0, split - 1), L'─', left_border);
    zex_put(chars, attrs, width, height, split, 0, L'┬', mid_border);
    zex_fill(chars, attrs, width, height, split + 1, 0, std::max(0, width - split - 2), L'─', right_border);
    zex_put(chars, attrs, width, height, width - 1, 0, L'┐', right_border);

    for (int y = 1; y < bottom; y++) {
        zex_put(chars, attrs, width, height, 0, y, L'│', left_border);
        zex_put(chars, attrs, width, height, split, y, L'│', mid_border);
        zex_put(chars, attrs, width, height, width - 1, y, L'│', right_border);
    }

    zex_put(chars, attrs, width, height, 0, sep_y, L'├', left_border);
    zex_fill(chars, attrs, width, height, 1, sep_y, std::max(0, split - 1), L'─', left_border);
    zex_put(chars, attrs, width, height, split, sep_y, L'┼', mid_border);
    zex_fill(chars, attrs, width, height, split + 1, sep_y, std::max(0, width - split - 2), L'─', right_border);
    zex_put(chars, attrs, width, height, width - 1, sep_y, L'┤', right_border);

    zex_put(chars, attrs, width, height, 0, bottom, L'└', left_border);
    zex_fill(chars, attrs, width, height, 1, bottom, std::max(0, split - 1), L'─', left_border);
    zex_put(chars, attrs, width, height, split, bottom, L'┴', mid_border);
    zex_fill(chars, attrs, width, height, split + 1, bottom, std::max(0, width - split - 2), L'─', right_border);
    zex_put(chars, attrs, width, height, width - 1, bottom, L'┘', right_border);

    zex_clamp_panel(g_zex.left);
    zex_clamp_panel(g_zex.right);
    zex_draw_panel(chars, attrs, width, height, g_zex.left, 0, split);
    zex_draw_panel(chars, attrs, width, height, g_zex.right, split, width - 1);

    zex_fill(chars, attrs, width, height, 0, height - 1, width, L' ', ZEX_STATUSBAR);
    int sx = 1;
    sx = zex_status_item(chars, attrs, width, height, sx, height - 1, L"F1", L"Mirror");
    sx = zex_status_item(chars, attrs, width, height, sx, height - 1, L"F2", L"Sort");
    sx = zex_status_item(chars, attrs, width, height, sx, height - 1, L"F3", L"View");
    sx = zex_status_item(chars, attrs, width, height, sx, height - 1, L"F4", L"Edit");
    sx = zex_status_item(chars, attrs, width, height, sx, height - 1, L"F5", L"Copy");
    sx = zex_status_item(chars, attrs, width, height, sx, height - 1, L"F6", L"Move");
    sx = zex_status_item(chars, attrs, width, height, sx, height - 1, L"F7", L"MkDir");
    sx = zex_status_item(chars, attrs, width, height, sx, height - 1, L"F8", L"Del");
    sx = zex_status_item(chars, attrs, width, height, sx, height - 1, L"S+F8", L"Del");
    zex_status_item(chars, attrs, width, height, sx, height - 1, L"CTRL+O", L"Hide");

    zex_draw_dialog(chars, attrs, width, height);
    zex_present(chars, attrs, width, height);
}

static Panel& zex_active_panel() {
    return g_zex.left.active ? g_zex.left : g_zex.right;
}

static Panel& zex_inactive_panel() {
    return g_zex.left.active ? g_zex.right : g_zex.left;
}

static Panel& zex_copy_source_panel() {
    return g_zex.copy.source_left ? g_zex.left : g_zex.right;
}

static std::wstring zex_copy_summary() {
    Panel& panel = zex_active_panel();
    if (!panel.selected.empty())
        return L"From: " + std::to_wstring(panel.selected.size()) + L" selected";
    if (panel.cursor <= 0 || panel.cursor - 1 >= (int)panel.entries.size())
        return L"From: Nothing selected";

    const Entry& entry = panel.entries[panel.cursor - 1];
    return L"From: " + entry.name + (entry.is_dir ? L"/" : L"");
}

static void zex_dialog_open(const std::wstring& title, const std::wstring& summary,
    const std::wstring& detail, const std::wstring& input_label,
    const std::wstring& input_value, zex_dialog_kind kind) {
    zex_jump_clear();
    g_zex.focus = zex_focus_dialog;
    g_zex.filter_replace = false;
    g_zex.dialog.kind = kind;
    g_zex.dialog.visible = true;
    g_zex.dialog.title = title;
    g_zex.dialog.summary = summary;
    g_zex.dialog.detail = detail;
    g_zex.dialog.input_label = input_label;
    g_zex.dialog.input_value = input_value;
    g_zex.dialog.input_cursor = (int)g_zex.dialog.input_value.size();
}

static void zex_copy_dialog_open() {
    std::wstring target = zex_display_path(zex_inactive_panel().cwd);
    zex_dialog_open(L"Copy", zex_copy_summary(), L"", L"To:", target, zex_dialog_copy);
}

static void zex_mkdir_dialog_open() {
    zex_dialog_open(L"MkDir",
        L"In: " + zex_display_path(zex_active_panel().cwd),
        L"",
        L"Name:",
        L"",
        zex_dialog_mkdir);
}

static void zex_move_dialog_open() {
    std::wstring target = zex_display_path(zex_inactive_panel().cwd);
    zex_dialog_open(L"Move", zex_copy_summary(), L"", L"To:", target, zex_dialog_move);
}

static void zex_open_other_same_dir() {
    zex_jump_clear();
    Panel& active = zex_active_panel();
    Panel& inactive = zex_inactive_panel();
    inactive.cwd = active.cwd;
    inactive.filter.clear();
    inactive.filter_cursor = 0;
    zex_load_entries(inactive, true);
}

static void zex_cycle_sort_mode() {
    zex_jump_clear();
    Panel& panel = zex_active_panel();
    std::wstring focus_name;
    bool focus_entry = panel.cursor > 0 && panel.cursor - 1 < (int)panel.entries.size();
    if (focus_entry)
        focus_name = panel.entries[panel.cursor - 1].name;

    panel.sort_mode = (panel.sort_mode + 1) % zex_sort_count;
    zex_load_entries(panel, false, false);
    if (focus_entry)
        zex_focus_entry_name(panel, focus_name);
    else
        zex_clamp_panel(panel);
}

static bool zex_open_current_file(bool readonly) {
    Panel& panel = zex_active_panel();
    if (panel.cursor <= 0 || panel.cursor - 1 >= (int)panel.entries.size())
        return false;

    const Entry& entry = panel.entries[panel.cursor - 1];
    if (entry.is_dir)
        return false;

    std::wstring path = zex_join_path(panel.cwd, entry.name);
    SetCurrentDirectoryW(panel.cwd.c_str());
    SetConsoleActiveScreenBuffer(g_zex.shell_buf);
    if (g_zex.shell_buf) {
        DWORD cur_mode = 0;
        if (!GetConsoleMode(g_zex.shell_buf, &cur_mode) || cur_mode != g_zex.shell_out_mode)
            SetConsoleMode(g_zex.shell_buf, g_zex.shell_out_mode);
        SetConsoleCursorInfo(g_zex.shell_buf, &g_zex.shell_cursor);
    }

    if (readonly)
        view_file(to_utf8(path));
    else
        edit_file(to_utf8(path));

    zex_sync_panels_from_shell();
    if (zex_ensure_buffer()) {
        SetConsoleActiveScreenBuffer(g_zex.zex_buf);
        zex_cache_view_size();
    }
    return true;
}

static std::wstring zex_delete_summary(const std::vector<std::wstring>& sources, bool recycle) {
    if (sources.empty())
        return L"Nothing selected";
    if (sources.size() == 1)
        return zex_display_path(sources[0]);
    return std::to_wstring(sources.size()) + L" selected items";
}

static std::wstring zex_delete_detail(const std::vector<std::wstring>& sources, bool recycle) {
    if (sources.empty())
        return L"";
    if (sources.size() == 1)
        return recycle ? L"Move this item to recycle bin?" : L"Permanently delete this item?";
    return recycle ? L"Move selected items to recycle bin?" : L"Permanently delete selected items?";
}

static void zex_delete_dialog_open(bool recycle) {
    std::vector<std::wstring> sources = zex_copy_sources();
    zex_dialog_open(recycle ? L"Recycle" : L"Delete",
        zex_delete_summary(sources, recycle),
        zex_delete_detail(sources, recycle),
        L"", L"", recycle ? zex_dialog_recycle : zex_dialog_delete);
}

static void zex_info_dialog_open(const std::wstring& title, const std::wstring& summary, const std::wstring& detail) {
    zex_dialog_open(title, summary, detail, L"", L"", zex_dialog_info);
}

static void zex_overwrite_dialog_open(const std::wstring& dst) {
    std::wstring title = g_zex.copy.move_mode ? L"Move" : L"Copy";
    zex_dialog_open(title + L" Overwrite", L"Target file already exists", zex_display_path(dst), L"", L"", zex_dialog_overwrite);
}

static void zex_progress_dialog_open(const std::wstring& title, const std::wstring& summary, const std::wstring& detail, int current, int total) {
    g_zex.focus = zex_focus_dialog;
    g_zex.filter_replace = false;
    g_zex.dialog.kind = zex_dialog_progress;
    g_zex.dialog.visible = true;
    g_zex.dialog.title = title;
    g_zex.dialog.summary = summary;
    g_zex.dialog.detail = detail;
    g_zex.dialog.input_label.clear();
    g_zex.dialog.input_value.clear();
    g_zex.dialog.input_cursor = 0;
    g_zex.dialog.progress_current = current;
    g_zex.dialog.progress_total = total;
}

static void zex_dialog_close() {
    g_zex.dialog.visible = false;
    g_zex.dialog = ZexDialog();
    g_zex.focus = zex_focus_panel;
}

static void zex_dialog_confirm() {
    if (g_zex.dialog.kind == zex_dialog_mkdir) {
        Panel& panel = zex_active_panel();
        std::wstring raw = zex_trim(g_zex.dialog.input_value);
        if (raw.empty()) {
            zex_info_dialog_open(L"MkDir", L"Folder name is empty", L"");
            return;
        }

        std::wstring target = zex_native_path(raw);
        if (!zex_is_absolute_path(target))
            target = zex_join_path(panel.cwd, target);

        if (zex_path_exists(target)) {
            zex_info_dialog_open(L"MkDir", L"Folder already exists", zex_display_path(target));
            return;
        }
        if (!zex_ensure_dir(target)) {
            zex_info_dialog_open(L"MkDir", L"Failed to create folder", zex_last_error_text(GetLastError()));
            return;
        }

        std::wstring created_name = zex_leaf_name(target);
        zex_dialog_close();
        zex_copy_refresh_panels();
        if (zex_same_path(panel.cwd, zex_parent_dir(target)) || zex_same_path(panel.cwd, target)) {
            zex_focus_entry_name(panel, created_name);
        }
        return;
    }

    if (g_zex.dialog.kind == zex_dialog_copy) {
        std::vector<std::wstring> sources = zex_copy_sources();
        std::vector<ZexCopyTask> tasks;
        std::map<std::wstring, size_t> pending;
        std::wstring error;
        if (!zex_copy_build_tasks(sources, g_zex.dialog.input_value, tasks, pending, error)) {
            zex_info_dialog_open(L"Copy", error, L"");
            return;
        }

        zex_dialog_close();
        g_zex.copy = ZexCopyState();
        g_zex.copy.active = true;
        g_zex.copy.source_left = g_zex.left.active;
        g_zex.copy.sources = sources;
        g_zex.copy.tasks = std::move(tasks);
        g_zex.copy.pending = std::move(pending);
        zex_copy_run();
        return;
    }

    if (g_zex.dialog.kind == zex_dialog_move) {
        std::vector<std::wstring> sources = zex_copy_sources();
        std::vector<ZexCopyTask> tasks;
        std::map<std::wstring, size_t> pending;
        std::wstring error;
        if (!zex_move_build_tasks(sources, g_zex.dialog.input_value, tasks, pending, error)) {
            zex_info_dialog_open(L"Move", error, L"");
            return;
        }

        zex_dialog_close();
        g_zex.copy = ZexCopyState();
        g_zex.copy.active = true;
        g_zex.copy.move_mode = true;
        g_zex.copy.source_left = g_zex.left.active;
        g_zex.copy.sources = sources;
        g_zex.copy.tasks = std::move(tasks);
        g_zex.copy.pending = std::move(pending);
        zex_copy_run();
        return;
    }

    if (g_zex.dialog.kind == zex_dialog_overwrite) {
        g_zex.copy.overwrite_once = true;
        zex_dialog_close();
        zex_copy_run();
        return;
    }

    if (g_zex.dialog.kind == zex_dialog_recycle || g_zex.dialog.kind == zex_dialog_delete) {
        std::vector<std::wstring> sources = zex_copy_sources();
        if (sources.empty()) {
            zex_info_dialog_open(g_zex.dialog.kind == zex_dialog_recycle ? L"Recycle" : L"Delete", L"Nothing selected", L"");
            return;
        }

        bool recycle = g_zex.dialog.kind == zex_dialog_recycle;
        zex_dialog_close();
        g_zex.del = ZexDeleteState();
        g_zex.del.active = true;
        g_zex.del.recycle_mode = recycle;
        g_zex.del.source_left = g_zex.left.active;
        g_zex.del.sources = std::move(sources);
        zex_delete_run();
        return;
    }

    zex_dialog_close();
}

static void zex_dialog_confirm_all() {
    if (g_zex.dialog.kind != zex_dialog_overwrite) return;
    g_zex.copy.overwrite_all = true;
    zex_dialog_close();
    zex_copy_run();
}

static void zex_dialog_move_cursor(int delta) {
    int next = g_zex.dialog.input_cursor + delta;
    g_zex.dialog.input_cursor = std::max(0, std::min(next, (int)g_zex.dialog.input_value.size()));
}

static void zex_dialog_move_home_end(bool home) {
    g_zex.dialog.input_cursor = home ? 0 : (int)g_zex.dialog.input_value.size();
}

static void zex_dialog_backspace() {
    if (g_zex.dialog.input_cursor <= 0) return;
    g_zex.dialog.input_value.erase(g_zex.dialog.input_cursor - 1, 1);
    g_zex.dialog.input_cursor--;
}

static void zex_dialog_delete_char() {
    if (g_zex.dialog.input_cursor >= (int)g_zex.dialog.input_value.size()) return;
    g_zex.dialog.input_value.erase(g_zex.dialog.input_cursor, 1);
}

static void zex_dialog_insert(wchar_t ch) {
    g_zex.dialog.input_value.insert(g_zex.dialog.input_cursor, 1, ch);
    g_zex.dialog.input_cursor++;
}

static std::vector<std::wstring> zex_copy_sources() {
    Panel& panel = zex_active_panel();
    if (!panel.selected.empty())
        return std::vector<std::wstring>(panel.selected.begin(), panel.selected.end());
    if (panel.cursor <= 0 || panel.cursor - 1 >= (int)panel.entries.size())
        return {};
    return {zex_entry_key(panel, panel.entries[panel.cursor - 1])};
}

static void zex_copy_add_tasks(const std::wstring& src, const std::wstring& dst, const std::wstring& source_key,
    std::vector<ZexCopyTask>& tasks, std::map<std::wstring, size_t>& pending) {
    if (zex_path_is_dir(src)) {
        tasks.push_back({zex_copy_task_mkdir, src, dst, source_key});
        pending[source_key]++;

        WIN32_FIND_DATAW fd = {};
        HANDLE h = FindFirstFileW(zex_glob_pattern(src).c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            std::wstring child_src = zex_join_path(src, name);
            std::wstring child_dst = zex_join_path(dst, name);
            zex_copy_add_tasks(child_src, child_dst, source_key, tasks, pending);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        return;
    }

    tasks.push_back({zex_copy_task_file, src, dst, source_key});
    pending[source_key]++;
}

static bool zex_copy_build_tasks(const std::vector<std::wstring>& sources, const std::wstring& raw_dest,
    std::vector<ZexCopyTask>& tasks, std::map<std::wstring, size_t>& pending, std::wstring& error) {
    tasks.clear();
    pending.clear();
    if (sources.empty()) {
        error = L"Nothing selected";
        return false;
    }

    std::wstring dest = zex_native_path(raw_dest);
    if (dest.empty()) {
        error = L"Destination is empty";
        return false;
    }

    bool dest_exists = zex_path_exists(dest);
    bool dest_is_dir = zex_path_is_dir(dest);
    bool dest_hint_dir = zex_trailing_slash(raw_dest);

    if (sources.size() > 1) {
        if (dest_exists && !dest_is_dir) {
            error = L"Destination must be a folder for multiple items";
            return false;
        }
        for (const std::wstring& src : sources) {
            std::wstring resolved = zex_join_path(dest, zex_leaf_name(src));
            if (zex_same_path(src, resolved)) {
                error = L"Source and destination are the same";
                return false;
            }
            zex_copy_add_tasks(src, resolved, src, tasks, pending);
        }
        return true;
    }

    std::wstring src = sources[0];
    bool src_is_dir = zex_path_is_dir(src);
    std::wstring resolved = dest;
    if (dest_exists && dest_is_dir)
        resolved = zex_join_path(dest, zex_leaf_name(src));
    else if (dest_hint_dir)
        resolved = zex_join_path(dest, zex_leaf_name(src));
    else if (src_is_dir && dest_exists && !dest_is_dir) {
        error = L"Cannot copy folder onto a file";
        return false;
    }

    if (zex_same_path(src, resolved)) {
        error = L"Source and destination are the same";
        return false;
    }

    zex_copy_add_tasks(src, resolved, src, tasks, pending);
    return true;
}

static void zex_move_add_tasks(const std::wstring& src, const std::wstring& dst, const std::wstring& source_key,
    std::vector<ZexCopyTask>& tasks, std::map<std::wstring, size_t>& pending) {
    if (zex_path_is_dir(src)) {
        tasks.push_back({zex_copy_task_mkdir, src, dst, source_key});
        pending[source_key]++;

        WIN32_FIND_DATAW fd = {};
        HANDLE h = FindFirstFileW(zex_glob_pattern(src).c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                std::wstring name = fd.cFileName;
                if (name == L"." || name == L"..") continue;
                std::wstring child_src = zex_join_path(src, name);
                std::wstring child_dst = zex_join_path(dst, name);
                zex_move_add_tasks(child_src, child_dst, source_key, tasks, pending);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }

        tasks.push_back({zex_copy_task_rmdir, src, dst, source_key});
        pending[source_key]++;
        return;
    }

    tasks.push_back({zex_copy_task_file, src, dst, source_key});
    pending[source_key]++;
}

static bool zex_move_build_tasks(const std::vector<std::wstring>& sources, const std::wstring& raw_dest,
    std::vector<ZexCopyTask>& tasks, std::map<std::wstring, size_t>& pending, std::wstring& error) {
    tasks.clear();
    pending.clear();
    if (sources.empty()) {
        error = L"Nothing selected";
        return false;
    }

    std::wstring dest = zex_native_path(raw_dest);
    if (dest.empty()) {
        error = L"Destination is empty";
        return false;
    }

    bool dest_exists = zex_path_exists(dest);
    bool dest_is_dir = zex_path_is_dir(dest);
    bool dest_hint_dir = zex_trailing_slash(raw_dest);

    if (sources.size() > 1) {
        if (dest_exists && !dest_is_dir) {
            error = L"Destination must be a folder for multiple items";
            return false;
        }
        for (const std::wstring& src : sources) {
            std::wstring resolved = zex_join_path(dest, zex_leaf_name(src));
            if (zex_same_path(src, resolved)) {
                error = L"Source and destination are the same";
                return false;
            }
            if (zex_path_is_dir(src) && zex_path_starts_with(resolved, src)) {
                error = L"Cannot move a folder into itself";
                return false;
            }
            zex_move_add_tasks(src, resolved, src, tasks, pending);
        }
        return true;
    }

    std::wstring src = sources[0];
    bool src_is_dir = zex_path_is_dir(src);
    std::wstring resolved = dest;
    if (dest_exists && dest_is_dir)
        resolved = zex_join_path(dest, zex_leaf_name(src));
    else if (dest_hint_dir)
        resolved = zex_join_path(dest, zex_leaf_name(src));
    else if (src_is_dir && dest_exists && !dest_is_dir) {
        error = L"Cannot move folder onto a file";
        return false;
    }

    if (zex_same_path(src, resolved)) {
        error = L"Source and destination are the same";
        return false;
    }
    if (src_is_dir && zex_path_starts_with(resolved, src)) {
        error = L"Cannot move a folder into itself";
        return false;
    }

    zex_move_add_tasks(src, resolved, src, tasks, pending);
    return true;
}

static void zex_copy_refresh_panels() {
    zex_load_entries(g_zex.left, false, false);
    zex_load_entries(g_zex.right, false, false);
}

static int zex_copy_total_steps() {
    return std::max(1, (int)g_zex.copy.tasks.size());
}

static Panel& zex_delete_source_panel() {
    return g_zex.del.source_left ? g_zex.left : g_zex.right;
}

static int zex_delete_total_steps() {
    return std::max(1, (int)g_zex.del.sources.size());
}

static std::wstring zex_delete_name() {
    return g_zex.del.recycle_mode ? L"Recycle" : L"Delete";
}

static std::wstring zex_delete_progress_name() {
    return g_zex.del.recycle_mode ? L"Recycling" : L"Deleting";
}

static std::wstring zex_fileop_error_text(int code, bool aborted) {
    if (aborted)
        return L"Operation canceled";
    if (code == 0)
        return L"Operation failed";

    std::wstring text = zex_last_error_text((DWORD)code);
    if (text.empty() || text == L"The operation completed successfully.")
        return L"Operation failed (code " + std::to_wstring(code) + L")";
    return text;
}

static bool zex_delete_path(const std::wstring& path, bool recycle, std::wstring& error) {
    std::wstring from = zex_native_path(path);
    if (from.empty()) {
        error = L"Nothing selected";
        return false;
    }

    from.push_back(L'\0');
    from.push_back(L'\0');

    SHFILEOPSTRUCTW op = {};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    if (recycle)
        op.fFlags |= FOF_ALLOWUNDO;

    int rc = SHFileOperationW(&op);
    if (rc == 0 && !op.fAnyOperationsAborted)
        return true;

    error = zex_fileop_error_text(rc, op.fAnyOperationsAborted != FALSE);
    return false;
}

static std::wstring zex_transfer_name() {
    return g_zex.copy.move_mode ? L"Move" : L"Copy";
}

static void zex_copy_finish() {
    Panel& source = zex_copy_source_panel();
    for (const std::wstring& src : g_zex.copy.sources)
        source.selected.erase(src);
    if (g_zex.dialog.kind == zex_dialog_progress)
        zex_dialog_close();
    g_zex.copy = ZexCopyState();
    zex_copy_refresh_panels();
}

static void zex_copy_cancel() {
    if (g_zex.dialog.kind == zex_dialog_progress)
        zex_dialog_close();
    g_zex.copy = ZexCopyState();
}

static void zex_copy_run() {
    while (g_zex.copy.active && g_zex.copy.index < g_zex.copy.tasks.size()) {
        const ZexCopyTask& task = g_zex.copy.tasks[g_zex.copy.index];
        zex_progress_dialog_open(
            zex_transfer_name() + L"ing",
            L"From: " + zex_display_path(task.source_key),
            L"To: " + zex_display_path(task.dst),
            (int)g_zex.copy.index,
            zex_copy_total_steps());
        zex_draw();

        if (task.kind == zex_copy_task_mkdir) {
            if (!zex_ensure_dir(task.dst)) {
                DWORD code = GetLastError();
                zex_copy_cancel();
                zex_info_dialog_open(zex_transfer_name(), L"Failed to create folder", zex_last_error_text(code));
                return;
            }
            g_zex.copy.overwrite_once = false;
            auto it = g_zex.copy.pending.find(task.source_key);
            if (it != g_zex.copy.pending.end()) {
                if (it->second > 0) it->second--;
                if (it->second == 0) {
                    zex_copy_source_panel().selected.erase(task.source_key);
                    g_zex.copy.pending.erase(it);
                }
            }
            g_zex.copy.index++;
            continue;
        }

        if (task.kind == zex_copy_task_rmdir) {
            if (!RemoveDirectoryW(task.src.c_str())) {
                DWORD code = GetLastError();
                if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND) {
                    zex_copy_cancel();
                    zex_info_dialog_open(zex_transfer_name(), L"Failed to remove source folder", zex_last_error_text(code));
                    return;
                }
            }
            g_zex.copy.overwrite_once = false;
            auto it = g_zex.copy.pending.find(task.source_key);
            if (it != g_zex.copy.pending.end()) {
                if (it->second > 0) it->second--;
                if (it->second == 0) {
                    zex_copy_source_panel().selected.erase(task.source_key);
                    g_zex.copy.pending.erase(it);
                }
            }
            g_zex.copy.index++;
            continue;
        }

        std::wstring parent = zex_parent_dir(task.dst);
        if (!parent.empty() && !zex_ensure_dir(parent)) {
            DWORD code = GetLastError();
            zex_copy_cancel();
            zex_info_dialog_open(zex_transfer_name(), L"Failed to create target folder", zex_last_error_text(code));
            return;
        }

        bool exists = zex_path_exists(task.dst);
        if (exists && !g_zex.copy.overwrite_all && !g_zex.copy.overwrite_once) {
            zex_overwrite_dialog_open(task.dst);
            return;
        }

        bool overwrite = exists && (g_zex.copy.overwrite_all || g_zex.copy.overwrite_once);
        bool ok = g_zex.copy.move_mode
            ? zex_move_file(task.src, task.dst, overwrite)
            : (overwrite
                ? zex_replace_file(task.src, task.dst)
                : (CopyFileW(task.src.c_str(), task.dst.c_str(), TRUE) != 0));
        if (!ok) {
            DWORD code = GetLastError();
            zex_copy_cancel();
            zex_info_dialog_open(zex_transfer_name(), zex_transfer_name() + L" failed", zex_last_error_text(code));
            return;
        }

        g_zex.copy.overwrite_once = false;
        auto it = g_zex.copy.pending.find(task.source_key);
        if (it != g_zex.copy.pending.end()) {
            if (it->second > 0) it->second--;
            if (it->second == 0) {
                zex_copy_source_panel().selected.erase(task.source_key);
                g_zex.copy.pending.erase(it);
            }
        }
        g_zex.copy.index++;
    }

    if (g_zex.copy.active && g_zex.copy.index >= g_zex.copy.tasks.size()) {
        zex_progress_dialog_open(zex_transfer_name() + L" complete", L"", L"",
            zex_copy_total_steps(), zex_copy_total_steps());
        zex_draw();
        zex_copy_finish();
    }
}

static void zex_delete_finish() {
    if (g_zex.dialog.kind == zex_dialog_progress)
        zex_dialog_close();
    g_zex.del = ZexDeleteState();
    zex_copy_refresh_panels();
}

static void zex_delete_cancel() {
    if (g_zex.dialog.kind == zex_dialog_progress)
        zex_dialog_close();
    g_zex.del = ZexDeleteState();
}

static void zex_delete_run() {
    while (g_zex.del.active && g_zex.del.index < g_zex.del.sources.size()) {
        const std::wstring& src = g_zex.del.sources[g_zex.del.index];
        zex_progress_dialog_open(
            zex_delete_progress_name(),
            L"From: " + zex_display_path(src),
            g_zex.del.recycle_mode ? L"To: recycle bin" : L"To: permanent delete",
            (int)g_zex.del.index,
            zex_delete_total_steps());
        zex_draw();

        std::wstring error;
        if (!zex_delete_path(src, g_zex.del.recycle_mode, error)) {
            zex_delete_cancel();
            zex_info_dialog_open(zex_delete_name(), zex_delete_name() + L" failed", error);
            return;
        }

        zex_delete_source_panel().selected.erase(src);
        g_zex.del.index++;
    }

    if (g_zex.del.active && g_zex.del.index >= g_zex.del.sources.size()) {
        zex_progress_dialog_open(zex_delete_name() + L" complete", L"", L"",
            zex_delete_total_steps(), zex_delete_total_steps());
        zex_draw();
        zex_delete_finish();
    }
}

static bool zex_jump_expired() {
    return !g_zex.jump_buffer.empty() &&
        (GetTickCount64() - g_zex.jump_tick) > ZEX_JUMP_TIMEOUT_MS;
}

static void zex_jump_clear() {
    g_zex.jump_buffer.clear();
    g_zex.jump_tick = 0;
}

static void zex_jump_reset_if_expired() {
    if (zex_jump_expired())
        zex_jump_clear();
}

static void zex_jump_focus_match(Panel& panel) {
    if (g_zex.jump_buffer.empty()) return;

    std::wstring prefix = zex_lower(g_zex.jump_buffer);
    for (int i = 0; i < (int)panel.entries.size(); i++) {
        std::wstring name = zex_lower(panel.entries[i].name);
        if (name.rfind(prefix, 0) == 0) {
            panel.cursor = i + 1;
            zex_clamp_panel(panel);
            return;
        }
    }
}

static void zex_jump_append(wchar_t ch) {
    zex_jump_reset_if_expired();
    g_zex.jump_buffer.push_back((wchar_t)::towlower(ch));
    g_zex.jump_tick = GetTickCount64();
    zex_jump_focus_match(zex_active_panel());
}

static void zex_jump_backspace() {
    zex_jump_reset_if_expired();
    if (g_zex.jump_buffer.empty()) return;
    g_zex.jump_buffer.pop_back();
    g_zex.jump_tick = g_zex.jump_buffer.empty() ? 0 : GetTickCount64();
    if (!g_zex.jump_buffer.empty())
        zex_jump_focus_match(zex_active_panel());
}

static void zex_sync_panels_from_shell() {
    std::wstring shell_cwd = zex_current_dir();
    if (shell_cwd.empty()) return;

    Panel& active = zex_active_panel();
    if (active.cwd != shell_cwd) {
        active.cwd = shell_cwd;
        zex_load_entries(active, true);
    } else {
        zex_load_entries(active, false, false);
    }

    Panel& inactive = zex_inactive_panel();
    if (inactive.cwd == shell_cwd)
        zex_load_entries(inactive, false, false);
}

static void zex_focus_filter_result(Panel& panel) {
    if (!panel.filter.empty() && !panel.entries.empty()) panel.cursor = 1;
    zex_clamp_panel(panel);
}

static void zex_focus_entry_name(Panel& panel, const std::wstring& name) {
    if (name.empty()) {
        zex_clamp_panel(panel);
        return;
    }
    for (int i = 0; i < (int)panel.entries.size(); i++) {
        if (panel.entries[i].name == name) {
            panel.cursor = i + 1;
            zex_clamp_panel(panel);
            return;
        }
    }
    zex_clamp_panel(panel);
}

static void zex_filter_apply_active(bool focus_first_match) {
    Panel& panel = zex_active_panel();
    panel.filter_cursor = std::max(0, std::min(panel.filter_cursor, (int)panel.filter.size()));
    zex_apply_filter(panel);
    if (focus_first_match && !panel.filter.empty())
        zex_focus_filter_result(panel);
    else
        zex_clamp_panel(panel);
}

static void zex_filter_begin() {
    Panel& panel = zex_active_panel();
    zex_jump_clear();
    g_zex.focus = zex_focus_filter;
    g_zex.filter_replace = !panel.filter.empty();
    panel.filter_cursor = (int)panel.filter.size();
}

static void zex_filter_append(wchar_t ch) {
    Panel& panel = zex_active_panel();
    if (g_zex.filter_replace) {
        panel.filter.clear();
        panel.filter_cursor = 0;
        g_zex.filter_replace = false;
    }
    panel.filter.insert(panel.filter_cursor, 1, ch);
    panel.filter_cursor++;
    zex_filter_apply_active(true);
}

static void zex_filter_backspace() {
    Panel& panel = zex_active_panel();
    g_zex.filter_replace = false;
    if (panel.filter_cursor <= 0 || panel.filter.empty()) return;
    panel.filter.erase(panel.filter_cursor - 1, 1);
    panel.filter_cursor--;
    zex_filter_apply_active(false);
}

static void zex_filter_delete() {
    Panel& panel = zex_active_panel();
    g_zex.filter_replace = false;
    if (panel.filter_cursor >= (int)panel.filter.size()) return;
    panel.filter.erase(panel.filter_cursor, 1);
    zex_filter_apply_active(false);
}

static void zex_filter_move_cursor(int delta) {
    Panel& panel = zex_active_panel();
    g_zex.filter_replace = false;
    int next = panel.filter_cursor + delta;
    panel.filter_cursor = std::max(0, std::min(next, (int)panel.filter.size()));
}

static void zex_filter_move_home_end(bool home) {
    Panel& panel = zex_active_panel();
    g_zex.filter_replace = false;
    panel.filter_cursor = home ? 0 : (int)panel.filter.size();
}

static void zex_filter_end() {
    g_zex.focus = zex_focus_panel;
    g_zex.filter_replace = false;
}

static void zex_filter_clear_and_exit() {
    Panel& panel = zex_active_panel();
    panel.filter.clear();
    panel.filter_cursor = 0;
    zex_filter_end();
    zex_filter_apply_active(false);
}

static void zex_switch_panel() {
    g_zex.focus = zex_focus_panel;
    g_zex.filter_replace = false;
    zex_jump_clear();
    g_zex.left.active = !g_zex.left.active;
    g_zex.right.active = !g_zex.right.active;
    zex_clamp_panel(g_zex.left);
    zex_clamp_panel(g_zex.right);
}

static void zex_move_cursor(int delta) {
    zex_jump_clear();
    Panel& panel = zex_active_panel();
    panel.cursor += delta;
    zex_clamp_panel(panel);
}

static void zex_move_page(int delta) {
    zex_jump_clear();
    Panel& panel = zex_active_panel();
    int step = std::max(1, zex_visible_rows());
    panel.cursor += delta * step;
    zex_clamp_panel(panel);
}

static void zex_move_home_end(bool home) {
    zex_jump_clear();
    Panel& panel = zex_active_panel();
    panel.cursor = home ? 0 : std::max(0, zex_total_rows(panel) - 1);
    zex_clamp_panel(panel);
}

static void zex_toggle_selection() {
    zex_jump_clear();
    Panel& panel = zex_active_panel();
    if (panel.cursor <= 0) return;

    int idx = panel.cursor - 1;
    if (idx >= (int)panel.entries.size()) return;

    std::wstring key = zex_entry_key(panel, panel.entries[idx]);
    if (panel.selected.count(key))
        panel.selected.erase(key);
    else
        panel.selected.insert(key);

    if (panel.cursor < zex_total_rows(panel) - 1)
        panel.cursor++;
    zex_clamp_panel(panel);
}

static bool zex_clear_selection() {
    Panel& panel = zex_active_panel();
    if (panel.selected.empty()) return false;
    panel.selected.clear();
    return true;
}

static void zex_go_parent() {
    zex_jump_clear();
    Panel& panel = zex_active_panel();
    std::wstring child_name = zex_leaf_name(panel.cwd);
    std::wstring parent = zex_parent_dir(panel.cwd);
    if (!parent.empty() && parent != panel.cwd) {
        panel.cwd = parent;
        zex_load_entries(panel, true);
        zex_focus_entry_name(panel, child_name);
    } else {
        panel.cursor = 0;
        panel.scroll = 0;
    }
}

static void zex_enter() {
    zex_jump_clear();
    Panel& panel = zex_active_panel();
    if (panel.cursor == 0) {
        zex_go_parent();
        return;
    }
    if (panel.cursor - 1 >= (int)panel.entries.size()) return;

    const Entry& entry = panel.entries[panel.cursor - 1];
    std::wstring path = zex_join_path(panel.cwd, entry.name);
    if (entry.is_dir) {
        panel.cwd = path;
        zex_load_entries(panel, true);
    }
}

void zex_toggle() {
    zex_init_state();
    zex_jump_clear();
    zex_sync_panels_from_shell();
    if (!zex_ensure_buffer()) return;

    if (g_zex.shell_buf) {
        GetConsoleCursorInfo(g_zex.shell_buf, &g_zex.shell_cursor);
        GetConsoleMode(g_zex.shell_buf, &g_zex.shell_out_mode);
    }

    SetConsoleActiveScreenBuffer(g_zex.zex_buf);
    zex_cache_view_size();
    zex_draw();

    while (true) {
        DWORD wait = WaitForSingleObject(in_h, 50);
        if (wait != WAIT_OBJECT_0) continue;

        INPUT_RECORD ir;
        DWORD count = 0;
        if (!ReadConsoleInputW(in_h, &ir, 1, &count)) break;

        if (ir.EventType == WINDOW_BUFFER_SIZE_EVENT) {
            zex_cache_view_size();
            zex_draw();
            continue;
        }
        if (ir.EventType != KEY_EVENT || !ir.Event.KeyEvent.bKeyDown) continue;

        WORD vk = ir.Event.KeyEvent.wVirtualKeyCode;
        wchar_t ch = ir.Event.KeyEvent.uChar.UnicodeChar;
        DWORD state = ir.Event.KeyEvent.dwControlKeyState;
        bool ctrl = (state & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
        bool shift = (state & SHIFT_PRESSED) != 0;

        if ((ctrl && vk == 'O') || vk == VK_F10) break;
        if (zex_dialog_focused()) {
            if (!ctrl && vk == VK_ESCAPE) {
                if (g_zex.dialog.kind == zex_dialog_overwrite)
                    zex_copy_cancel();
                zex_dialog_close();
                zex_draw();
                continue;
            }
            if (!ctrl && vk == VK_RETURN) { zex_dialog_confirm(); zex_draw(); continue; }
            if (ctrl && vk == VK_RETURN && g_zex.dialog.kind == zex_dialog_overwrite) {
                zex_dialog_confirm_all();
                zex_draw();
                continue;
            }
            if (g_zex.dialog.kind == zex_dialog_copy || g_zex.dialog.kind == zex_dialog_move || g_zex.dialog.kind == zex_dialog_mkdir) {
                if (!ctrl && vk == VK_LEFT)   { zex_dialog_move_cursor(-1); zex_draw(); continue; }
                if (!ctrl && vk == VK_RIGHT)  { zex_dialog_move_cursor(+1); zex_draw(); continue; }
                if (!ctrl && vk == VK_HOME)   { zex_dialog_move_home_end(true); zex_draw(); continue; }
                if (!ctrl && vk == VK_END)    { zex_dialog_move_home_end(false); zex_draw(); continue; }
                if (!ctrl && vk == VK_BACK)   { zex_dialog_backspace(); zex_draw(); continue; }
                if (!ctrl && vk == VK_DELETE) { zex_dialog_delete_char(); zex_draw(); continue; }
                if (!ctrl && ch >= 32 && ch != 127) { zex_dialog_insert(ch); zex_draw(); continue; }
            }
            continue;
        }
        if (zex_filter_focused()) {
            if (vk == VK_UP)     { zex_filter_end(); zex_move_cursor(-1); zex_draw(); continue; }
            if (vk == VK_DOWN)   { zex_filter_end(); zex_move_cursor(+1); zex_draw(); continue; }
            if (!ctrl && vk == VK_RETURN) { zex_filter_end(); zex_draw(); continue; }
            if (!ctrl && vk == VK_ESCAPE) { zex_filter_clear_and_exit(); zex_draw(); continue; }
            if (!ctrl && vk == VK_LEFT)   { zex_filter_move_cursor(-1); zex_draw(); continue; }
            if (!ctrl && vk == VK_RIGHT)  { zex_filter_move_cursor(+1); zex_draw(); continue; }
            if (!ctrl && vk == VK_HOME)   { zex_filter_move_home_end(true); zex_draw(); continue; }
            if (!ctrl && vk == VK_END)    { zex_filter_move_home_end(false); zex_draw(); continue; }
            if (!ctrl && vk == VK_BACK)   { zex_filter_backspace(); zex_draw(); continue; }
            if (!ctrl && vk == VK_DELETE) { zex_filter_delete(); zex_draw(); continue; }
            if (!ctrl && ch >= 32 && ch != 127) { zex_filter_append(ch); zex_draw(); continue; }
            continue;
        }
        zex_jump_reset_if_expired();
        if (!ctrl && ch == L'/')         { zex_filter_begin(); zex_draw(); continue; }
        if (!ctrl && vk == VK_ESCAPE)    {
            if (zex_clear_selection() || !g_zex.jump_buffer.empty()) {
                zex_jump_clear();
                zex_draw();
            }
            continue;
        }
        if (vk == VK_F1)       { zex_open_other_same_dir(); zex_draw(); continue; }
        if (vk == VK_F2)       { zex_cycle_sort_mode(); zex_draw(); continue; }
        if (vk == VK_TAB)      { zex_switch_panel(); zex_draw(); continue; }
        if (vk == VK_UP)       { zex_move_cursor(-1); zex_draw(); continue; }
        if (vk == VK_DOWN)     { zex_move_cursor(+1); zex_draw(); continue; }
        if (vk == VK_LEFT)     { zex_move_page(-1); zex_draw(); continue; }
        if (vk == VK_RIGHT)    { zex_move_page(+1); zex_draw(); continue; }
        if (vk == VK_PRIOR)    { zex_move_page(-1); zex_draw(); continue; }
        if (vk == VK_NEXT)     { zex_move_page(+1); zex_draw(); continue; }
        if (vk == VK_HOME)     { zex_move_home_end(true); zex_draw(); continue; }
        if (vk == VK_END)      { zex_move_home_end(false); zex_draw(); continue; }
        if (vk == VK_F3)       { if (zex_open_current_file(true)) zex_draw(); continue; }
        if (vk == VK_F4)       { if (zex_open_current_file(false)) zex_draw(); continue; }
        if (vk == VK_F5)       { zex_copy_dialog_open(); zex_draw(); continue; }
        if (vk == VK_F6)       { zex_move_dialog_open(); zex_draw(); continue; }
        if (vk == VK_F7)       { zex_mkdir_dialog_open(); zex_draw(); continue; }
        if (vk == VK_F8 && shift) { zex_delete_dialog_open(false); zex_draw(); continue; }
        if (vk == VK_F8)       { zex_delete_dialog_open(true); zex_draw(); continue; }
        if (vk == VK_INSERT)   { zex_toggle_selection(); zex_draw(); continue; }
        if (vk == VK_RETURN)   { zex_enter(); zex_draw(); continue; }
        if (!ctrl && vk == VK_BACK && !g_zex.jump_buffer.empty()) { zex_jump_backspace(); zex_draw(); continue; }
        if (vk == VK_BACK)     { zex_go_parent(); zex_draw(); continue; }
        if (!ctrl && ch >= 32 && ch != 127) { zex_jump_append(ch); zex_draw(); continue; }
    }

    Panel& active = zex_active_panel();
    if (!active.cwd.empty())
        SetCurrentDirectoryW(active.cwd.c_str());

    SetConsoleActiveScreenBuffer(g_zex.shell_buf);
    if (g_zex.shell_buf) {
        DWORD cur_mode = 0;
        if (!GetConsoleMode(g_zex.shell_buf, &cur_mode) || cur_mode != g_zex.shell_out_mode)
            SetConsoleMode(g_zex.shell_buf, g_zex.shell_out_mode);
        SetConsoleCursorInfo(g_zex.shell_buf, &g_zex.shell_cursor);
    }
    zex_close_buffer();
}
