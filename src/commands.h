// MODULE: commands
// Purpose : built-in shell commands — cd, ls, run/run_bash process spawner, which, separator rule
// Exports : cd() ls() run() run_bash() which() rule()
// Depends : common.h, info.h (cwd), complete.h, persist.h (prev_dir/last_session_dir)

// Built-in cd: strips the /d flag, expands ~ to USERPROFILE, resolves - and -- shortcuts.
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

    std::string before = cwd();
    if (!SetCurrentDirectoryW(to_wide(args).c_str()))
        err("The system cannot find the path specified.\r\n");
    else
        prev_dir = before;
}

// Maps a directory entry to its ANSI color escape.
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

    if (ext == L".exe" || ext == L".bat" || ext == L".cmd" ||
        ext == L".ps1" || ext == L".msi")
        return GREEN;

    if (ext == L".zip" || ext == L".tar" || ext == L".gz"  || ext == L".tgz" ||
        ext == L".bz2" || ext == L".xz"  || ext == L".7z"  || ext == L".rar" ||
        ext == L".z"   || ext == L".lz"  || ext == L".lzma"|| ext == L".zst" ||
        ext == L".deb" || ext == L".rpm" || ext == L".cab" || ext == L".iso")
        return "\x1b[1;31m";

    if (ext == L".jpg"  || ext == L".jpeg" || ext == L".png"  || ext == L".gif"  ||
        ext == L".bmp"  || ext == L".tif"  || ext == L".tiff" || ext == L".svg"  ||
        ext == L".webp" || ext == L".ico"  || ext == L".raw"  || ext == L".heic")
        return "\x1b[1;35m";

    if (ext == L".mp3"  || ext == L".wav"  || ext == L".ogg"  || ext == L".flac" ||
        ext == L".aac"  || ext == L".m4a"  || ext == L".wma"  ||
        ext == L".mp4"  || ext == L".mkv"  || ext == L".avi"  || ext == L".mov"  ||
        ext == L".wmv"  || ext == L".flv"  || ext == L".webm" || ext == L".m4v")
        return "\x1b[36m";

    return "";
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

// Prints a full-width gray horizontal rule; used to frame curl output.
static std::string rule() {
    std::string s = GRAY;
    int w = term_width();
    for (int i = 0; i < w; i++) s += "\xe2\x94\x80";
    s += "\r\n";
    s += RESET;
    return s;
}

// Spawns "cmd.exe /c <line>" and waits for it to finish. Returns the child's exit code.
int run(const std::string& line) {
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
    static const std::vector<std::string> builtins = {"ls","cd","pwd","cat","exit","which","help","version","alias","unalias"};
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
