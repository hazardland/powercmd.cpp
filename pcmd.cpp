#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <windows.h>
#include <string>
#include <algorithm>
#include <vector>
#include <cstdio>
#include <cwctype>
#include <fstream>
#include <sstream>
#include <unordered_set>
#include <unordered_map>

#ifndef VERSION_MINOR
#define VERSION_MINOR 0
#endif
#define STR_(x) #x
#define STR(x) STR_(x)
#define VERSION "0.0." STR(VERSION_MINOR)

#define GRAY   "\x1b[38;5;240m"
#define BLUE   "\x1b[38;5;75m"
#define RED    "\x1b[38;5;203m"
#define YELLOW "\x1b[38;5;229m"
#define GREEN  "\x1b[38;5;114m"  // executables (ls color guide)
#define RESET  "\x1b[0m"

// Console handles initialised once in main and used by all I/O functions.
static HANDLE out_h;
static HANDLE err_h;
static HANDLE in_h;
// Saved input mode; restored before spawning child processes so they receive normal input handling.
static DWORD  orig_in_mode;

// ---- string helpers ----

// Convert wide string to UTF-8; used when writing wstring data to the console or history file.
std::string to_utf8(const std::wstring& ws) {
    if (ws.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, NULL, 0, NULL, NULL);
    std::string s(n - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, &s[0], n, NULL, NULL);
    return s;
}

// Convert UTF-8 string to wide; used before any Win32 API call that requires a wchar_t path or command.
std::wstring to_wide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, NULL, 0);
    std::wstring ws(n - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &ws[0], n);
    return ws;
}

// Write UTF-8 bytes directly to stdout handle; bypasses C runtime buffering so ANSI sequences render immediately.
void out(const std::string& s) {
    DWORD w;
    WriteFile(out_h, s.c_str(), (DWORD)s.size(), &w, NULL);
}

// Write to stderr handle; used for error messages that should not mix with stdout output.
void err(const std::string& s) {
    DWORD w;
    WriteFile(err_h, s.c_str(), (DWORD)s.size(), &w, NULL);
}

// ---- ctrl+c handler ----

// Set to true by ctrl_handler on Ctrl+C; checked after child exits to suppress exit-code display and emit a newline.
static volatile bool ctrl_c_fired = false;

// forward declarations so ctrl_handler can save history on close/shutdown
struct editor;
void save_prev_dir();
void write_alias(const std::string&, const std::string&);
void append_history(const std::wstring&);
void compact_history();
// Globals so ctrl_handler (which has no parameters) can reach the live editor state on unexpected exit.
static editor* g_editor  = nullptr;

// Ctrl+C/Break: set flag and suppress for our process (child still receives the signal).
// Close/Logoff/Shutdown: flush history to disk then let Windows terminate us.
BOOL WINAPI ctrl_handler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        ctrl_c_fired = true;
        return TRUE; // suppress for our process, child still gets it
    }
    if (type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT) {
        if (g_editor) { compact_history(); save_prev_dir(); }
        return FALSE; // let default handler terminate the process
    }
    return FALSE;
}

// ---- shell info ----

// Returns true if the process has admin elevation; used to switch prompt color from blue to red.
bool elevated() {
    BOOL result = FALSE;
    HANDLE token = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION e = {};
        DWORD size = sizeof(e);
        if (GetTokenInformation(token, TokenElevation, &e, sizeof(e), &size))
            result = e.TokenIsElevated;
        CloseHandle(token);
    }
    return result;
}

// Returns the current local time as "HH:MM:SS.cs" (centiseconds) for the prompt timestamp.
std::string cur_time() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%02d",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds / 10);
    return buf;
}

// Returns the current directory with all backslashes replaced by forward slashes.
std::string cwd() {
    wchar_t buf[MAX_PATH];
    GetCurrentDirectoryW(MAX_PATH, buf);
    std::string s = to_utf8(buf);
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

// Extracts the last path component for display in the prompt; handles trailing slash by returning the component before it.
std::string folder(const std::string& path) {
    size_t pos = path.find_last_of("\\/");
    if (pos == std::string::npos) return path;
    std::string name = path.substr(pos + 1);
    return name.empty() ? path.substr(0, pos) : name;
}

// Walks up from cwd looking for .git/HEAD to find the current branch name.
// Also sets root_out to the repo root (parent of .git) when found.
// Returns the branch name, a 7-char SHA for detached HEAD, or empty string if not in a git repo.
std::string branch(std::wstring& root_out) {
    wchar_t dir[MAX_PATH];
    GetCurrentDirectoryW(MAX_PATH, dir);
    std::wstring path = dir;
    while (!path.empty()) {
        HANDLE f = CreateFileW((path + L"\\.git\\HEAD").c_str(),
            GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            char buf[256] = {};
            DWORD read = 0;
            ReadFile(f, buf, sizeof(buf) - 1, &read, NULL);
            CloseHandle(f);
            root_out = path;
            std::string s(buf);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
                s.pop_back();
            const std::string ref = "ref: refs/heads/";
            if (s.substr(0, ref.size()) == ref) return s.substr(ref.size());
            if (s.size() >= 7) return s.substr(0, 7);
            return s;
        }
        size_t sep = path.find_last_of(L"\\/");
        if (sep == std::wstring::npos) break;
        path = path.substr(0, sep);
    }
    return "";
}

// big-endian helpers for reading git's binary index format (no winsock/ntohl needed)
static inline uint32_t be32(const char* p) {
    return ((uint8_t)p[0] << 24) | ((uint8_t)p[1] << 16) | ((uint8_t)p[2] << 8) | (uint8_t)p[3];
}
static inline uint16_t be16(const char* p) {
    return ((uint8_t)p[0] << 8) | (uint8_t)p[1];
}

// Reads .git/index directly and compares each tracked file's cached mtime+size to the real file.
// Returns true if any tracked file has been modified since it was last staged.
// No git process is spawned. Staged-only changes (same size/mtime, different content) are not detected.
bool dirty(const std::wstring& root) {
    std::wstring index_path = root + L"\\.git\\index";
    HANDLE f = CreateFileW(index_path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER sz;
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart < 12 || sz.QuadPart > 64 * 1024 * 1024) {
        CloseHandle(f); return false;
    }
    std::vector<char> data((size_t)sz.QuadPart);
    DWORD n = 0;
    bool ok = ReadFile(f, data.data(), (DWORD)sz.QuadPart, &n, NULL) && n == (DWORD)sz.QuadPart;
    CloseHandle(f);
    if (!ok) return false;

    const char* d = data.data();
    if (memcmp(d, "DIRC", 4) != 0) return false;
    uint32_t ver   = be32(d + 4);
    uint32_t count = be32(d + 8);
    if (ver < 2 || ver > 3) return false; // v4 uses path compression; skip

    size_t pos = 12;
    for (uint32_t i = 0; i < count; i++) {
        if (pos + 62 > data.size()) break;

        const char* e       = d + pos;
        uint32_t mtime_s    = be32(e + 8);
        uint32_t cached_sz  = be32(e + 36);
        uint16_t flags      = be16(e + 60);
        bool extended       = (ver >= 3) && (flags & 0x4000);
        size_t name_off     = pos + 62 + (extended ? 2 : 0);

        // find null-terminator of the path
        size_t name_end = name_off;
        while (name_end < data.size() && d[name_end] != '\0') name_end++;

        // advance pos: entry is padded to 8-byte boundary from its start
        size_t hdr   = 62 + (extended ? 2 : 0);
        size_t nlen  = name_end - name_off;
        size_t entry = hdr + nlen + 1;
        pos += (entry + 7) & ~(size_t)7;

        // build the full path and stat the file
        int wlen = MultiByteToWideChar(CP_UTF8, 0, d + name_off, (int)nlen, NULL, 0);
        if (wlen <= 0) continue;
        std::wstring rel(wlen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, d + name_off, (int)nlen, &rel[0], wlen);
        for (auto& c : rel) if (c == L'/') c = L'\\';

        WIN32_FILE_ATTRIBUTE_DATA attr;
        if (!GetFileAttributesExW((root + L"\\" + rel).c_str(), GetFileExInfoStandard, &attr))
            return true; // deleted tracked file → dirty

        // size mismatch → dirty
        uint64_t cur_sz = ((uint64_t)attr.nFileSizeHigh << 32) | attr.nFileSizeLow;
        if (cur_sz != cached_sz) return true;

        // mtime mismatch → dirty (compare seconds; ignore sub-second)
        ULARGE_INTEGER ft = { attr.ftLastWriteTime.dwLowDateTime, attr.ftLastWriteTime.dwHighDateTime };
        if (ft.QuadPart >= 116444736000000000ULL) {
            uint32_t cur_s = (uint32_t)((ft.QuadPart - 116444736000000000ULL) / 10000000ULL);
            if (cur_s != mtime_s) return true;
        }
    }
    return false;
}

// ---- prompt ----

// Prompt string paired with its visual width (ANSI escape codes excluded).
// vis is needed by redraw to correctly compute screen column positions.
struct prompt_t {
    std::string str; // full ANSI string
    int vis;         // printable character count (no escape codes)
};

// Builds the "[time]folder[branch*][exitcode]> " prompt and computes vis in one pass.
// Exit code segment is omitted when code == 0; branch segment omitted when b is empty.
prompt_t make_prompt(bool elev, const std::string& t, const std::string& f,
                     const std::string& b, bool d, int code) {
    const char* color = elev ? RED : BLUE;
    std::string s;
    s += GRAY "["; s += t; s += "]";
    s += color; s += f;
    if (!b.empty()) {
        s += RESET "[";
        s += YELLOW; s += b;
        if (d) s += "*";
        s += RESET "]";
    }
    std::string cs = code != 0 ? std::to_string(code) : "";
    if (!cs.empty()) {
        s += RED "["; s += cs; s += "]";
        s += color;
    }
    s += color; s += "> ";
    s += RESET;

    int vis = 2 + (int)t.size() + (int)f.size() + 2;
    if (!b.empty()) vis += 1 + (int)b.size() + (d ? 1 : 0) + 1;
    if (!cs.empty()) vis += 1 + (int)cs.size() + 1;
    return { s, vis };
}

// ---- tab completion ----

// Returns filesystem entries matching prefix* sorted alphabetically; appends "/" to directories.
// dirs_only is set true when completing after "cd" so files are excluded.
std::vector<std::wstring> complete(const std::wstring& prefix, bool dirs_only = false) {
    std::vector<std::wstring> result;
    std::wstring dir, name;
    size_t sep = prefix.find_last_of(L"\\/");
    if (sep == std::wstring::npos) { dir = L""; name = prefix; }
    else { dir = prefix.substr(0, sep + 1); name = prefix.substr(sep + 1); }

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + name + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return result;
    do {
        std::wstring fname = fd.cFileName;
        if (fname == L"." || fname == L"..") continue;
        bool is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (dirs_only && !is_dir) continue;
        std::wstring full = dir + fname;
        if (is_dir) full += L"/";
        std::replace(full.begin(), full.end(), L'\\', L'/');
        result.push_back(full);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(result.begin(), result.end());
    return result;
}

// ---- line editor ----

// All mutable state for one input session. hist persists across prompts; all other
// fields are reset after each Enter so each new line starts clean.
struct editor {
    std::wstring buf;           // what the user has typed; the only thing that gets executed
    std::wstring full_cmd;      // segments accumulated across ^ line continuations; prepended to buf on final Enter
    int pos        = 0;         // cursor position within buf (not screen column)
    int prev_pos   = 0;         // buf position as of last redraw; used to compute how many rows to move up
    int prompt_vis = 0;         // visual width of prompt_str (no ANSI codes); needed for screen-column math
    std::string prompt_str;     // stored so redraw can reprint it without recomputing

    std::vector<std::wstring> hist; // history list oldest-first; deduplicated so each command appears once
    int hist_idx  = -1;         // -1 = edit mode; >= 0 = index into hist during UP/DOWN navigation
    std::wstring saved;         // snapshot of buf taken when UP is first pressed; prefix filter for navigation; empty = plain (unfiltered)
    bool plain_nav = false;     // set true after accepting a hint with →/End so the next UP ignores buf as filter
    std::wstring hint;          // gray ghost suffix after cursor; in nav mode it holds the suffix of the matching history entry

    bool tab_on = false;                    // true while Tab is being cycled; any non-Tab key resets this
    std::vector<std::wstring> tab_matches;  // all completions found on first Tab press
    int tab_idx   = 0;                      // which completion is currently shown
    int tab_start = 0;                      // start offset of the token being completed within buf
    std::wstring tab_pre;                   // buf content before the completion token; preserved across cycles
    std::wstring tab_suf;                   // buf content after cursor at Tab-press time; reappended each cycle
};

// Calculates the ghost hint for the current buf in edit mode (hist_idx == -1).
// For "cd <path>" uses filesystem completions (dirs only); for "ls <path>" files+dirs;
// for everything else scans history backwards so the most recent matching command wins.
void find_hint(editor& e) {
    e.hint.clear();
    if (e.buf.empty() || e.hist_idx != -1) return;
    std::wstring lower = e.buf;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    if (lower == L"cd" || (lower.size() >= 3 && lower.substr(0, 3) == L"cd ")) {
        if (lower.size() >= 3) {
            std::wstring token = e.buf.substr(3);
            auto matches = complete(token, true);
            if (!matches.empty() && matches[0].size() > token.size())
                e.hint = matches[0].substr(token.size());
        }
        return;
    }
    if (lower.size() >= 3 && lower.substr(0, 3) == L"ls ") {
        std::wstring token = e.buf.substr(3);
        auto matches = complete(token, true);
        if (!matches.empty() && matches[0].size() > token.size())
            e.hint = matches[0].substr(token.size());
        return;
    }
    if (lower.size() >= 4 && lower.substr(0, 4) == L"cat ") {
        std::wstring token = e.buf.substr(4);
        auto matches = complete(token, false);
        if (!matches.empty() && matches[0].size() > token.size())
            e.hint = matches[0].substr(token.size());
        return;
    }
    for (int i = (int)e.hist.size() - 1; i >= 0; i--) {
        if (e.hist[i].size() > e.buf.size() &&
            e.hist[i].substr(0, e.buf.size()) == e.buf) {
            e.hint = e.hist[i].substr(e.buf.size());
            return;
        }
    }
}

// Returns the visible column count of the terminal window; falls back to 80 if not a real console.
int term_width() {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(out_h, &csbi))
        return csbi.srWindow.Right - csbi.srWindow.Left + 1;
    return 80;
}

// Redraws the entire input line in place: moves up to the prompt row using prev_pos (where the
// cursor actually is), clears to end of screen, reprints prompt + buf + gray hint, then
// repositions the cursor at e.pos. Batches all output into one write to avoid flicker.
void redraw(editor& e) {
    int width   = term_width();
    // cur_row: how many rows below the prompt start the cursor currently sits
    int cur_row = (e.prompt_vis + e.prev_pos) / width;

    std::string s;
    // move up to the prompt row
    if (cur_row > 0) {
        char esc[32];
        snprintf(esc, sizeof(esc), "\x1b[%dA", cur_row);
        s += esc;
    }
    s += "\r";           // col 0 of prompt row
    s += "\x1b[J";       // clear to end of screen
    s += e.prompt_str;   // reprint prompt
    s += to_utf8(e.buf); // reprint buffer

    // gray hint — cap to remaining cols on last line so it never wraps
    if (!e.hint.empty()) {
        int end_col = (e.prompt_vis + (int)e.buf.size()) % width;
        int remaining = width - end_col;
        std::wstring shown = e.hint.substr(0, std::min((int)e.hint.size(), remaining - 1));
        if (!shown.empty())
            s += GRAY + to_utf8(shown) + RESET;
    }

    // position cursor at e.pos
    int end_row = (e.prompt_vis + (int)e.buf.size()) / width;
    int pos_row = (e.prompt_vis + e.pos) / width;
    int pos_col = (e.prompt_vis + e.pos) % width;
    int rows_up = end_row - pos_row;
    if (rows_up > 0) {
        char esc[32];
        snprintf(esc, sizeof(esc), "\x1b[%dA", rows_up);
        s += esc;
    }
    char col[32];
    snprintf(col, sizeof(col), "\x1b[%dG", pos_col + 1);
    s += col;

    out(s);
    e.prev_pos = e.pos;
}

// Advances history navigation one step in direction dir (-1 = UP, +1 = DOWN).
// In filtered mode (saved non-empty) searches for the next entry starting with saved;
// falls back to plain cycle if no match exists. Shared by VK_UP and VK_DOWN handlers.
static void nav_step(editor& e, int dir) {
    int n = (int)e.hist.size();
    int start = ((e.hist_idx + dir) % n + n) % n;
    if (e.saved.empty()) {
        e.hist_idx = start;
        e.buf = e.hist[e.hist_idx]; e.hint.clear();
    } else {
        int found = -1;
        for (int i = 0; i < n; i++) {
            int idx = ((e.hist_idx + dir * (1 + i)) % n + n) % n;
            if (e.hist[idx].substr(0, e.saved.size()) == e.saved) { found = idx; break; }
        }
        if (found == -1) {
            e.hist_idx = start; e.buf = e.hist[e.hist_idx]; e.hint.clear();
        } else {
            e.hist_idx = found;
            e.buf  = e.saved;
            e.hint = e.hist[found].substr(e.saved.size());
        }
    }
    e.pos = (int)e.buf.size();
    redraw(e);
}

// Transitions the editor into history-nav mode on the first UP key press.
// Snapshots buf as the filter prefix (or empty if plain_nav). If a hint is already visible,
// anchors hist_idx at that entry so the next nav_step immediately moves past it.
static void enter_nav(editor& e) {
    e.saved = e.plain_nav ? L"" : e.buf;
    e.hist_idx = (int)e.hist.size();
    if (!e.hint.empty() && !e.saved.empty()) {
        std::wstring full = e.buf + e.hint;
        for (int i = (int)e.hist.size() - 1; i >= 0; i--)
            if (e.hist[i] == full) { e.hist_idx = i; break; }
    }
}

// Raw input loop: reads INPUT_RECORDs one at a time (key-down only) and drives the full
// editor state machine — history navigation, hint accept, tab completion, cursor movement,
// line continuation, Ctrl+C. Calls redraw after every state change. Returns the completed
// command on Enter (with nav hint auto-accepted), or empty string on Ctrl+C.
std::string readline(editor& e) {
    while (true) {
        // -- read one event, skip everything except key-down --
        // Mouse moves, key-up events, and window resize records all arrive here; we ignore them.
        INPUT_RECORD ir;
        DWORD count;
        if (!ReadConsoleInputW(in_h, &ir, 1, &count)) break;
        if (ir.EventType != KEY_EVENT || !ir.Event.KeyEvent.bKeyDown) continue;

        WORD vk     = ir.Event.KeyEvent.wVirtualKeyCode;
        wchar_t ch  = ir.Event.KeyEvent.uChar.UnicodeChar;
        DWORD state = ir.Event.KeyEvent.dwControlKeyState;
        bool ctrl   = (state & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;

        // Any key other than Tab resets the Tab cycling state.
        if (vk != VK_TAB) e.tab_on = false;

        // -- Enter: submit line --
        // If buf ends with ^ or \, accumulate into full_cmd and start a continuation line.
        // Otherwise auto-accept any nav hint, deduplicate + push to history, reset all state, return.
        if (vk == VK_RETURN) {
            std::wstring trimmed = e.buf;
            while (!trimmed.empty() && trimmed.back() == L' ') trimmed.pop_back();
            if (!trimmed.empty() && (trimmed.back() == L'^' || trimmed.back() == L'\\')) {
                e.full_cmd += trimmed.substr(0, trimmed.size() - 1);
                e.buf.clear();
                e.pos        = 0;
                e.prev_pos   = 0;
                e.prompt_str = "> ";
                e.prompt_vis = 2;
                out("\r\n\x1b[2K> "); // \x1b[2K clears the line (erases any "More?" ConHost may have echoed)
                continue;
            }
            if (e.hist_idx != -1 && !e.hint.empty()) e.buf += e.hint; // accept nav hint on Enter
            e.hint.clear(); redraw(e); // clear ghost text before submitting
            out("\r\n");
            std::wstring full = e.full_cmd + e.buf;
            std::string line = to_utf8(full);
            if (!full.empty()) {
                e.hist.erase(std::remove(e.hist.begin(), e.hist.end(), full), e.hist.end());
                e.hist.push_back(full);
                append_history(full);
            }
            e.buf.clear();
            e.full_cmd.clear();
            e.pos       = 0;
            e.hist_idx  = -1;
            e.saved.clear();
            e.plain_nav = false;
            return line;
        }

        // -- Backspace / Delete: erase a character --
        // Both exit nav mode and reset plain_nav so the next UP re-filters from the new shorter buf.
        if (vk == VK_BACK) {
            if (e.pos > 0) { e.hist_idx = -1; e.plain_nav = false; e.buf.erase(e.pos - 1, 1); e.pos--; find_hint(e); redraw(e); }
            continue;
        }

        if (vk == VK_DELETE) {
            if (e.pos < (int)e.buf.size()) { e.hist_idx = -1; e.plain_nav = false; e.buf.erase(e.pos, 1); find_hint(e); redraw(e); }
            continue;
        }

        // -- Left / Right / Home / End: cursor movement --
        // Ctrl+Left/Right jumps by whole words. Right at the end of buf accepts the hint and
        // sets plain_nav=true so subsequent UP/DOWN cycles all history unfiltered. End does the same.
        if (vk == VK_LEFT) {
            if (ctrl) {
                while (e.pos > 0 && e.buf[e.pos - 1] == L' ') e.pos--;
                while (e.pos > 0 && e.buf[e.pos - 1] != L' ') e.pos--;
            } else if (e.pos > 0) e.pos--;
            redraw(e); continue;
        }
        if (vk == VK_RIGHT) {
            if (ctrl) {
                while (e.pos < (int)e.buf.size() && e.buf[e.pos] != L' ') e.pos++;
                while (e.pos < (int)e.buf.size() && e.buf[e.pos] == L' ') e.pos++;
            } else if (e.pos < (int)e.buf.size()) {
                e.pos++;
            } else if (!e.hint.empty()) {
                e.buf += e.hint;
                e.pos = (int)e.buf.size();
                e.hint.clear();
                e.hist_idx = -1;
                e.saved.clear();
                e.plain_nav = true;
            }
            redraw(e); continue;
        }
        if (vk == VK_HOME)  { e.pos = 0;                          redraw(e); continue; }
        if (vk == VK_END) {
            if (e.pos == (int)e.buf.size() && !e.hint.empty()) {
                e.buf += e.hint;
                e.hint.clear();
                e.hist_idx = -1;
                e.saved.clear();
                e.plain_nav = true;
            }
            e.pos = (int)e.buf.size();
            redraw(e); continue;
        }

        // -- UP: step backward through history --
        // On first press, snapshots buf as the filter prefix (or empty if plain_nav).
        // If a hint is already visible, anchors at that entry so UP immediately moves past it.
        if (vk == VK_UP) {
            if (e.hist.empty()) continue;
            if (e.hist_idx == -1) enter_nav(e);
            nav_step(e, -1);
            continue;
        }

        // -- DOWN: step forward through history --
        // No-op in edit mode (hist_idx == -1); only active once UP has been pressed.
        if (vk == VK_DOWN) {
            if (e.hist_idx == -1) continue;
            nav_step(e, +1);
            continue;
        }

        // -- Tab: cycle filesystem completions --
        // First Tab computes all matches for the token under the cursor and stores pre/suf so
        // the rest of the line is preserved. Each subsequent Tab advances to the next match.
        // Auto-dive: if the only match is a directory, the next Tab re-initializes inside it.
        if (vk == VK_TAB) {
            if (!e.tab_on) {
                std::wstring before = e.buf.substr(0, e.pos);
                size_t space = before.find_last_of(L" \t");
                int start = (space == std::wstring::npos) ? 0 : (int)space + 1;
                std::wstring token = before.substr(start);
                std::wstring lower_buf = e.buf;
                std::transform(lower_buf.begin(), lower_buf.end(), lower_buf.begin(), ::towlower);
                bool dirs_only = (lower_buf.substr(0, 3) == L"cd " || lower_buf == L"cd" ||
                                  lower_buf.substr(0, 3) == L"ls " || lower_buf == L"ls");
                e.tab_matches = complete(token, dirs_only);
                if (e.tab_matches.empty()) continue;
                e.tab_on    = true;
                e.tab_idx   = 0;
                e.tab_start = start;
                e.tab_pre   = e.buf.substr(0, start);
                e.tab_suf   = e.buf.substr(e.pos);
            } else {
                std::wstring cur = e.tab_matches[e.tab_idx];
                // auto-dive: single dir match — re-initialize completion inside it
                if (e.tab_matches.size() == 1 && !cur.empty() && cur.back() == L'/') {
                    std::wstring lower_buf = e.buf;
                    std::transform(lower_buf.begin(), lower_buf.end(), lower_buf.begin(), ::towlower);
                    bool dirs_only = (lower_buf.substr(0, 3) == L"cd " || lower_buf == L"cd" ||
                                      lower_buf.substr(0, 3) == L"ls " || lower_buf == L"ls");
                    std::wstring new_token = e.tab_pre + cur;
                    // new_token relative to command (strip the command prefix stored in tab_pre up to space)
                    size_t space = new_token.find_last_of(L" \t");
                    std::wstring path_token = (space == std::wstring::npos) ? new_token : new_token.substr(space + 1);
                    auto matches = complete(path_token, dirs_only);
                    if (!matches.empty()) {
                        e.tab_matches = matches;
                        e.tab_idx     = 0;
                        e.tab_pre     = (space == std::wstring::npos) ? L"" : new_token.substr(0, space + 1);
                        e.tab_suf     = L"";
                    } else {
                        e.tab_idx = 0; // stay on current match, nothing inside
                    }
                } else {
                    e.tab_idx = (e.tab_idx + 1) % (int)e.tab_matches.size();
                }
            }
            std::wstring match = e.tab_matches[e.tab_idx];
            e.buf = e.tab_pre + match + e.tab_suf;
            e.pos = (int)(e.tab_pre.size() + match.size());
            e.hint.clear();
            redraw(e);
            continue;
        }

        // -- Escape: hard reset --
        // Clears everything — buf, hint, nav state — back to a blank prompt.
        if (vk == VK_ESCAPE) {
            e.buf.clear(); e.pos = 0; e.hint.clear(); e.hist_idx = -1; e.saved.clear(); redraw(e);
            continue;
        }

        // -- Ctrl+C: abort current line --
        // Prints ^C, discards buf and any continuation segments, returns empty to signal no execution.
        if (ctrl && vk == 'C') {
            out("^C\r\n");
            e.buf.clear();
            e.full_cmd.clear();
            e.pos = 0;
            return "";
        }

        // -- Printable character: insert and recalculate hint --
        // Exits nav mode (any typed char abandons history browsing) and recomputes the ghost hint
        // from the new buf so it stays in sync with every keystroke.
        if (ch >= 32 && ch != 127) {
            e.hist_idx = -1;
            e.plain_nav = false;
            e.buf.insert(e.pos, 1, ch);
            e.pos++;
            find_hint(e);
            redraw(e);
        }
    }
    return "";
}

// ---- commands ----

// Last directory before a successful cd; enables "cd -" to jump back (session-only).
static std::string prev_dir;
// Last working directory from the previous session; enables "cd --" to restore it.
static std::string last_session_dir;

// Built-in cd: strips the /d compatibility flag, expands ~ to USERPROFILE, resolves "-" to
// prev_dir, "--" to last_session_dir, then calls SetCurrentDirectory. Updates prev_dir only on success.
void cd(const std::string& line) {
    std::string args = line.size() > 2 ? line.substr(3) : "";
    // strip /d flag
    if (args.size() >= 3 && args[0] == '/' && (args[1] == 'd' || args[1] == 'D') && args[2] == ' ')
        args = args.substr(3);
    while (!args.empty() && args.front() == ' ') args.erase(args.begin());
    while (!args.empty() && args.back()  == ' ') args.pop_back();

    if (args.empty()) { out(cwd() + "\r\n"); return; }

    // cd - : jump to previous directory (session-only)
    if (args == "-") {
        if (prev_dir.empty()) { err("No previous directory.\r\n"); return; }
        args = prev_dir;
    }
    // cd -- : restore last session's working directory
    else if (args == "--") {
        if (last_session_dir.empty()) { err("No previous session directory.\r\n"); return; }
        args = last_session_dir;
    }
    // cd ~~ : go to the directory where pcmd.exe lives
    else if (args == "~~") {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        std::wstring ws(exe);
        size_t slash = ws.find_last_of(L"\\/");
        args = to_utf8(slash != std::wstring::npos ? ws.substr(0, slash) : ws);
    }

    // expand ~ to %USERPROFILE%
    if (args[0] == '~') {
        char home[MAX_PATH] = {};
        GetEnvironmentVariableA("USERPROFILE", home, MAX_PATH);
        args = std::string(home) + args.substr(1);
    }

    std::string before = cwd();
    if (!SetCurrentDirectoryW(to_wide(args).c_str()))
        err("The system cannot find the path specified.\r\n");
    else
        prev_dir = before;
}

// Maps a directory entry to its ANSI color escape: hidden→gray, dir→blue, exe→green,
// archive→bold red, image→bold magenta, audio/video→cyan, everything else→empty (default).
static std::string ls_color(const WIN32_FIND_DATAW& fd) {
    bool is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    bool hidden = (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0;
    if (hidden) return GRAY;
    if (is_dir) return BLUE;
    std::wstring name = fd.cFileName;
    size_t dot = name.rfind(L'.');
    if (dot == std::wstring::npos) return "";
    std::wstring ext = name.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);

    // executables — green
    if (ext == L".exe" || ext == L".bat" || ext == L".cmd" ||
        ext == L".ps1" || ext == L".msi")
        return GREEN;

    // archives — bold red
    if (ext == L".zip" || ext == L".tar" || ext == L".gz"  || ext == L".tgz" ||
        ext == L".bz2" || ext == L".xz"  || ext == L".7z"  || ext == L".rar" ||
        ext == L".z"   || ext == L".lz"  || ext == L".lzma"|| ext == L".zst" ||
        ext == L".deb" || ext == L".rpm" || ext == L".cab" || ext == L".iso")
        return "\x1b[1;31m";

    // images — bold magenta
    if (ext == L".jpg"  || ext == L".jpeg" || ext == L".png"  || ext == L".gif"  ||
        ext == L".bmp"  || ext == L".tif"  || ext == L".tiff" || ext == L".svg"  ||
        ext == L".webp" || ext == L".ico"  || ext == L".raw"  || ext == L".heic")
        return "\x1b[1;35m";

    // audio / video — cyan
    if (ext == L".mp3"  || ext == L".wav"  || ext == L".ogg"  || ext == L".flac" ||
        ext == L".aac"  || ext == L".m4a"  || ext == L".wma"  ||
        ext == L".mp4"  || ext == L".mkv"  || ext == L".avi"  || ext == L".mov"  ||
        ext == L".wmv"  || ext == L".flv"  || ext == L".webm" || ext == L".m4v")
        return "\x1b[36m";

    return "";
}

// Lists a directory with ANSI colors. Flags (all combinable, order of -s/-t sets sort priority):
//   -a  show hidden files   -s  sort by size desc + show size   -t  sort by time desc + show time
//   -l  show size + time (sort alphabetical unless -s or -t also present)   -r  reverse sort (global)
// Any of -s/-t/-l switches to one-column mode. filter (| grep / | findstr) is case-insensitive name substring.
void ls(const std::string& arg, const std::string& filter = "") {
    // -- collect all flag chars left-to-right across all -xxx tokens, then path --
    bool show_all = false, show_size = false, show_time = false, reverse = false;
    char sort_by = 0; // 0=alpha, 's'=size, 't'=time; first -s or -t wins
    std::string path_s = arg;
    while (!path_s.empty() && path_s.front() == ' ') path_s.erase(path_s.begin());
    while (!path_s.empty() && path_s[0] == '-') {
        size_t sp = path_s.find(' ');
        std::string tok = sp == std::string::npos ? path_s : path_s.substr(0, sp);
        for (char c : tok.substr(1)) {
            if (c == 'a') show_all  = true;
            if (c == 'r') reverse   = true;
            if (c == 'l') { show_size = true; show_time = true; }
            if (c == 's') { show_size = true; if (!sort_by) sort_by = 's'; }
            if (c == 't') { show_time = true; if (!sort_by) sort_by = 't'; }
        }
        path_s = sp == std::string::npos ? "" : path_s.substr(sp + 1);
        while (!path_s.empty() && path_s.front() == ' ') path_s.erase(path_s.begin());
    }
    if (path_s.size() >= 2 && path_s.front() == '"' && path_s.back() == '"')
        path_s = path_s.substr(1, path_s.size() - 2);

    std::wstring wpath = path_s.empty() ? L"." : to_wide(path_s);

    struct entry { std::wstring name; bool is_dir; std::string color; ULONGLONG size; FILETIME mtime; };
    std::vector<entry> dirs, files;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((wpath + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) { out("ls: cannot access '" + path_s + "'\r\n"); return; }
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        bool hidden = (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0;
        if (hidden && !show_all) continue;
        bool is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        ULONGLONG sz = is_dir ? 0 : (((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow);
        (is_dir ? dirs : files).push_back({ name, is_dir, ls_color(fd), sz, fd.ftLastWriteTime });
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    // -- sort each group independently, dirs always above files --
    auto alpha = [](const entry& a, const entry& b) {
        std::wstring la = a.name, lb = b.name;
        std::transform(la.begin(), la.end(), la.begin(), ::towlower);
        std::transform(lb.begin(), lb.end(), lb.begin(), ::towlower);
        return la < lb;
    };
    auto by_size = [](const entry& a, const entry& b) { return a.size > b.size; };
    auto by_time = [](const entry& a, const entry& b) {
        return CompareFileTime(&a.mtime, &b.mtime) > 0;
    };

    auto sort_group = [&](std::vector<entry>& g) {
        if      (sort_by == 's') std::sort(g.begin(), g.end(), by_size);
        else if (sort_by == 't') std::sort(g.begin(), g.end(), by_time);
        else                     std::sort(g.begin(), g.end(), alpha);
        if (reverse) std::reverse(g.begin(), g.end());
    };
    sort_group(dirs);
    sort_group(files);

    std::vector<entry> all;
    all.insert(all.end(), dirs.begin(), dirs.end());
    all.insert(all.end(), files.begin(), files.end());

    // -- filter --
    if (!filter.empty()) {
        std::wstring wfl = to_wide(filter);
        std::transform(wfl.begin(), wfl.end(), wfl.begin(), ::towlower);
        all.erase(std::remove_if(all.begin(), all.end(), [&](const entry& e) {
            std::wstring nl = e.name;
            std::transform(nl.begin(), nl.end(), nl.begin(), ::towlower);
            return nl.find(wfl) == std::wstring::npos;
        }), all.end());
    }

    if (all.empty()) return;

    // -- columnar mode (no -s/-t/-l) --
    if (!show_size && !show_time) {
        int max_w = 0;
        for (auto& e : all) {
            int w = (int)e.name.size() + (e.is_dir ? 1 : 0);
            if (w > max_w) max_w = w;
        }
        int col_w = max_w + 2;
        int tw    = term_width();
        int ncols = std::max(1, tw / col_w);
        int nrows = ((int)all.size() + ncols - 1) / ncols;
        for (int r = 0; r < nrows; r++) {
            std::string row;
            for (int c = 0; c < ncols; c++) {
                int idx = c * nrows + r;
                if (idx >= (int)all.size()) break;
                auto& e = all[idx];
                std::string disp = to_utf8(e.name) + (e.is_dir ? "/" : "");
                int pad = col_w - (int)disp.size();
                if (!e.color.empty()) row += e.color;
                row += disp;
                if (!e.color.empty()) row += RESET;
                bool last = (c == ncols - 1) || (idx + nrows >= (int)all.size());
                if (!last) row += std::string(std::max(0, pad), ' ');
            }
            out(row + "\r\n");
        }
        return;
    }

    // -- one-column mode (-s, -t, or -l) --

    // human-readable size: "42", "1.2K", "3.4M", "1.1G"
    auto fmt_size = [](ULONGLONG b) -> std::string {
        if (b < 1024ULL)               return std::to_string(b);
        if (b < 1024ULL * 1024)        { char s[16]; snprintf(s, sizeof(s), "%.1fK", b / 1024.0);              return s; }
        if (b < 1024ULL * 1024 * 1024) { char s[16]; snprintf(s, sizeof(s), "%.1fM", b / (1024.0*1024));       return s; }
                                         { char s[16]; snprintf(s, sizeof(s), "%.1fG", b / (1024.0*1024*1024)); return s; }
    };

    // modification time as "yyyy-mm-dd HH:MM:SS" in local time
    auto fmt_time = [](FILETIME ft) -> std::string {
        FILETIME lft; FileTimeToLocalFileTime(&ft, &lft);
        SYSTEMTIME st; FileTimeToSystemTime(&lft, &st);
        char s[24];
        snprintf(s, sizeof(s), "%04d-%02d-%02d %02d:%02d:%02d",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        return s;
    };

    int max_w = 0;
    for (auto& e : all) {
        int w = (int)e.name.size() + (e.is_dir ? 1 : 0);
        if (w > max_w) max_w = w;
    }

    for (auto& e : all) {
        std::string name = to_utf8(e.name) + (e.is_dir ? "/" : "");
        std::string row;
        if (!e.color.empty()) row += e.color;
        row += name;
        if (!e.color.empty()) row += RESET;
        row += std::string(max_w - (int)name.size() + 2, ' ');
        if (show_size) {
            std::string sz = e.is_dir ? "" : fmt_size(e.size);
            row += std::string(std::max(0, 6 - (int)sz.size()), ' ') + sz + "  ";
        }
        if (show_time) row += GRAY + fmt_time(e.mtime) + RESET;
        out(row + "\r\n");
    }
}

// Returns %USERPROFILE%\.pcmd\ and creates it if it doesn't exist.
std::string pcmd_dir() {
    wchar_t buf[MAX_PATH];
    GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
    std::string dir = to_utf8(buf) + "\\.pcmd";
    CreateDirectoryW(to_wide(dir).c_str(), NULL); // no-op if already exists
    return dir + "\\";
}

std::string history_path()  { return pcmd_dir() + "history";  }
std::string prev_dir_path() { return pcmd_dir() + "prev_dir"; }

void load_prev_dir() {
    std::ifstream f(prev_dir_path());
    std::string line;
    if (std::getline(f, line) && !line.empty()) last_session_dir = line;
}

void save_prev_dir() {
    std::string cur = cwd();
    if (cur.empty()) return;
    std::ofstream f(prev_dir_path());
    f << cur << "\n";
}

// ---- aliases ----

static std::unordered_map<std::string, std::string> aliases;

std::string aliases_path() { return pcmd_dir() + "aliases"; }

void load_aliases() {
    std::ifstream f(aliases_path());
    std::string line;
    while (std::getline(f, line)) {
        size_t eq = line.find('=');
        if (eq != std::string::npos && eq > 0)
            aliases[line.substr(0, eq)] = line.substr(eq + 1);
    }
}

// Read-modify-write: reads file, applies change, rewrites.
// Safe with parallel sessions — file is always authoritative.
void write_alias(const std::string& name, const std::string& val) {
    std::unordered_map<std::string, std::string> file_aliases;
    { std::ifstream f(aliases_path()); std::string line;
      while (std::getline(f, line)) {
          size_t eq = line.find('=');
          if (eq != std::string::npos && eq > 0)
              file_aliases[line.substr(0, eq)] = line.substr(eq + 1);
      }
    }
    if (val.empty()) file_aliases.erase(name);
    else             file_aliases[name] = val;
    aliases = file_aliases; // keep in-memory in sync
    std::ofstream f(aliases_path());
    for (auto& kv : file_aliases) f << kv.first << "=" << kv.second << "\n";
}

// Expands the first word of line if it matches an alias; appends remaining args.
// Returns empty string if no alias matched.
std::string expand_alias(const std::string& line) {
    size_t sp = line.find(' ');
    std::string name = sp == std::string::npos ? line : line.substr(0, sp);
    std::string namel = name;
    std::transform(namel.begin(), namel.end(), namel.begin(), ::tolower);
    auto it = aliases.find(namel);
    if (it == aliases.end()) return "";
    std::string expanded = it->second;
    if (sp != std::string::npos) expanded += line.substr(sp); // append args
    return expanded;
}

// Reads .history into e.hist and deduplicates, keeping only the last occurrence of each
// command so the most recently used entry wins during UP navigation.
void load_history(editor& e) {
    std::ifstream f(history_path());
    std::string line;
    std::vector<std::wstring> raw;
    while (std::getline(f, line)) {
        if (!line.empty()) raw.push_back(to_wide(line));
    }
    // deduplicate: keep only the last occurrence of each entry
    std::unordered_set<std::wstring> seen;
    for (int i = (int)raw.size() - 1; i >= 0; i--) {
        if (seen.insert(raw[i]).second)
            e.hist.push_back(raw[i]);
    }
    std::reverse(e.hist.begin(), e.hist.end());
}

// Appends a single command to the history file immediately (fish-style: no loss on crash).
void append_history(const std::wstring& cmd) {
    std::ofstream f(history_path(), std::ios::app);
    f << to_utf8(cmd) << "\n";
}

// Re-reads the history file, deduplicates keeping last occurrence, rewrites.
// Safe with parallel sessions: file is the source of truth, not in-memory buffer.
void compact_history() {
    std::string path = history_path();
    std::vector<std::wstring> raw;
    { std::ifstream f(path); std::string line;
      while (std::getline(f, line)) if (!line.empty()) raw.push_back(to_wide(line)); }
    std::vector<std::wstring> deduped;
    std::unordered_set<std::wstring> seen;
    for (int i = (int)raw.size() - 1; i >= 0; i--)
        if (seen.insert(raw[i]).second) deduped.push_back(raw[i]);
    std::reverse(deduped.begin(), deduped.end());
    std::ofstream f(path);
    for (auto& e : deduped) f << to_utf8(e) << "\n";
}

// Spawns "cmd.exe /c <line>" and waits for it to finish. Temporarily restores orig_in_mode
// so Ctrl+C reaches the child, then re-enables raw mode afterward. Returns the child's exit code.
int run(const std::string& line) {
    std::wstring cmd = L"cmd.exe /c " + to_wide(line);
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    STARTUPINFOW si = {}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(NULL, buf.data(), NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi))
        return -1;
    SetConsoleMode(in_h, orig_in_mode);  // restore so Ctrl+C reaches child
    WaitForSingleObject(pi.hProcess, INFINITE);
    SetConsoleMode(in_h, ENABLE_EXTENDED_FLAGS | ENABLE_WINDOW_INPUT);  // back to raw
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

// Runs line through sh.exe (Git-for-Windows) instead of cmd.exe.
// Escapes any " in the command so it can be wrapped in sh -c "...".
// Falls back to run() if sh.exe is not found in PATH.
int run_bash(const std::string& line) {
    std::wstring escaped;
    for (unsigned char c : line) {
        if (c == '"') { escaped += L'\\'; escaped += L'"'; }
        else escaped += (wchar_t)c;
    }
    std::wstring cmd = L"sh.exe -c \"" + escaped + L"\"";
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    STARTUPINFOW si = {}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(NULL, buf.data(), NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi))
        return run(line);
    SetConsoleMode(in_h, orig_in_mode);
    WaitForSingleObject(pi.hProcess, INFINITE);
    SetConsoleMode(in_h, ENABLE_EXTENDED_FLAGS | ENABLE_WINDOW_INPUT);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

// Searches for arg as a built-in or executable in PATH; prints result and returns exit code (0=found, 1=not found).
int which(const std::string& arg) {
    std::string argl = arg;
    std::transform(argl.begin(), argl.end(), argl.begin(), ::tolower);
    static const std::vector<std::string> builtins = {"ls","cd","pwd","cat","exit","which","help","version","alias","unalias"};
    for (auto& b : builtins) {
        if (argl == b) { out(arg + ": pcmd built-in\r\n"); return 0; }
    }
    std::vector<std::string> exts;
    char pathext[4096] = {};
    GetEnvironmentVariableA("PATHEXT", pathext, sizeof(pathext));
    std::stringstream pe(pathext);
    std::string ext;
    while (std::getline(pe, ext, ';')) {
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        exts.push_back(ext);
    }
    if (exts.empty()) exts = {".exe",".cmd",".bat",".com"};
    char path_env[32768] = {};
    GetEnvironmentVariableA("PATH", path_env, sizeof(path_env));
    std::stringstream ps(path_env);
    std::string dir;
    while (std::getline(ps, dir, ';')) {
        if (dir.size() >= 2 && dir.front() == '"' && dir.back() == '"')
            dir = dir.substr(1, dir.size() - 2);
        if (!dir.empty() && dir.back() != '\\') dir += '\\';
        for (auto& e : exts) {
            std::string full = dir + arg + e;
            if (GetFileAttributesA(full.c_str()) != INVALID_FILE_ATTRIBUTES) {
                std::replace(full.begin(), full.end(), '\\', '/');
                out(full + "\r\n");
                return 0;
            }
        }
    }
    out(arg + ": not found\r\n");
    return 1;
}

// ---- cat ----

// Language detected from file extension; drives syntax highlight rules.
enum class lang { none, cpp, py, js, json, md, bat, sol, php, go, rust, cs, java, sh, html };

static lang detect_lang(const std::string& path) {
    size_t dot = path.rfind('.');
    if (dot == std::string::npos) return lang::none;
    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext==".cpp"||ext==".c"||ext==".h"||ext==".hpp"||ext==".cc") return lang::cpp;
    if (ext==".py")                                                   return lang::py;
    if (ext==".js"||ext==".ts"||ext==".jsx"||ext==".tsx")            return lang::js;
    if (ext==".json")                                                 return lang::json;
    if (ext==".md")                                                   return lang::md;
    if (ext==".bat"||ext==".cmd")                                     return lang::bat;
    if (ext==".sol")                                                  return lang::sol;
    if (ext==".php")                                                  return lang::php;
    if (ext==".go")                                                   return lang::go;
    if (ext==".rs")                                                   return lang::rust;
    if (ext==".cs")                                                   return lang::cs;
    if (ext==".java")                                                 return lang::java;
    if (ext==".sh"||ext==".bash")                                     return lang::sh;
    if (ext==".html"||ext==".htm"||ext==".xml"||ext==".svg")         return lang::html;
    return lang::none;
}

// Scan a line left-to-right, coloring string literals (yellow), inline comment suffix (gray),
// and any word matching the keywords list (blue). Used for languages with C-style syntax.
static std::string colorize_inline(const std::string& line,
                                   const std::vector<std::string>& kws,
                                   const std::string& comment2 = "",
                                   char comment1 = 0) {
    std::string res;
    size_t i = 0, n = line.size();
    while (i < n) {
        // two-char comment (// or ::)
        if (!comment2.empty() && i + comment2.size() <= n &&
            line.substr(i, comment2.size()) == comment2) {
            res += GRAY + line.substr(i) + RESET; break;
        }
        // single-char comment (# for py/bat)
        if (comment1 && line[i] == comment1) {
            res += GRAY + line.substr(i) + RESET; break;
        }
        // string literal " or '
        if (line[i] == '"' || line[i] == '\'') {
            char q = line[i]; size_t j = i + 1;
            while (j < n && line[j] != q) { if (line[j] == '\\') j++; j++; }
            if (j < n) j++;
            res += YELLOW + line.substr(i, j - i) + RESET;
            i = j; continue;
        }
        // word / identifier
        if (isalpha((unsigned char)line[i]) || line[i] == '_') {
            size_t j = i;
            while (j < n && (isalnum((unsigned char)line[j]) || line[j] == '_')) j++;
            std::string word = line.substr(i, j - i);
            bool kw = std::find(kws.begin(), kws.end(), word) != kws.end();
            res += kw ? (BLUE + word + RESET) : word;
            i = j; continue;
        }
        res += line[i++];
    }
    return res;
}

// Apply syntax highlighting to one line based on detected language.
static std::string colorize_line(const std::string& line, lang l) {
    if (l == lang::none) return line;
    size_t first = line.find_first_not_of(" \t");
    std::string pfx = (first == std::string::npos) ? "" : line.substr(first);
    std::string pfl = pfx; std::transform(pfl.begin(), pfl.end(), pfl.begin(), ::tolower);

    if (l == lang::md) {
        if (!pfx.empty() && pfx[0] == '#')                              return YELLOW + line + RESET;
        if (pfx.size()>=2 && (pfx[0]=='-'||pfx[0]=='*') && pfx[1]==' ') return BLUE + line + RESET;
        if (pfx.size()>=3 && pfx.substr(0,3)=="```")                    return GREEN + line + RESET;
        return line;
    }
    if (l == lang::bat) {
        if (pfl.size()>=4 && pfl.substr(0,4)=="rem ") return GRAY + line + RESET;
        if (pfx.size()>=2 && pfx.substr(0,2)=="::")   return GRAY + line + RESET;
        static const std::vector<std::string> kw = {
            "echo","set","if","else","for","call","goto","exit","mkdir","del",
            "copy","move","pushd","popd","setlocal","endlocal","defined","exist"
        };
        return colorize_inline(line, kw);
    }
    if (l == lang::json) {
        static const std::vector<std::string> kw = {"true","false","null"};
        return colorize_inline(line, kw);
    }
    if (l == lang::cpp) {
        if (pfx.size()>=2 && pfx.substr(0,2)=="//") return GRAY + line + RESET;
        if (pfx.size()>=2 && pfx.substr(0,2)=="/*") return GRAY + line + RESET;
        if (!pfx.empty() && pfx[0]=='#')             return YELLOW + line + RESET;
        static const std::vector<std::string> kw = {
            "auto","bool","break","case","char","class","const","continue","default",
            "delete","do","double","else","enum","explicit","false","float","for",
            "friend","if","inline","int","long","namespace","new","nullptr","operator",
            "override","private","protected","public","return","short","signed","sizeof",
            "static","struct","switch","template","this","throw","true","try","typedef",
            "typename","union","unsigned","using","virtual","void","volatile","while"
        };
        return colorize_inline(line, kw, "//");
    }
    if (l == lang::py) {
        if (!pfx.empty() && pfx[0]=='#') return GRAY + line + RESET;
        static const std::vector<std::string> kw = {
            "and","as","assert","async","await","break","class","continue","def","del",
            "elif","else","except","False","finally","for","from","global","if","import",
            "in","is","lambda","None","nonlocal","not","or","pass","raise","return",
            "True","try","while","with","yield"
        };
        return colorize_inline(line, kw, "", '#');
    }
    if (l == lang::js) {
        if (pfx.size()>=2 && pfx.substr(0,2)=="//") return GRAY + line + RESET;
        static const std::vector<std::string> kw = {
            "async","await","break","case","catch","class","const","continue","debugger",
            "default","delete","do","else","export","extends","false","finally","for",
            "function","if","import","in","instanceof","let","new","null","of","return",
            "static","super","switch","this","throw","true","try","typeof","undefined",
            "var","void","while","with","yield"
        };
        return colorize_inline(line, kw, "//");
    }
    if (l == lang::html) {
        if (pfx.size()>=4 && pfx.substr(0,4)=="<!--") return GRAY + line + RESET;
        static const std::vector<std::string> kw = {
            // tags
            "html","head","body","title","meta","link","script","style","base",
            "div","span","p","a","br","hr","img","input","button","form","label",
            "select","option","optgroup","textarea","fieldset","legend",
            "h1","h2","h3","h4","h5","h6","ul","ol","li","dl","dt","dd",
            "table","thead","tbody","tfoot","tr","th","td","caption","colgroup","col",
            "header","footer","main","nav","section","article","aside","figure","figcaption",
            "details","summary","dialog","template","slot","canvas","svg","path","rect",
            "circle","ellipse","line","polyline","polygon","text","g","defs","use","symbol",
            "audio","video","source","track","iframe","embed","object","param","picture",
            "pre","code","blockquote","cite","q","abbr","acronym","address","em","strong",
            "small","mark","del","ins","sub","sup","s","u","b","i","bdi","bdo","wbr",
            "noscript","noframes","area","map",
            // attributes
            "class","id","href","src","type","name","value","placeholder","action",
            "method","target","rel","charset","content","lang","dir","style",
            "width","height","alt","title","role","aria","data","for","checked",
            "disabled","readonly","required","multiple","selected","hidden","tabindex",
            "onclick","onload","onchange","oninput","onsubmit","defer","async","crossorigin"
        };
        return colorize_inline(line, kw);
    }
    if (l == lang::php) {
        if (pfx.size()>=2 && pfx.substr(0,2)=="//") return GRAY + line + RESET;
        if (pfx.size()>=2 && pfx.substr(0,2)=="/*") return GRAY + line + RESET;
        if (!pfx.empty() && pfx[0]=='#')             return GRAY + line + RESET;
        static const std::vector<std::string> kw = {
            "abstract","array","as","break","callable","case","catch","class","clone",
            "const","continue","declare","default","do","echo","else","elseif","empty",
            "enddeclare","endfor","endforeach","endif","endswitch","endwhile","enum",
            "extends","final","finally","fn","for","foreach","function","global","goto",
            "if","implements","include","include_once","instanceof","insteadof","interface",
            "isset","list","match","namespace","new","null","print","private","protected",
            "public","readonly","require","require_once","return","static","switch","throw",
            "trait","true","false","try","unset","use","var","while","yield","int","float",
            "string","bool","void","never","mixed","self","parent"
        };
        return colorize_inline(line, kw, "//", '#');
    }
    if (l == lang::go) {
        if (pfx.size()>=2 && pfx.substr(0,2)=="//") return GRAY + line + RESET;
        if (pfx.size()>=2 && pfx.substr(0,2)=="/*") return GRAY + line + RESET;
        static const std::vector<std::string> kw = {
            "break","case","chan","const","continue","default","defer","else","fallthrough",
            "for","func","go","goto","if","import","interface","map","package","range",
            "return","select","struct","switch","type","var","nil","true","false",
            "int","int8","int16","int32","int64","uint","uint8","uint16","uint32","uint64",
            "uintptr","float32","float64","complex64","complex128","byte","rune","string",
            "bool","error","any","make","new","len","cap","append","copy","delete","close",
            "panic","recover","print","println"
        };
        return colorize_inline(line, kw, "//");
    }
    if (l == lang::rust) {
        if (pfx.size()>=2 && pfx.substr(0,2)=="//") return GRAY + line + RESET;
        if (pfx.size()>=2 && pfx.substr(0,2)=="/*") return GRAY + line + RESET;
        static const std::vector<std::string> kw = {
            "as","async","await","break","const","continue","crate","dyn","else","enum",
            "extern","false","fn","for","if","impl","in","let","loop","match","mod","move",
            "mut","pub","ref","return","self","Self","static","struct","super","trait",
            "true","type","unsafe","use","where","while","i8","i16","i32","i64","i128",
            "isize","u8","u16","u32","u64","u128","usize","f32","f64","bool","char","str",
            "String","Vec","Option","Result","Some","None","Ok","Err","Box","Rc","Arc"
        };
        return colorize_inline(line, kw, "//");
    }
    if (l == lang::cs) {
        if (pfx.size()>=2 && pfx.substr(0,2)=="//") return GRAY + line + RESET;
        if (pfx.size()>=2 && pfx.substr(0,2)=="/*") return GRAY + line + RESET;
        static const std::vector<std::string> kw = {
            "abstract","as","base","bool","break","byte","case","catch","char","checked",
            "class","const","continue","decimal","default","delegate","do","double","else",
            "enum","event","explicit","extern","false","finally","fixed","float","for",
            "foreach","goto","if","implicit","in","int","interface","internal","is","lock",
            "long","namespace","new","null","object","operator","out","override","params",
            "private","protected","public","readonly","ref","return","sbyte","sealed","short",
            "sizeof","static","string","struct","switch","this","throw","true","try","typeof",
            "uint","ulong","unchecked","unsafe","ushort","using","var","virtual","void",
            "volatile","while","async","await","dynamic","record","init","required","with"
        };
        return colorize_inline(line, kw, "//");
    }
    if (l == lang::java) {
        if (pfx.size()>=2 && pfx.substr(0,2)=="//") return GRAY + line + RESET;
        if (pfx.size()>=2 && pfx.substr(0,2)=="/*") return GRAY + line + RESET;
        static const std::vector<std::string> kw = {
            "abstract","assert","boolean","break","byte","case","catch","char","class",
            "const","continue","default","do","double","else","enum","extends","final",
            "finally","float","for","goto","if","implements","import","instanceof","int",
            "interface","long","native","new","null","package","private","protected","public",
            "return","short","static","strictfp","super","switch","synchronized","this",
            "throw","throws","transient","true","false","try","var","void","volatile","while",
            "record","sealed","permits","yield"
        };
        return colorize_inline(line, kw, "//");
    }
    if (l == lang::sh) {
        if (!pfx.empty() && pfx[0]=='#') return GRAY + line + RESET;
        static const std::vector<std::string> kw = {
            "if","then","else","elif","fi","for","in","do","done","while","until","case",
            "esac","select","function","return","exit","break","continue","shift","local",
            "readonly","export","unset","source","declare","typeset","eval","exec","trap",
            "wait","read","echo","printf","test","true","false"
        };
        return colorize_inline(line, kw, "", '#');
    }
    if (l == lang::sol) {
        if (pfx.size()>=2 && pfx.substr(0,2)=="//") return GRAY + line + RESET;
        if (pfx.size()>=2 && pfx.substr(0,2)=="/*") return GRAY + line + RESET;
        static const std::vector<std::string> kw = {
            "pragma","contract","interface","library","abstract","is","using","import",
            "function","modifier","event","error","struct","enum","mapping","constructor",
            "fallback","receive","returns","return",
            "public","private","internal","external","view","pure","payable","virtual","override",
            "memory","storage","calldata","indexed","anonymous",
            "uint","uint8","uint16","uint32","uint64","uint128","uint256",
            "int","int8","int16","int32","int64","int128","int256",
            "bytes","bytes1","bytes2","bytes4","bytes8","bytes16","bytes32",
            "address","bool","string","fixed","ufixed",
            "if","else","for","while","do","break","continue","new","delete","emit","revert",
            "require","assert","selfdestruct","type","try","catch",
            "true","false","wei","gwei","ether","seconds","minutes","hours","days","weeks"
        };
        return colorize_inline(line, kw, "//");
    }
    return line;
}

// ---- image rendering (stb_image + 2x2 block Unicode) ----

static bool is_image_ext(const std::string& path) {
    size_t dot = path.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext==".jpg"||ext==".jpeg"||ext==".png"||ext==".bmp"||ext==".gif"||ext==".tga"||ext==".psd";
}

struct imgpixel { uint8_t r, g, b; };

static imgpixel imgsample(const uint8_t* img, int w, int x, int y) {
    const uint8_t* p = img + (y * w + x) * 3;
    return { p[0], p[1], p[2] };
}

static char* imgwrite_u8(char* p, uint8_t v) {
    if (v >= 100) { *p++ = '0' + v/100; *p++ = '0' + (v/10)%10; }
    else if (v >= 10) { *p++ = '0' + v/10; }
    *p++ = '0' + v%10;
    return p;
}

static constexpr const char* imgquad[16] = {
    " ","\xE2\x96\x98","\xE2\x96\x9D","\xE2\x96\x80",
    "\xE2\x96\x96","\xE2\x96\x8C","\xE2\x96\x9E","\xE2\x96\x9B",
    "\xE2\x96\x97","\xE2\x96\x9A","\xE2\x96\x90","\xE2\x96\x9C",
    "\xE2\x96\x84","\xE2\x96\x99","\xE2\x96\x9F","\xE2\x96\x88",
};

static float imgluma(imgpixel p) { return 0.299f*p.r + 0.587f*p.g + 0.114f*p.b; }
static float imgdist2(imgpixel a, imgpixel b) {
    float dr=a.r-b.r, dg=a.g-b.g, db=a.b-b.b;
    return dr*dr + dg*dg + db*db;
}

static char* imgpush_cell(char* p, imgpixel tl, imgpixel tr, imgpixel bl, imgpixel br) {
    imgpixel px[4] = { tl, tr, bl, br };
    float lu[4] = { imgluma(px[0]), imgluma(px[1]), imgluma(px[2]), imgluma(px[3]) };
    int lo=0, hi=0;
    for (int i=1; i<4; ++i) {
        if (lu[i] < lu[lo]) lo = i;
        if (lu[i] > lu[hi]) hi = i;
    }
    imgpixel c0=px[lo], c1=px[hi];
    int mask = 0;
    for (int iter=0; iter<3; ++iter) {
        mask = 0;
        for (int i=0; i<4; ++i)
            if (imgdist2(px[i], c1) < imgdist2(px[i], c0))
                mask |= (1 << i);
        int r0=0,g0=0,b0=0,n0=0, r1=0,g1=0,b1=0,n1=0;
        for (int i=0; i<4; ++i) {
            if (mask & (1<<i)) { r1+=px[i].r; g1+=px[i].g; b1+=px[i].b; ++n1; }
            else               { r0+=px[i].r; g0+=px[i].g; b0+=px[i].b; ++n0; }
        }
        if (n0) c0 = { (uint8_t)(r0/n0), (uint8_t)(g0/n0), (uint8_t)(b0/n0) };
        if (n1) c1 = { (uint8_t)(r1/n1), (uint8_t)(g1/n1), (uint8_t)(b1/n1) };
    }
    *p++='\033'; *p++='['; *p++='3'; *p++='8'; *p++=';'; *p++='2'; *p++=';';
    p=imgwrite_u8(p,c1.r); *p++=';'; p=imgwrite_u8(p,c1.g); *p++=';'; p=imgwrite_u8(p,c1.b); *p++='m';
    *p++='\033'; *p++='['; *p++='4'; *p++='8'; *p++=';'; *p++='2'; *p++=';';
    p=imgwrite_u8(p,c0.r); *p++=';'; p=imgwrite_u8(p,c0.g); *p++=';'; p=imgwrite_u8(p,c0.b); *p++='m';
    for (const char* g=imgquad[mask]; *g;) *p++=*g++;
    return p;
}

static int cat_image(const std::string& path) {
    int img_w, img_h;
    uint8_t* img = stbi_load(path.c_str(), &img_w, &img_h, nullptr, 3);
    if (!img) { out("cat: cannot load image '" + path + "'\r\n"); return 1; }

    CONSOLE_SCREEN_BUFFER_INFO csbi;
    int term_w = 80, term_h = 24;
    if (GetConsoleScreenBufferInfo(out_h, &csbi)) {
        term_w = csbi.srWindow.Right  - csbi.srWindow.Left + 1;
        term_h = csbi.srWindow.Bottom - csbi.srWindow.Top  + 1;
    }
    --term_h; // reserve one row for the prompt

    int out_w = term_w;
    int out_h2 = (int)((double)img_h / img_w * out_w / 2.0 + 0.5);
    if (out_h2 > term_h) {
        out_h2 = term_h;
        out_w = (int)((double)img_w / img_h * out_h2 * 2.0 + 0.5);
    }
    if (out_w < 1) out_w = 1;
    if (out_h2 < 1) out_h2 = 1;

    std::vector<char> buf((size_t)out_h2 * (out_w * 41 + 6));
    char* p = buf.data();

    const double x_scale = (double)img_w / (out_w * 2);
    const double y_scale = (double)img_h / (out_h2 * 2);

    for (int row=0; row<out_h2; ++row) {
        int y0 = (int)((2*row)   * y_scale); if (y0 >= img_h) y0 = img_h-1;
        int y1 = (int)((2*row+1) * y_scale); if (y1 >= img_h) y1 = img_h-1;
        for (int col=0; col<out_w; ++col) {
            int x0 = (int)((2*col)   * x_scale); if (x0 >= img_w) x0 = img_w-1;
            int x1 = (int)((2*col+1) * x_scale); if (x1 >= img_w) x1 = img_w-1;
            p = imgpush_cell(p,
                imgsample(img, img_w, x0, y0), imgsample(img, img_w, x1, y0),
                imgsample(img, img_w, x0, y1), imgsample(img, img_w, x1, y1));
        }
        *p++='\033'; *p++='['; *p++='0'; *p++='m'; *p++='\r'; *p++='\n';
    }

    DWORD written;
    WriteConsoleA(out_h, buf.data(), (DWORD)(p - buf.data()), &written, nullptr);
    stbi_image_free(img);
    return 0;
}

// ---- video rendering (ffmpeg pipe) ----

static bool is_video_ext(const std::string& path) {
    size_t dot = path.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext==".mp4"||ext==".mkv"||ext==".avi"||ext==".mov"||ext==".webm"||ext==".flv"||ext==".wmv";
}

static int cat_video(const std::string& path) {
    // Query dimensions and duration via ffprobe
    int vid_w = 0, vid_h = 0;
    double duration = 0.0;
    {
        char probe[2048];
        snprintf(probe, sizeof(probe),
            "ffprobe -v error -select_streams v:0"
            " -show_entries stream=width,height -of csv=p=0 \"%s\"",
            path.c_str());
        FILE* fp = _popen(probe, "r");
        if (fp) { fscanf(fp, "%d,%d", &vid_w, &vid_h); _pclose(fp); }
    }
    {
        char probe[2048];
        snprintf(probe, sizeof(probe),
            "ffprobe -v error -show_entries format=duration -of csv=p=0 \"%s\"",
            path.c_str());
        FILE* fp = _popen(probe, "r");
        if (fp) { fscanf(fp, "%lf", &duration); _pclose(fp); }
    }
    if (vid_w <= 0 || vid_h <= 0) {
        out("cat: cannot read video dimensions (is ffmpeg installed?)\r\n");
        return 1;
    }

    CONSOLE_SCREEN_BUFFER_INFO csbi;
    int tw = 80, th = 24;
    if (GetConsoleScreenBufferInfo(out_h, &csbi)) {
        tw = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        th = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    }
    --th;

    int out_w = tw;
    int out_h2 = (int)((double)vid_h / vid_w * out_w / 2.0 + 0.5);
    if (out_h2 > th) {
        out_h2 = th;
        out_w = (int)((double)vid_w / vid_h * out_h2 * 2.0 + 0.5);
    }
    if (out_w < 1) out_w = 1;
    if (out_h2 < 1) out_h2 = 1;

    int frame_w = out_w * 2, frame_h = out_h2 * 2;
    double fps = 24.0;
    long long frame_ms = (long long)(1000.0 / fps);

    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
        "ffmpeg -hide_banner -loglevel quiet -i \"%s\""
        " -vf fps=%.3f,scale=%d:%d -f rawvideo -pix_fmt rgb24 -",
        path.c_str(), fps, frame_w, frame_h);

    FILE* pipe = _popen(cmd, "rb");
    if (!pipe) { out("cat: failed to open video pipe\r\n"); return 1; }

    size_t frame_bytes = (size_t)frame_w * frame_h * 3;
    std::vector<uint8_t> rawframe(frame_bytes);
    std::vector<char> vbuf((size_t)out_h2 * (out_w * 41 + 6) + 8);

    // extract filename for title
    size_t slash = path.find_last_of("/\\");
    std::string fname = (slash == std::string::npos) ? path : path.substr(slash + 1);

    // format seconds as m:ss
    auto fmt_ts = [](double secs) -> std::string {
        int s = (int)secs;
        char buf[16]; snprintf(buf, sizeof(buf), "%d:%02d", s/60, s%60);
        return buf;
    };
    std::string total_ts = fmt_ts(duration);

    char orig_title[512] = {};
    GetConsoleTitleA(orig_title, sizeof(orig_title));

    out("\033[?25l\033[2J\033[H"); // hide cursor, clear screen
    ctrl_c_fired = false;

    bool stop = false;
    int frame_no = 0;
    while (!stop && fread(rawframe.data(), 1, frame_bytes, pipe) == frame_bytes) {
        ULONGLONG t0 = GetTickCount64();

        // update window title with timestamp
        {
            std::string ts = fmt_ts(frame_no / fps);
            std::string title = ts + "/" + total_ts + " - " + fname;
            SetConsoleTitleA(title.c_str());
        }
        ++frame_no;

        char* p = vbuf.data();
        *p++='\033'; *p++='['; *p++='H'; // cursor home
        for (int row = 0; row < out_h2; ++row) {
            for (int col = 0; col < out_w; ++col) {
                auto px = [&](int x, int y) -> imgpixel {
                    const uint8_t* d = rawframe.data() + (y * frame_w + x) * 3;
                    return { d[0], d[1], d[2] };
                };
                p = imgpush_cell(p, px(col*2,row*2),   px(col*2+1,row*2),
                                    px(col*2,row*2+1), px(col*2+1,row*2+1));
            }
            *p++='\033'; *p++='['; *p++='0'; *p++='m'; *p++='\r'; *p++='\n';
        }
        DWORD wr;
        WriteFile(out_h, vbuf.data(), (DWORD)(p - vbuf.data()), &wr, nullptr);

        // check for Esc or Ctrl+C to stop playback
        if (ctrl_c_fired) stop = true;
        DWORD nevents = 0;
        GetNumberOfConsoleInputEvents(in_h, &nevents);
        while (nevents-- > 0) {
            INPUT_RECORD ir; DWORD rd;
            ReadConsoleInput(in_h, &ir, 1, &rd);
            if (ir.EventType == KEY_EVENT && ir.Event.KeyEvent.bKeyDown) {
                WORD vk = ir.Event.KeyEvent.wVirtualKeyCode;
                DWORD ctrl = ir.Event.KeyEvent.dwControlKeyState;
                if (vk == VK_ESCAPE || (vk == 'C' && (ctrl & (LEFT_CTRL_PRESSED|RIGHT_CTRL_PRESSED))))
                    stop = true;
            }
        }

        ULONGLONG elapsed = GetTickCount64() - t0;
        if ((long long)elapsed < frame_ms) Sleep((DWORD)(frame_ms - elapsed));
    }

    _pclose(pipe);
    out("\033[0m\033[?25h\r\n"); // reset colors, show cursor
    SetConsoleTitleA(orig_title);
    return 0;
}

// Reads a file and prints it with syntax highlighting. filter is an optional case-insensitive
// substring — only lines containing it are shown (like cat file | grep word).
int cat(const std::string& path, const std::string& filter = "") {
    std::string p = path;
    if (p.size()>=2 && p.front()=='"' && p.back()=='"') p = p.substr(1, p.size()-2);
    std::replace(p.begin(), p.end(), '/', '\\');
    if (is_image_ext(p)) return cat_image(p);
    if (is_video_ext(p)) return cat_video(p);
    std::ifstream f(p);
    if (!f.is_open()) { out("cat: cannot open '" + path + "'\r\n"); return 1; }
    lang l = detect_lang(p);
    std::string flt = filter;
    std::transform(flt.begin(), flt.end(), flt.begin(), ::tolower);
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back()=='\r') line.pop_back();
        if (!flt.empty()) {
            std::string ll = line;
            std::transform(ll.begin(), ll.end(), ll.begin(), ::tolower);
            if (ll.find(flt) == std::string::npos) continue;
        }
        out(colorize_line(line, l) + "\r\n");
    }
    return 0;
}

// ---- main ----

int main() {
    out_h = GetStdHandle(STD_OUTPUT_HANDLE);
    err_h = GetStdHandle(STD_ERROR_HANDLE);
    in_h  = GetStdHandle(STD_INPUT_HANDLE);
    SetConsoleCtrlHandler(ctrl_handler, TRUE);

    // enable ANSI
    DWORD out_mode = 0;
    GetConsoleMode(out_h, &out_mode);
    SetConsoleMode(out_h, out_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    // raw input: no line buffer, no echo, no processed ctrl
    GetConsoleMode(in_h, &orig_in_mode);
    SetConsoleMode(in_h, ENABLE_EXTENDED_FLAGS | ENABLE_WINDOW_INPUT);

    SetConsoleCP(65001);
    SetConsoleOutputCP(65001);

    out(
        "\x1b[38;5;75m\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x84"                                                       // P: ███▄
        "\x1b[38;5;226m \xe2\x96\x84\xe2\x96\x88\xe2\x96\x88 \xe2\x96\x88\xe2\x96\x84 \xe2\x96\x84\xe2\x96\x88 \xe2\x96\x88\xe2\x96\x88\xe2\x96\x84\r\n"  // CMD:  ▄██ █▄ ▄█ ██▄
        "\x1b[38;5;75m\xe2\x96\x88  \xe2\x96\x88"                                                                              // P: █  █
        "\x1b[38;5;226m \xe2\x96\x88   \xe2\x96\x88 \xe2\x96\x88 \xe2\x96\x88 \xe2\x96\x88 \xe2\x96\x88\r\n"                  // CMD:  █   █ █ █ █ █
        "\x1b[38;5;75m\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x80"                                                       // P: ███▀
        "\x1b[38;5;226m \xe2\x96\x88   \xe2\x96\x88   \xe2\x96\x88 \xe2\x96\x88 \xe2\x96\x88\r\n"                             // CMD:  █   █   █ █ █
        "\x1b[38;5;75m\xe2\x96\x88"                                                                                            // P: █
        "\x1b[38;5;226m    \xe2\x96\x88   \xe2\x96\x88   \xe2\x96\x88 \xe2\x96\x88 \xe2\x96\x88\r\n"                          // CMD:     █   █   █ █ █
        "\x1b[38;5;75m\xe2\x96\x88"                                                                                            // P: █
        "\x1b[38;5;226m    \xe2\x96\x80\xe2\x96\x88\xe2\x96\x88 \xe2\x96\x88   \xe2\x96\x88 \xe2\x96\x88\xe2\x96\x88\xe2\x96\x80\r\n"  // CMD:     ▀██ █   █ ██▀
        RESET GRAY "Power cmd v" VERSION RESET "\r\n"
    );

    bool elev = elevated();
    editor e;
    load_history(e);
    load_prev_dir();
    load_aliases();
    g_editor = &e;

    int last_code = 0;
    while (true) {
        std::string dir  = cwd();
        std::string name = folder(dir);
        std::string t    = cur_time();
        std::wstring git_root;
        std::string b    = branch(git_root);
        bool d           = b.empty() ? false : dirty(git_root);

        // if cursor is not at column 0, previous output had no trailing newline
        {
            CONSOLE_SCREEN_BUFFER_INFO csbi;
            if (GetConsoleScreenBufferInfo(out_h, &csbi) && csbi.dwCursorPosition.X > 0)
                out("\r\n");
        }

        SetConsoleTitleA(name.c_str());

        auto p = make_prompt(elev, t, name, b, d, last_code);
        e.prompt_str  = p.str;
        e.prompt_vis  = p.vis;
        e.prev_pos    = 0;
        e.full_cmd.clear();
        out(p.str);

        std::string line = readline(e);

        while (!line.empty() && line.front() == ' ') line.erase(line.begin());
        while (!line.empty() && line.back()  == ' ') line.pop_back();

        if (line.empty()) continue;

        std::string lower = line;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

        if (lower == "exit") { compact_history(); save_prev_dir(); break; }

        // alias / unalias
        if (lower == "alias") {
            if (aliases.empty()) { out(GRAY "No aliases defined.\r\n" RESET); }
            else for (auto& kv : aliases)
                out(GREEN + kv.first + RESET "=" + kv.second + "\r\n");
            last_code = 0; continue;
        }
        if (lower.size() > 6 && lower.substr(0, 6) == "alias ") {
            std::string rest = line.substr(6);
            while (!rest.empty() && rest.front() == ' ') rest.erase(rest.begin());
            size_t eq = rest.find('=');
            if (eq == std::string::npos) {
                // alias ll — show one
                std::string namel = rest;
                std::transform(namel.begin(), namel.end(), namel.begin(), ::tolower);
                auto it = aliases.find(namel);
                if (it == aliases.end()) err("alias: " + rest + ": not found\r\n");
                else out(GREEN + it->first + RESET "=" + it->second + "\r\n");
            } else {
                std::string name = rest.substr(0, eq);
                std::string val  = rest.substr(eq + 1);
                std::transform(name.begin(), name.end(), name.begin(), ::tolower);
                if (val.empty()) { write_alias(name, ""); }  // alias ll= removes it
                else { write_alias(name, val); }
            }
            last_code = 0; continue;
        }
        if (lower.size() > 8 && lower.substr(0, 8) == "unalias ") {
            std::string name = line.substr(8);
            while (!name.empty() && name.front() == ' ') name.erase(name.begin());
            std::transform(name.begin(), name.end(), name.begin(), ::tolower);
            if (aliases.count(name)) { write_alias(name, ""); }
            else err("unalias: " + name + ": not found\r\n");
            last_code = 0; continue;
        }

        // expand alias (recursion guard: expanded line must not start with same name)
        {
            std::string expanded = expand_alias(line);
            if (!expanded.empty()) {
                size_t sp = expanded.find(' ');
                std::string expl = sp == std::string::npos ? expanded : expanded.substr(0, sp);
                std::transform(expl.begin(), expl.end(), expl.begin(), ::tolower);
                size_t sp2 = lower.find(' ');
                std::string orig = sp2 == std::string::npos ? lower : lower.substr(0, sp2);
                if (expl != orig) { line = expanded; lower = expl; }
            }
        }

        if (lower == "help") {
            out(
                GREEN "version" RESET " Print pcmd version\r\n"
                GREEN "pwd" RESET "     Print current directory\r\n"
                GREEN "ls" RESET "      Colored listing  -a all  -s by size  -t by time  -l long  -r reverse  | grep <word>\r\n"
                GREEN "cd" RESET "      Change directory  ~ home  - prev dir\r\n"
                GREEN "cat" RESET "     Print file with syntax highlighting  | grep <word>\r\n"
                "        Image files (jpg png bmp gif) rendered inline as 24-bit color block art\r\n"
                "        Video files (mp4 mkv avi mov webm) played inline  Esc/Ctrl+C to stop  requires ffmpeg\r\n"
                GREEN "which" RESET "   Locate a command in PATH or identify built-ins\r\n"
                GREEN "alias" RESET "   alias ll=ls -l  define · alias ll  show · alias  list all\r\n"
                GREEN "unalias" RESET " Remove an alias\r\n"
                GREEN "help" RESET "    Show this help\r\n"
                GREEN "exit" RESET "    Exit pcmd\r\n"
                GRAY "All other commands are passed to cmd.exe" RESET "\r\n"
            );
            last_code = 0;
            continue;
        }

        if (lower == "version") {
            out("pcmd v" VERSION "\r\n");
            last_code = 0;
            continue;
        }

        if (lower == "pwd") {
            out(cwd() + "\r\n");
            last_code = 0;
            continue;
        }

        if (lower.size() >= 6 && lower.substr(0, 6) == "which ") {
            std::string arg = line.substr(6);
            while (!arg.empty() && arg.front() == ' ') arg.erase(arg.begin());
            last_code = which(arg);
            continue;
        }

        // cat <file> | grep <word>  or  cat <file> | findstr <word>
        if (lower.size() >= 4 && lower.substr(0, 4) == "cat ") {
            std::string rest = line.substr(4);
            std::string filter;
            size_t pipe = rest.find('|');
            if (pipe != std::string::npos) {
                std::string rhs = rest.substr(pipe + 1);
                while (!rhs.empty() && rhs.front() == ' ') rhs.erase(rhs.begin());
                std::string rhl = rhs; std::transform(rhl.begin(), rhl.end(), rhl.begin(), ::tolower);
                auto word = [](const std::string& s, size_t skip) {
                    std::string w = s.substr(skip);
                    while (!w.empty() && w.front() == ' ') w.erase(w.begin());
                    size_t sp = w.find(' ');
                    return sp == std::string::npos ? w : w.substr(0, sp);
                };
                if (rhl.size() >= 5 && rhl.substr(0, 5) == "grep ")    filter = word(rhs, 5);
                if (rhl.size() >= 8 && rhl.substr(0, 8) == "findstr ") filter = word(rhs, 8);
                rest = rest.substr(0, pipe);
            }
            while (!rest.empty() && rest.front() == ' ') rest.erase(rest.begin());
            while (!rest.empty() && rest.back()  == ' ') rest.pop_back();
            last_code = cat(rest, filter);
            continue;
        }

        // ls [path] | grep <word>  or  ls [path] | findstr <word>
        // Intercept before passing to cmd.exe so the built-in ls output is filterable.
        if (lower == "ls --help") {
            out(
                GREEN "ls" RESET " [flags] [path] [| grep <word>]\r\n"
                "\r\n"
                "Flags\r\n"
                "  " GREEN "-a" RESET "  show hidden files\r\n"
                "  " GREEN "-l" RESET "  long format: size + time, sorted alphabetically\r\n"
                "  " GREEN "-s" RESET "  sort by size descending, show size\r\n"
                "  " GREEN "-t" RESET "  sort by time descending, show time\r\n"
                "  " GREEN "-r" RESET "  reverse sort order (global, works with any flag)\r\n"
                "\r\n"
                "Flags combine freely: " GRAY "-al  -tr  -st  -trl  -a -l -r" RESET "\r\n"
                "When both -s and -t are given, first one sets sort: " GRAY "-st" RESET " sorts by size, " GRAY "-ts" RESET " by time\r\n"
                "-l alone shows size + time columns without changing sort order\r\n"
                "\r\n"
                "Pipe filter\r\n"
                "  " GRAY "ls | grep <word>     case-insensitive name filter, combinable with flags\r\n"
                "  ls -tr | grep cpp   newest-first, only names containing \"cpp\"" RESET "\r\n"
            );
            last_code = 0;
            continue;
        }

        if (lower == "ls" || (lower.size() >= 3 && lower.substr(0, 3) == "ls ")) {
            std::string rest = line.size() >= 3 ? line.substr(3) : "";
            std::string filter;
            // detect "| grep <word>" or "| findstr <word>"
            size_t pipe = rest.find('|');
            if (pipe != std::string::npos) {
                std::string rhs = rest.substr(pipe + 1);
                while (!rhs.empty() && rhs.front() == ' ') rhs.erase(rhs.begin());
                std::string rhs_lower = rhs;
                std::transform(rhs_lower.begin(), rhs_lower.end(), rhs_lower.begin(), ::tolower);
                auto extract_word = [](const std::string& s, size_t skip) {
                    std::string word = s.substr(skip);
                    while (!word.empty() && word.front() == ' ') word.erase(word.begin());
                    size_t sp = word.find(' ');
                    return sp == std::string::npos ? word : word.substr(0, sp);
                };
                if (rhs_lower.size() >= 5 && rhs_lower.substr(0, 5) == "grep ")
                    filter = extract_word(rhs, 5);
                else if (rhs_lower.size() >= 8 && rhs_lower.substr(0, 8) == "findstr ")
                    filter = extract_word(rhs, 8);
                rest = rest.substr(0, pipe);
            }
            while (!rest.empty() && rest.back() == ' ') rest.pop_back();
            ls(rest, filter);
            last_code = 0;
            continue;
        }

        if (lower == "cd" || lower.substr(0, 3) == "cd " ||
            (lower.size() >= 6 && lower.substr(0, 6) == "cd /d ")) {
            cd(line);
            last_code = 0;
            continue;
        }

        // auto-cd: if input is a valid directory path (e.g. src/ from tab/ls), cd into it
        {
            std::wstring wpath = to_wide(line);
            if (!wpath.empty() && (wpath.back() == L'/' || wpath.back() == L'\\'))
                wpath.pop_back();
            DWORD attr = GetFileAttributesW(wpath.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
                SetCurrentDirectoryW(wpath.c_str());
                last_code = 0;
                continue;
            }
        }

        // show command name in title while running, folder name restored next iteration
        {
            std::string cmd_name = line.substr(0, line.find(' '));
            SetConsoleTitleA(cmd_name.c_str());
        }
        ctrl_c_fired = false;
        ULONGLONG t_start = GetTickCount64();
        bool bash_curl = lower.size() >= 5 && lower.substr(0, 5) == "curl " &&
                         line.find('\'') != std::string::npos;
        last_code = bash_curl ? run_bash(line) : run(line);
        ULONGLONG elapsed = GetTickCount64() - t_start;
        if (ctrl_c_fired) { out("\r\n"); last_code = 0; }
        if (!ctrl_c_fired && elapsed >= 2000) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.1fs", elapsed / 1000.0);
            out(std::string(GRAY) + "[" + buf + "]" + RESET + "\r\n");
        }
    }

    return 0;
}
