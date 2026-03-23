#include <windows.h>
#include <shlobj.h>
#include <string>
#include <algorithm>
#include <vector>
#include <cstdio>
#include <cwctype>
#include <fstream>

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
#define RESET  "\x1b[0m"

static HANDLE out_h;
static HANDLE err_h;
static HANDLE in_h;
static DWORD  orig_in_mode;

// ---- string helpers ----

std::string to_utf8(const std::wstring& ws) {
    if (ws.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, NULL, 0, NULL, NULL);
    std::string s(n - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, &s[0], n, NULL, NULL);
    return s;
}

std::wstring to_wide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, NULL, 0);
    std::wstring ws(n - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &ws[0], n);
    return ws;
}

void out(const std::string& s) {
    DWORD w;
    WriteFile(out_h, s.c_str(), (DWORD)s.size(), &w, NULL);
}

void err(const std::string& s) {
    DWORD w;
    WriteFile(err_h, s.c_str(), (DWORD)s.size(), &w, NULL);
}

// ---- ctrl+c handler ----

static volatile bool ctrl_c_fired = false;

// forward declarations so ctrl_handler can save history on close/shutdown
struct editor;
void save_history(const editor&, size_t);
static editor* g_editor  = nullptr;
static size_t  g_loaded  = 0;

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

std::string cur_time() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%02d",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds / 10);
    return buf;
}

std::string cwd() {
    wchar_t buf[MAX_PATH];
    GetCurrentDirectoryW(MAX_PATH, buf);
    return to_utf8(buf);
}

std::string folder(const std::string& path) {
    size_t pos = path.find_last_of("\\/");
    if (pos == std::string::npos) return path;
    std::string name = path.substr(pos + 1);
    return name.empty() ? path.substr(0, pos) : name;
}

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

struct prompt_t {
    std::string str; // full ANSI string
    int vis;         // visual character width
};

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
    if (code != 0) {
        std::string cs = std::to_string(code);
        s += RED "["; s += cs; s += "]";
        s += color;
    }
    s += color; s += "> ";
    s += RESET;

    int vis = 2 + (int)t.size() + (int)f.size() + 2;
    if (!b.empty()) vis += 1 + (int)b.size() + (d ? 1 : 0) + 1;
    if (code != 0) vis += 1 + (int)std::to_string(code).size() + 1;
    return { s, vis };
}

// ---- tab completion ----

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

struct editor {
    std::wstring buf;
    std::wstring full_cmd;    // accumulated command across ^ continuations
    int pos        = 0;
    int prev_pos   = 0;       // cursor pos after last redraw (for relative movement)
    int prompt_vis = 0;
    std::string prompt_str;
    std::vector<std::wstring> hist;
    int hist_idx  = -1;
    std::wstring saved;

    bool tab_on = false;
    std::vector<std::wstring> tab_matches;
    int tab_idx   = 0;
    int tab_start = 0;
    std::wstring tab_pre;
    std::wstring tab_suf;
};

int term_width() {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(out_h, &csbi))
        return csbi.srWindow.Right - csbi.srWindow.Left + 1;
    return 80;
}

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

std::string readline(editor& e) {
    while (true) {
        INPUT_RECORD ir;
        DWORD count;
        if (!ReadConsoleInputW(in_h, &ir, 1, &count)) break;
        if (ir.EventType != KEY_EVENT || !ir.Event.KeyEvent.bKeyDown) continue;

        WORD vk     = ir.Event.KeyEvent.wVirtualKeyCode;
        wchar_t ch  = ir.Event.KeyEvent.uChar.UnicodeChar;
        DWORD state = ir.Event.KeyEvent.dwControlKeyState;
        bool ctrl   = (state & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;

        if (vk != VK_TAB) e.tab_on = false;

        if (vk == VK_RETURN) {
            // cmd-style line continuation: ^ at end of line
            std::wstring trimmed = e.buf;
            while (!trimmed.empty() && trimmed.back() == L' ') trimmed.pop_back();
            if (!trimmed.empty() && (trimmed.back() == L'^' || trimmed.back() == L'\\')) {
                // commit this segment into full_cmd, start fresh visual line
                e.full_cmd += trimmed.substr(0, trimmed.size() - 1);
                e.buf.clear();
                e.pos        = 0;
                e.prev_pos   = 0;
                e.prompt_str = "> ";
                e.prompt_vis = 2;
                out("\r\n\x1b[2K> "); // \x1b[2K clears the line (erases any "More?" ConHost may have echoed)
                continue;
            }
            out("\r\n");
            std::wstring full = e.full_cmd + e.buf;
            std::string line = to_utf8(full);
            if (!full.empty() && (e.hist.empty() || e.hist.back() != full))
                e.hist.push_back(full);
            e.buf.clear();
            e.full_cmd.clear();
            e.pos      = 0;
            e.hist_idx = -1;
            return line;
        }

        if (vk == VK_BACK) {
            if (e.pos > 0) { e.buf.erase(e.pos - 1, 1); e.pos--; redraw(e); }
            continue;
        }

        if (vk == VK_DELETE) {
            if (e.pos < (int)e.buf.size()) { e.buf.erase(e.pos, 1); redraw(e); }
            continue;
        }

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
            } else if (e.pos < (int)e.buf.size()) e.pos++;
            redraw(e); continue;
        }
        if (vk == VK_HOME)  { e.pos = 0;                          redraw(e); continue; }
        if (vk == VK_END)   { e.pos = (int)e.buf.size();          redraw(e); continue; }

        if (vk == VK_UP) {
            if (e.hist.empty()) continue;
            if (e.hist_idx == -1) { e.saved = e.buf; e.hist_idx = (int)e.hist.size() - 1; }
            else if (e.hist_idx > 0) e.hist_idx--;
            e.buf = e.hist[e.hist_idx];
            e.pos = (int)e.buf.size();
            redraw(e);
            continue;
        }

        if (vk == VK_DOWN) {
            if (e.hist_idx == -1) continue;
            if (e.hist_idx < (int)e.hist.size() - 1) { e.hist_idx++; e.buf = e.hist[e.hist_idx]; }
            else { e.hist_idx = -1; e.buf = e.saved; }
            e.pos = (int)e.buf.size();
            redraw(e);
            continue;
        }

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
            redraw(e);
            continue;
        }

        if (vk == VK_ESCAPE) {
            e.buf.clear(); e.pos = 0; redraw(e);
            continue;
        }

        if (ctrl && vk == 'C') {
            out("^C\r\n");
            e.buf.clear();
            e.full_cmd.clear();
            e.pos = 0;
            return "";
        }

        if (ch >= 32 && ch != 127) {
            e.buf.insert(e.pos, 1, ch);
            e.pos++;
            redraw(e);
        }
    }
    return "";
}

// ---- commands ----

void cd(const std::string& line) {
    std::string args = line.size() > 2 ? line.substr(3) : "";
    // strip /d flag
    if (args.size() >= 3 && args[0] == '/' && (args[1] == 'd' || args[1] == 'D') && args[2] == ' ')
        args = args.substr(3);
    while (!args.empty() && args.front() == ' ') args.erase(args.begin());
    while (!args.empty() && args.back()  == ' ') args.pop_back();

    if (args.empty()) { out(cwd() + "\r\n"); return; }

    if (!SetCurrentDirectoryW(to_wide(args).c_str()))
        err("The system cannot find the path specified.\r\n");
}

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
        return "\x1b[38;5;114m";

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

void do_ls(const std::string& arg) {
    std::string path_s = arg;
    while (!path_s.empty() && path_s.front() == ' ') path_s.erase(path_s.begin());
    // skip flags (e.g. -la)
    if (!path_s.empty() && path_s[0] == '-') {
        size_t sp = path_s.find(' ');
        path_s = sp == std::string::npos ? "" : path_s.substr(sp + 1);
        while (!path_s.empty() && path_s.front() == ' ') path_s.erase(path_s.begin());
    }
    // strip surrounding quotes
    if (path_s.size() >= 2 && path_s.front() == '"' && path_s.back() == '"')
        path_s = path_s.substr(1, path_s.size() - 2);

    std::wstring wpath = path_s.empty() ? L"." : to_wide(path_s);

    struct entry { std::wstring name; bool is_dir; std::string color; };
    std::vector<entry> dirs, files;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((wpath + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        out("ls: cannot access '" + path_s + "'\r\n");
        return;
    }
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        bool is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        entry e = { name, is_dir, ls_color(fd) };
        (is_dir ? dirs : files).push_back(e);
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    auto cmp = [](const entry& a, const entry& b) {
        std::wstring la = a.name, lb = b.name;
        std::transform(la.begin(), la.end(), la.begin(), ::towlower);
        std::transform(lb.begin(), lb.end(), lb.begin(), ::towlower);
        return la < lb;
    };
    std::sort(dirs.begin(), dirs.end(), cmp);
    std::sort(files.begin(), files.end(), cmp);

    std::vector<entry> all;
    all.insert(all.end(), dirs.begin(), dirs.end());
    all.insert(all.end(), files.begin(), files.end());
    if (all.empty()) return;

    // max display width (name + trailing / for dirs)
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
}

std::string history_path() {
    wchar_t buf[MAX_PATH];
    GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
    return to_utf8(buf) + "\\.history";
}

void load_history(editor& e) {
    std::ifstream f(history_path());
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty()) e.hist.push_back(to_wide(line));
    }
}

void save_history(const editor& e, size_t loaded) {
    std::ofstream f(history_path(), std::ios::app);
    for (size_t i = loaded; i < e.hist.size(); i++)
        f << to_utf8(e.hist[i]) << "\n";
}

int run(const std::string& line) {
    std::string cmd = "cmd.exe /c " + line;
    std::vector<char> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    STARTUPINFOA si = {}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(NULL, buf.data(), NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi))
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

    out(GRAY "Power CMD v" VERSION RESET "\r\n");

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

        if (lower == "ls" || (lower.size() >= 3 && lower.substr(0, 3) == "ls ")) {
            do_ls(line.size() >= 3 ? line.substr(3) : "");
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
        last_code = run(line);
        if (ctrl_c_fired) out("\r\n");
    }

    return 0;
}
