// MODULE: commands
// Purpose : built-in shell commands — cd, ls, run/run_bash process spawner, which, separator rule
// Exports : cd() ls() run() run_bash() which() rule()
// Depends : common.h, info.h (cwd), complete.h, persist.h (prev_dir/last_session_dir)

#include <shellapi.h>

// Built-in cd: strips the /d flag, expands ~ to USERPROFILE, resolves - and -- shortcuts.
static bool cd_is_drive_root(const std::string& path) {
    return path.size() == 2 && std::isalpha((unsigned char)path[0]) && path[1] == ':';
}

void cd(const std::string& line) {
    std::string args = line.size() > 2 ? line.substr(3) : "";
    if (args.size() >= 3 && args[0] == '/' && (args[1] == 'd' || args[1] == 'D') && args[2] == ' ')
        args = args.substr(3);
    while (!args.empty() && args.front() == ' ') args.erase(args.begin());
    while (!args.empty() && args.back()  == ' ') args.pop_back();

    if (args.empty()) { out(cwd() + "\r\n"); return; }

    if (args == "-") {
        if (prev_dir.empty()) { err("No previous directory.\r\n"); return; }
        args = prev_dir;
    }
    else if (args == "--") {
        if (last_session_dir.empty()) { err("No previous session directory.\r\n"); return; }
        args = last_session_dir;
    }
    else if (args == "~~") {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        std::wstring ws(exe);
        size_t slash = ws.find_last_of(L"\\/");
        args = to_utf8(slash != std::wstring::npos ? ws.substr(0, slash) : ws);
    }

    if (args[0] == '~') {
        char home[MAX_PATH] = {};
        GetEnvironmentVariableA("USERPROFILE", home, MAX_PATH);
        args = std::string(home) + args.substr(1);
    }

    if (cd_is_drive_root(args))
        args += '/';

    std::string before = cwd();
    if (!SetCurrentDirectoryW(to_wide(args).c_str()))
        err("The system cannot find the path specified.\r\n");
    else
        prev_dir = before;
}

static const char* LS_COLOR_FILE = "\x1b[38;5;248m";
static const char* LS_COLOR_SIZE = "\x1b[38;5;245m";

// Maps a directory entry to its ANSI color escape.
static std::string ls_color(const WIN32_FIND_DATAW& fd) {
    std::wstring name = fd.cFileName;
    bool is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    bool is_hidden = (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0 ||
        (!name.empty() && name[0] == L'.');

    switch (entry_color_kind(name, is_dir, is_hidden)) {
    case ENTRY_COLOR_HIDDEN:  return GRAY;
    case ENTRY_COLOR_DIR:     return BLUE;
    case ENTRY_COLOR_EXE:     return GREEN;
    case ENTRY_COLOR_ARCHIVE: return ARCHIVE_RED;
    case ENTRY_COLOR_IMAGE:   return MAGENTA;
    case ENTRY_COLOR_MEDIA:   return "\x1b[38;5;51m";
    default:                  return LS_COLOR_FILE;
    }
}

// Lists a directory with ANSI colors. Flags: -a all -s sort/size -t sort/time -l long -r reverse.
void ls(const std::string& arg, const std::string& filter = "") {
    bool show_all = false, show_size = false, show_time = false, reverse = false;
    char sort_by = 0;
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

    if (!show_size && !show_time) {
        int max_w = 0;
        for (auto& e : all) {
            std::wstring disp_w = e.name + (e.is_dir ? L"/" : L"");
            int w = ui_text_width(disp_w);
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
                std::wstring disp_w = e.name + (e.is_dir ? L"/" : L"");
                std::string disp = to_utf8(disp_w);
                int pad = col_w - ui_text_width(disp_w);
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

    auto fmt_size = [](ULONGLONG b) -> std::string {
        if (b < 1024ULL)               return std::to_string(b);
        if (b < 1024ULL * 1024)        { char s[16]; snprintf(s, sizeof(s), "%.1fK", b / 1024.0);              return s; }
        if (b < 1024ULL * 1024 * 1024) { char s[16]; snprintf(s, sizeof(s), "%.1fM", b / (1024.0*1024));       return s; }
                                         { char s[16]; snprintf(s, sizeof(s), "%.1fG", b / (1024.0*1024*1024)); return s; }
    };

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
        std::wstring disp_w = e.name + (e.is_dir ? L"/" : L"");
        int w = ui_text_width(disp_w);
        if (w > max_w) max_w = w;
    }

    for (auto& e : all) {
        std::wstring disp_w = e.name + (e.is_dir ? L"/" : L"");
        std::string name = to_utf8(disp_w);
        int vis_w = ui_text_width(disp_w);
        std::string row;
        if (!e.color.empty()) row += e.color;
        row += name;
        if (!e.color.empty()) row += RESET;
        row += std::string(max_w - vis_w + 2, ' ');
        if (show_size) {
            std::string sz = e.is_dir ? "" : fmt_size(e.size);
            std::string padded = std::string(std::max(0, 6 - (int)sz.size()), ' ') + sz;
            if (!sz.empty()) row += std::string(LS_COLOR_SIZE) + padded + RESET;
            else row += padded;
            row += "  ";
        }
        if (show_time) row += GRAY + fmt_time(e.mtime) + RESET;
        out(row + "\r\n");
    }
}

// Prints a full-width gray horizontal rule; used to frame curl output.
static std::string rule() {
    std::string s = GRAY;
    int w = term_width();
    for (int i = 0; i < w; i++) s += "\xe2\x94\x80";
    s += "\r\n";
    s += RESET;
    return s;
}

// Tools known to mutate the environment (PATH, runtime vars, etc.) via .cmd/.bat wrappers.
// For these, we capture the post-command environment and import it back into Zcmd.
static const std::unordered_set<std::string> env_mutators = {
    // Node version managers
    "nvs", "nvm", "fnm", "volta", "nodenv",
    // Python env managers
    "pyenv", "conda", "mamba", "micromamba", "uv", "pipenv", "poetry", "hatch", "pdm",
    // Universal version manager
    "asdf",
    // Ruby
    "rbenv", "rvm", "chruby",
    // Java
    "jenv", "jabba", "sdk",
    // Go
    "goenv", "gvm",
    // Visual Studio build environments
    "vcvarsall", "vcvars64", "vcvars32", "vcvarsarm64", "vsvars32",
};

static bool is_env_mutator(const std::string& line) {
    size_t sp = line.find(' ');
    std::string tok = sp == std::string::npos ? line : line.substr(0, sp);
    std::transform(tok.begin(), tok.end(), tok.begin(), ::tolower);
    size_t dot = tok.rfind('.');
    if (dot != std::string::npos) tok = tok.substr(0, dot);
    return env_mutators.count(tok) > 0;
}

static std::string shell_trim(std::string s) {
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

static bool shell_split_first_token(const std::string& line, std::string& token, std::string& rest) {
    std::string s = shell_trim(line);
    token.clear();
    rest.clear();
    if (s.empty())
        return false;

    if (s[0] == '"') {
        size_t end = s.find('"', 1);
        if (end == std::string::npos)
            return false;
        token = s.substr(1, end - 1);
        rest = shell_trim(s.substr(end + 1));
        return true;
    }

    size_t sp = s.find(' ');
    if (sp == std::string::npos) {
        token = s;
        return true;
    }
    token = s.substr(0, sp);
    rest = shell_trim(s.substr(sp + 1));
    return true;
}

static bool shell_has_meta(const std::string& text) {
    bool in_quotes = false;
    for (size_t i = 0; i < text.size(); i++) {
        char c = text[i];
        if (c == '"') {
            in_quotes = !in_quotes;
            continue;
        }
        if (in_quotes)
            continue;
        if (c == '|' || c == '<' || c == '>')
            return true;
        if ((c == '&' || c == '%') && (i == 0 || text[i - 1] != '^'))
            return true;
    }
    return false;
}

static std::wstring shell_ext_lower(const std::wstring& path) {
    size_t dot = path.rfind(L'.');
    if (dot == std::wstring::npos)
        return L"";
    std::wstring ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    return ext;
}

static bool shell_is_non_wait_open_ext(const std::wstring& ext) {
    return ext != L".exe" && ext != L".com" && ext != L".bat" && ext != L".cmd" &&
        ext != L".ps1" && ext != L".py" && ext != L".js" && ext != L".vbs" && ext != L".wsf";
}

static bool shell_resolve_existing_path(const std::string& token, std::wstring& path_out) {
    std::wstring path = to_wide(normalize_path(token));
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES)
        return false;

    wchar_t full[MAX_PATH] = {};
    DWORD len = GetFullPathNameW(path.c_str(), MAX_PATH, full, NULL);
    path_out = (len > 0 && len < MAX_PATH) ? std::wstring(full) : path;
    return true;
}

static bool shell_resolve_exe_token(const std::string& token, std::wstring& path_out) {
    std::wstring direct;
    if (shell_resolve_existing_path(token, direct)) {
        if (shell_ext_lower(direct) == L".exe") {
            path_out = direct;
            return true;
        }
        return false;
    }

    std::wstring wtoken = to_wide(token);
    wchar_t found[MAX_PATH] = {};
    DWORD len = SearchPathW(NULL, wtoken.c_str(), L".exe", MAX_PATH, found, NULL);
    if (len == 0 || len >= MAX_PATH)
        return false;
    path_out = found;
    return true;
}

static bool shell_is_gui_exe(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    IMAGE_DOS_HEADER dos = {};
    DWORD read = 0;
    bool ok = ReadFile(h, &dos, sizeof(dos), &read, NULL) != 0 && read == sizeof(dos) && dos.e_magic == IMAGE_DOS_SIGNATURE;
    if (!ok) {
        CloseHandle(h);
        return false;
    }

    if (SetFilePointer(h, dos.e_lfanew, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER && GetLastError() != NO_ERROR) {
        CloseHandle(h);
        return false;
    }

    DWORD pe_sig = 0;
    ok = ReadFile(h, &pe_sig, sizeof(pe_sig), &read, NULL) != 0 && read == sizeof(pe_sig) && pe_sig == IMAGE_NT_SIGNATURE;
    if (!ok) {
        CloseHandle(h);
        return false;
    }

    IMAGE_FILE_HEADER file = {};
    ok = ReadFile(h, &file, sizeof(file), &read, NULL) != 0 && read == sizeof(file);
    if (!ok) {
        CloseHandle(h);
        return false;
    }

    WORD magic = 0;
    ok = ReadFile(h, &magic, sizeof(magic), &read, NULL) != 0 && read == sizeof(magic);
    if (!ok) {
        CloseHandle(h);
        return false;
    }

    WORD subsystem = IMAGE_SUBSYSTEM_WINDOWS_CUI;
    LONG off = 0;
    if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC)
        off = offsetof(IMAGE_OPTIONAL_HEADER32, Subsystem) - sizeof(WORD);
    else if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        off = offsetof(IMAGE_OPTIONAL_HEADER64, Subsystem) - sizeof(WORD);
    else {
        CloseHandle(h);
        return false;
    }

    if (SetFilePointer(h, off, NULL, FILE_CURRENT) == INVALID_SET_FILE_POINTER && GetLastError() != NO_ERROR) {
        CloseHandle(h);
        return false;
    }
    ok = ReadFile(h, &subsystem, sizeof(subsystem), &read, NULL) != 0 && read == sizeof(subsystem);
    CloseHandle(h);
    return ok && subsystem == IMAGE_SUBSYSTEM_WINDOWS_GUI;
}

static bool shell_launch_file_open_detached(const std::wstring& path) {
    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"open";
    sei.lpFile = path.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei))
        return false;
    if (sei.hProcess)
        CloseHandle(sei.hProcess);
    return true;
}

static bool shell_launch_gui_exe_detached(const std::wstring& exe_path, const std::string& rest) {
    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"open";
    sei.lpFile = exe_path.c_str();
    std::wstring params = to_wide(rest);
    if (!params.empty())
        sei.lpParameters = params.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei))
        return false;
    if (sei.hProcess)
        CloseHandle(sei.hProcess);
    return true;
}

static bool shell_try_detached_launch(const std::string& line, int& code_out) {
    code_out = 1;

    std::wstring path;
    if (shell_resolve_existing_path(line, path)) {
        DWORD attr = GetFileAttributesW(path.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES &&
            (attr & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
            shell_is_non_wait_open_ext(shell_ext_lower(path))) {
            if (shell_launch_file_open_detached(path)) {
                code_out = 0;
                return true;
            }
        }
    }

    std::string token, rest;
    if (!shell_split_first_token(line, token, rest))
        return false;
    if (shell_has_meta(rest))
        return false;

    std::wstring exe_path;
    if (!shell_resolve_exe_token(token, exe_path))
        return false;
    if (!shell_is_gui_exe(exe_path))
        return false;

    if (!shell_launch_gui_exe_detached(exe_path, rest))
        return false;

    code_out = 0;
    return true;
}

// Runs an env-mutating command and imports the resulting environment into Zcmd's process.
// Uses cmd /v:on so !errorlevel! captures the runtime exit code of the user command.
static int run_capture_env(const std::string& line) {
    char tmp_dir[MAX_PATH], tmp_file[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, tmp_dir) || !GetTempFileNameA(tmp_dir, "ze", 0, tmp_file)) {
        // Temp file unavailable — fall through to normal run()
        std::wstring cmd = L"cmd.exe /c " + to_wide(line);
        std::vector<wchar_t> buf(cmd.begin(), cmd.end()); buf.push_back(0);
        STARTUPINFOW si = {}; si.cb = sizeof(si); PROCESS_INFORMATION pi = {};
        if (!CreateProcessW(NULL, buf.data(), NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) return -1;
        SetConsoleMode(in_h, orig_in_mode);
        WaitForSingleObject(pi.hProcess, INFINITE);
        SetConsoleMode(in_h, ENABLE_EXTENDED_FLAGS | ENABLE_QUICK_EDIT_MODE | ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT);
        DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        return (int)code;
    }

    // Build: cmd /v:on /c <line> & set _ZCMD_EL_=!errorlevel! & set > "tmpfile" & exit /b !_ZCMD_EL_!
    std::wstring tmp_w = to_wide(std::string(tmp_file));
    std::wstring cmd = L"cmd.exe /v:on /c " + to_wide(line)
        + L" & set _ZCMD_EL_=!errorlevel! & set > \""  + tmp_w + L"\" & exit /b !_ZCMD_EL_!";
    std::vector<wchar_t> buf(cmd.begin(), cmd.end()); buf.push_back(0);
    STARTUPINFOW si = {}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(NULL, buf.data(), NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        DeleteFileA(tmp_file);
        return -1;
    }
    SetConsoleMode(in_h, orig_in_mode);
    WaitForSingleObject(pi.hProcess, INFINITE);
    SetConsoleMode(in_h, ENABLE_EXTENDED_FLAGS | ENABLE_QUICK_EDIT_MODE | ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    // Import env changes into Zcmd's process
    std::ifstream f(tmp_file);
    if (f) {
        std::string env_line;
        while (std::getline(f, env_line)) {
            if (!env_line.empty() && env_line.back() == '\r') env_line.pop_back();
            size_t eq = env_line.find('=');
            if (eq == std::string::npos || eq == 0) continue;
            std::string name = env_line.substr(0, eq);
            std::string val  = env_line.substr(eq + 1);
            if (name[0] == '=') continue;        // internal cmd.exe drive-tracking vars
            if (name == "_ZCMD_EL_") continue;   // our own marker
            SetEnvironmentVariableA(name.c_str(), val.c_str());
        }
        f.close();
    }
    DeleteFileA(tmp_file);
    return (int)code;
}

// Spawns "cmd.exe /c <line>" and waits for it to finish. Returns the child's exit code.
int run(const std::string& line) {
    if (is_env_mutator(line)) return run_capture_env(line);
    std::wstring cmd = L"cmd.exe /c " + to_wide(line);
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    STARTUPINFOW si = {}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(NULL, buf.data(), NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi))
        return -1;
    SetConsoleMode(in_h, orig_in_mode);
    WaitForSingleObject(pi.hProcess, INFINITE);
    SetConsoleMode(in_h, ENABLE_EXTENDED_FLAGS | ENABLE_QUICK_EDIT_MODE | ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

// Runs line through sh.exe (Git-for-Windows); falls back to run() if sh.exe not found.
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
    SetConsoleMode(in_h, ENABLE_EXTENDED_FLAGS | ENABLE_QUICK_EDIT_MODE | ENABLE_WINDOW_INPUT);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

// Searches for arg as a built-in or executable in PATH; prints result and returns 0=found / 1=not found.
int which(const std::string& arg) {
    std::string argl = arg;
    std::transform(argl.begin(), argl.end(), argl.begin(), ::tolower);
    static const std::vector<std::string> builtins = {"ls","cd","pwd","cat","img","vid","edit","view","explore","exit","which","help","version","alias","unalias","play","resmon","yt"};
    for (auto& b : builtins) {
        if (argl == b) { out(arg + ": zcmd built-in\r\n"); return 0; }
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
