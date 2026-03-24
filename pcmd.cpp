#include <windows.h>
#include <string>
#include <algorithm>
#include <vector>
#include <cstdio>
#include <cwctype>
#include <fstream>
#include <sstream>
#include <unordered_set>

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
// Excluded in test builds — pcmd_test.cpp provides its own stub so tests run without a real console.
#ifndef PCMD_TEST
void out(const std::string& s) {
    DWORD w;
    WriteFile(out_h, s.c_str(), (DWORD)s.size(), &w, NULL);
}

// Write to stderr handle; used for error messages that should not mix with stdout output.
void err(const std::string& s) {
    DWORD w;
    WriteFile(err_h, s.c_str(), (DWORD)s.size(), &w, NULL);
}
#endif

// ---- ctrl+c handler ----

// Set to true by ctrl_handler on Ctrl+C; checked after child exits to suppress exit-code display and emit a newline.
static volatile bool ctrl_c_fired = false;

// forward declarations so ctrl_handler can save history on close/shutdown
struct editor;
void save_history(const editor&, size_t);
// Globals so ctrl_handler (which has no parameters) can reach the live editor state on unexpected exit.
static editor* g_editor  = nullptr;
static size_t  g_loaded  = 0;

// Ctrl+C/Break: set flag and suppress for our process (child still receives the signal).
// Close/Logoff/Shutdown: flush history to disk then let Windows terminate us.
BOOL WINAPI ctrl_handler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        ctrl_c_fired = true;
        return TRUE; // suppress for our process, child still gets it
    }
    if (type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT) {
        if (g_editor) save_history(*g_editor, g_loaded);
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
// Returns the branch name, a 7-char SHA for detached HEAD, or empty string if not in a git repo.
std::string branch() {
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

// Spawns "git status --porcelain" in a hidden window and reads one byte from its output.
// Returns true if any output exists (repo is dirty); kept fast by not reading the full output.
bool dirty() {
    HANDLE pipe_r = NULL, pipe_w = NULL;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    if (!CreatePipe(&pipe_r, &pipe_w, &sa, 0)) return false;
    SetHandleInformation(pipe_r, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdOutput = pipe_w;
    si.hStdError  = err_h;
    si.hStdInput  = in_h;

    PROCESS_INFORMATION pi = {};
    char cmd[] = "git.exe status --porcelain";
    bool ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE,
        CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(pipe_w);
    if (!ok) { CloseHandle(pipe_r); return false; }

    char buf[4] = {};
    DWORD n = 0;
    bool has = ReadFile(pipe_r, buf, 1, &n, NULL) && n > 0;
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(pipe_r);
    return has;
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
// For "cd <path>" uses filesystem completions (dirs only); for everything else scans history
// backwards so the most recent matching command wins. Clears hint if no match found.
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
        if (vk == VK_TAB) {
            if (!e.tab_on) {
                std::wstring before = e.buf.substr(0, e.pos);
                size_t space = before.find_last_of(L" \t");
                int start = (space == std::wstring::npos) ? 0 : (int)space + 1;
                std::wstring token = before.substr(start);
                std::wstring lower_buf = e.buf;
                std::transform(lower_buf.begin(), lower_buf.end(), lower_buf.begin(), ::towlower);
                bool dirs_only = (lower_buf.substr(0, 3) == L"cd " || lower_buf == L"cd");
                e.tab_matches = complete(token, dirs_only);
                if (e.tab_matches.empty()) continue;
                e.tab_on    = true;
                e.tab_idx   = 0;
                e.tab_start = start;
                e.tab_pre   = e.buf.substr(0, start);
                e.tab_suf   = e.buf.substr(e.pos);
            } else {
                e.tab_idx = (e.tab_idx + 1) % (int)e.tab_matches.size();
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

// Last directory before a successful cd; enables "cd -" to jump back.
static std::string prev_dir;

// Built-in cd: strips the /d compatibility flag, expands ~ to USERPROFILE, resolves "-" to
// prev_dir, then calls SetCurrentDirectory. Updates prev_dir only on success.
void cd(const std::string& line) {
    std::string args = line.size() > 2 ? line.substr(3) : "";
    // strip /d flag
    if (args.size() >= 3 && args[0] == '/' && (args[1] == 'd' || args[1] == 'D') && args[2] == ' ')
        args = args.substr(3);
    while (!args.empty() && args.front() == ' ') args.erase(args.begin());
    while (!args.empty() && args.back()  == ' ') args.pop_back();

    if (args.empty()) { out(cwd() + "\r\n"); return; }

    // cd - : jump to previous directory
    if (args == "-") {
        if (prev_dir.empty()) { err("No previous directory.\r\n"); return; }
        args = prev_dir;
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

// Returns the path to the persistent history file: %USERPROFILE%\.history.
std::string history_path() {
    wchar_t buf[MAX_PATH];
    GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
    return to_utf8(buf) + "\\.history";
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

// Appends only the commands added this session (index >= loaded) to avoid rewriting
// the whole file; the file grows append-only and is deduplicated on next load.
void save_history(const editor& e, size_t loaded) {
    std::ofstream f(history_path(), std::ios::app);
    for (size_t i = loaded; i < e.hist.size(); i++)
        f << to_utf8(e.hist[i]) << "\n";
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

#ifdef DEMO
#include "pcmd_demo.cpp"
#endif

// Searches for arg as a built-in or executable in PATH; prints result and returns exit code (0=found, 1=not found).
int which(const std::string& arg) {
    std::string argl = arg;
    std::transform(argl.begin(), argl.end(), argl.begin(), ::tolower);
    static const std::vector<std::string> builtins = {"ls","cd","pwd","exit","which","help"
#ifdef DEMO
        ,"demo"
#endif
    };
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

// ---- main ----

#ifndef PCMD_TEST
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
    size_t loaded = e.hist.size();
    g_editor = &e;
    g_loaded = loaded;

    int last_code = 0;
    while (true) {
        std::string dir  = cwd();
        std::string name = folder(dir);
        std::string t    = cur_time();
        std::string b    = branch();
        bool d           = b.empty() ? false : dirty();

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

        if (lower == "exit") { save_history(e, loaded); break; }

#ifdef DEMO
        if (lower == "demo") { demo_run(e); last_code = 0; continue; }
#endif

        if (lower == "help") {
            out(
                GREEN "pwd" RESET "    Print current directory\r\n"
                GREEN "ls" RESET "     Colored listing  -a all  -s by size  -t by time  -l long  -r reverse  | grep <word>\r\n"
                GREEN "cd" RESET "     Change directory  ~ home  - prev dir\r\n"
                GREEN "which" RESET "  Locate a command in PATH or identify built-ins\r\n"
                GREEN "help" RESET "   Show this help\r\n"
                GREEN "exit" RESET "   Exit pcmd\r\n"
                GRAY "All other commands are passed to cmd.exe" RESET "\r\n"
            );
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
        last_code = run(line);
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
#endif // PCMD_TEST
