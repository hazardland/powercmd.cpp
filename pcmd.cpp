#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// Unity build: each src/ header is a self-contained module included in dependency order.
// Include order matters — later modules may call functions defined in earlier ones.
#include "src/common.h"   // includes, color macros, globals, out/err, to_utf8/to_wide
#include "src/terminal.h" // term_width() term_height()
#include "src/signal.h"   // ctrl_c_fired, g_input, ctrl_handler, fwd-decls for persist
#include "src/info.h"     // elevated, cur_time, cwd, folder, branch, dirty
#include "src/prompt.h"   // prompt_t, make_prompt
#include "src/complete.h" // complete() — tab completion
#include "src/input.h"    // struct input, find_hint, redraw, readline (calls append_history via fwd-decl)
#include "src/persist.h"  // history, aliases, prev_dir (defines append_history used above)
#include "src/commands.h" // cd, ls, run, run_bash, which, rule
#include "src/image.h"    // cat_image, imgpush_cell (shared with video.h)
#include "src/video.h"    // cat_video (uses imgpush_cell from image.h)
#include "src/highlight.h" // detect_lang, colorize_inline, colorize_line
#include "src/cat.h"      // cat
#include "src/edit.h"     // edit_file()
#include "src/matrix.h"  // matrix()
#include "src/note.h"    // note_cmd()
#include "src/ip.h"      // ip_cmd()
#include "src/calc.h"    // calc()
#include "src/clock.h"   // clock_cmd()
#include "src/timer.h"   // timer_cmd()
#include "src/json.h"    // json_fmt()
#include "src/top.h"     // top_cmd()

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
    SetConsoleMode(in_h, ENABLE_EXTENDED_FLAGS | ENABLE_QUICK_EDIT_MODE | ENABLE_WINDOW_INPUT);

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
        RESET GRAY "PowerCmd v" VERSION RESET "\r\n"
    );
    // Print Windows build info
    {
        HKEY hk;
        char name[128]="Windows", build[32]="";
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",0,KEY_READ,&hk)==ERROR_SUCCESS) {
            DWORD sz=sizeof(name);
            RegQueryValueExA(hk,"ProductName",nullptr,nullptr,(LPBYTE)name,&sz);
            sz=sizeof(build);
            RegQueryValueExA(hk,"CurrentBuildNumber",nullptr,nullptr,(LPBYTE)build,&sz);
            RegCloseKey(hk);
        }
        out(std::string(GRAY)+name+" Build "+build+RESET+"\r\n");
    }

    bool elev = elevated();
    input e;
    load_history(e);
    load_prev_dir();
    load_aliases();
    g_input = &e;

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
        e.cursor_row  = 0;
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
                std::string namel = rest;
                std::transform(namel.begin(), namel.end(), namel.begin(), ::tolower);
                auto it = aliases.find(namel);
                if (it == aliases.end()) err("alias: " + rest + ": not found\r\n");
                else out(GREEN + it->first + RESET "=" + it->second + "\r\n");
            } else {
                std::string name = rest.substr(0, eq);
                std::string val  = rest.substr(eq + 1);
                std::transform(name.begin(), name.end(), name.begin(), ::tolower);
                if (val.empty()) { write_alias(name, ""); }
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
                GREEN "edit" RESET "    Edit a file  edit path/to/file\r\n"
                GREEN "terminfo" RESET " Print terminal columns and rows\r\n"
                GREEN "matrix" RESET "   Matrix digital rain screensaver  any key to exit\r\n"
                GREEN "notes" RESET "    Open ~/notes.md in the editor\r\n"
                GREEN "ip" RESET "       Show local IPv4 addresses\r\n"
                GREEN "top" RESET "      Interactive process viewer  [↑↓] navigate  [m/c] sort  [k] kill  [q] quit\r\n"
                GREEN "clock" RESET "    Live fullscreen clock  any key to exit\r\n"
                GREEN "stopw" RESET "    Stopwatch counting up  any key stops and prints result\r\n"
                GREEN "calc" RESET "     Evaluate arithmetic  calc (2+3)*4^2\r\n"
                "        Alias: = (2+3)*4^2\r\n"
                GREEN "clip" RESET "     Copy file to clipboard  clip path/to/file\r\n"
                GREEN "json" RESET "     Pretty-print JSON  json path/to/file\r\n"
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

        if (lower == "matrix") {
            matrix();
            last_code = 0;
            continue;
        }

        if (lower == "ip") {
            last_code = ip_cmd();
            continue;
        }

        if (lower == "top") {
            top_cmd();
            last_code = 0;
            continue;
        }

        if (lower == "notes") {
            last_code = note_cmd();
            continue;
        }

        if (lower == "clock") {
            clock_cmd();
            last_code = 0;
            continue;
        }

        if (lower == "stopw") {
            timer_cmd();
            last_code = 0;
            continue;
        }

        if (lower.size() >= 5 && lower.substr(0, 5) == "calc ") {
            last_code = calc(line.substr(5));
            continue;
        }

        if (lower.size() >= 2 && lower.substr(0, 2) == "= ") {
            last_code = calc(line.substr(2));
            continue;
        }

        if (lower.size() >= 5 && lower.substr(0, 5) == "clip ") {
            std::string path = line.substr(5);
            while (!path.empty() && path.front() == ' ') path.erase(path.begin());
            while (!path.empty() && path.back()  == ' ') path.pop_back();
            std::ifstream f(path);
            if (!f) { err("Cannot open: " + path + "\r\n"); last_code = 1; continue; }
            std::string content((std::istreambuf_iterator<char>(f)), {});
            clipboard_set(to_wide(content));
            out("Copied to clipboard.\r\n");
            last_code = 0;
            continue;
        }

        if (lower.size() >= 5 && lower.substr(0, 5) == "json ") {
            std::string path = line.substr(5);
            while (!path.empty() && path.front() == ' ') path.erase(path.begin());
            while (!path.empty() && path.back()  == ' ') path.pop_back();
            last_code = json_fmt(path);
            continue;
        }

        if (lower == "terminfo") {
            char buf[64];
            snprintf(buf, sizeof(buf), "Columns: %d\r\nRows   : %d\r\n", term_width(), term_height());
            out(buf);
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

        if (lower.size() >= 5 && lower.substr(0, 5) == "edit ") {
            std::string arg = line.substr(5);
            while (!arg.empty() && arg.front() == ' ') arg.erase(arg.begin());
            while (!arg.empty() && arg.back()  == ' ') arg.pop_back();
            last_code = edit_file(arg);
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

        {
            std::string cmd_name = line.substr(0, line.find(' '));
            SetConsoleTitleA(cmd_name.c_str());
        }
        ctrl_c_fired = false;
        ULONGLONG t_start = GetTickCount64();
        bool is_curl   = lower.size() >= 5 && lower.substr(0, 5) == "curl ";
        bool bash_curl = is_curl && line.find('\'') != std::string::npos;
        if (is_curl) out(rule());
        last_code = bash_curl ? run_bash(line) : run(line);
        if (is_curl) {
            CONSOLE_SCREEN_BUFFER_INFO csbi;
            if (GetConsoleScreenBufferInfo(out_h, &csbi) && csbi.dwCursorPosition.X > 0)
                out("\r\n");
            out(rule());
        }
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
