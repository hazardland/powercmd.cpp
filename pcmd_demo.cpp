// Demo mode — compiled in only when built with -DDEMO (e.g. "build demo").
// All functions here are named demo_* so they can be removed without affecting anything else.
// This file is #included at the end of pcmd.cpp, so it has full access to all internals.

// Pause between demo actions to give the viewer time to read.
static void demo_pause(int ms) { Sleep(ms); }

// Print text character-by-character with a delay to simulate a human typing.
static void demo_type(const std::string& text, int delay_ms = 70) {
    for (char c : text) { out(std::string(1, c)); Sleep(delay_ms); }
}

// Print gray ghost hint after cursor (save/restore), then accept or erase.
static void demo_hint(const std::string& h, int hold_ms, bool accept) {
    out("\x1b[s" GRAY + h + RESET);
    Sleep(hold_ms);
    out("\x1b[u");
    if (accept) out(h);
    else { out(std::string(h.size(), ' ')); out("\x1b[u"); }
}

// Overwrite the current input line (prompt + buf) — used to simulate Tab / UP.
static void demo_overwrite(const std::string& prompt_str, const std::string& buf,
                            const std::string& hint_sfx = "") {
    out("\r\x1b[K" + prompt_str + buf);
    if (!hint_sfx.empty()) out(GRAY + hint_sfx + RESET "\x1b[u");
}

// Create an empty file at the given wide path.
static void demo_touch(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

// Build the demo folder tree under root and return true on success.
// Creates directories first, then empty placeholder files.
static bool demo_setup(const std::string& root) {
    auto mk  = [](const std::string& p) { return CreateDirectoryW(to_wide(p).c_str(), NULL) != 0; };
    auto tch = [](const std::string& p) { demo_touch(to_wide(p)); };

    if (!mk(root))                    return false;
    mk(root + "\\src");
    mk(root + "\\assets");
    mk(root + "\\assets\\images");
    mk(root + "\\assets\\audio");
    mk(root + "\\assets\\video");
    mk(root + "\\dist");
    mk(root + "\\archive");
    mk(root + "\\docs");

    tch(root + "\\build.bat");
    tch(root + "\\config.json");
    tch(root + "\\readme.md");
    tch(root + "\\src\\main.cpp");
    tch(root + "\\src\\utils.cpp");
    tch(root + "\\src\\parser.h");
    tch(root + "\\assets\\images\\logo.png");
    tch(root + "\\assets\\images\\banner.jpg");
    tch(root + "\\assets\\images\\icon.ico");
    tch(root + "\\assets\\images\\preview.webp");
    tch(root + "\\assets\\audio\\theme.mp3");
    tch(root + "\\assets\\audio\\intro.wav");
    tch(root + "\\assets\\video\\demo.mp4");
    tch(root + "\\assets\\video\\tutorial.mkv");
    tch(root + "\\dist\\app.exe");
    tch(root + "\\dist\\installer.msi");
    tch(root + "\\dist\\setup.bat");
    tch(root + "\\dist\\run.cmd");
    tch(root + "\\archive\\backup.zip");
    tch(root + "\\archive\\release.tar.gz");
    tch(root + "\\archive\\source.7z");
    tch(root + "\\archive\\old.rar");
    tch(root + "\\docs\\readme.md");
    tch(root + "\\docs\\changelog.txt");
    return true;
}

// Delete all demo files and directories created by demo_setup.
// Files must be removed before their parent directories.
static void demo_teardown(const std::string& root) {
    auto rm  = [](const std::string& p) { DeleteFileW(to_wide(p).c_str()); };
    auto rmd = [](const std::string& p) { RemoveDirectoryW(to_wide(p).c_str()); };

    rm(root + "\\build.bat");
    rm(root + "\\config.json");
    rm(root + "\\readme.md");
    rm(root + "\\src\\main.cpp");
    rm(root + "\\src\\utils.cpp");
    rm(root + "\\src\\parser.h");
    rm(root + "\\assets\\images\\logo.png");
    rm(root + "\\assets\\images\\banner.jpg");
    rm(root + "\\assets\\images\\icon.ico");
    rm(root + "\\assets\\images\\preview.webp");
    rm(root + "\\assets\\audio\\theme.mp3");
    rm(root + "\\assets\\audio\\intro.wav");
    rm(root + "\\assets\\video\\demo.mp4");
    rm(root + "\\assets\\video\\tutorial.mkv");
    rm(root + "\\dist\\app.exe");
    rm(root + "\\dist\\installer.msi");
    rm(root + "\\dist\\setup.bat");
    rm(root + "\\dist\\run.cmd");
    rm(root + "\\archive\\backup.zip");
    rm(root + "\\archive\\release.tar.gz");
    rm(root + "\\archive\\source.7z");
    rm(root + "\\archive\\old.rar");
    rm(root + "\\docs\\readme.md");
    rm(root + "\\docs\\changelog.txt");

    rmd(root + "\\src");
    rmd(root + "\\assets\\images");
    rmd(root + "\\assets\\audio");
    rmd(root + "\\assets\\video");
    rmd(root + "\\assets");
    rmd(root + "\\dist");
    rmd(root + "\\archive");
    rmd(root + "\\docs");
    rmd(root);
}

// Main demo entry point: seeds history, creates a temporary folder tree in %TEMP%,
// navigates it for scripted scenes, then deletes the tree on exit.
// Tab and UP key effects are visually simulated via demo_overwrite.
void demo_run(editor& e) {
    // Seed history for reliable hint / UP-nav demonstrations
    for (const wchar_t* s : {L"ping 8.8.8.8", L"ping github.com", L"ping google.com"}) {
        std::wstring ws(s);
        e.hist.erase(std::remove(e.hist.begin(), e.hist.end(), ws), e.hist.end());
        e.hist.push_back(ws);
    }

    // Build demo root in %TEMP% so no project folders are touched
    char tmp[MAX_PATH];
    GetEnvironmentVariableA("TEMP", tmp, sizeof(tmp));
    const std::string demo_root = std::string(tmp) + "\\pcmd-demo";

    if (!demo_setup(demo_root)) {
        out(RED "demo: failed to create temp folder " + demo_root + RESET "\r\n");
        return;
    }

    std::string start_dir = cwd();
    bool elev = elevated();

    // Helper: print a fresh prompt reflecting current state
    // Branch is computed once (we stay in the same repo the whole demo)
    std::string br = branch();
    auto ppt = [&](int exit_code = 0) -> std::string {
        auto p = make_prompt(elev, cur_time(), folder(cwd()), br, false, exit_code);
        out(p.str);
        return p.str; // caller saves for demo_overwrite
    };

    // ── Scene 1: cd with parent-dir hinting ───────────────────────────────
    // Navigate into pcmd-demo/src so we can show "cd ../" parent hints
    SetCurrentDirectoryW(to_wide(demo_root + "\\src").c_str());

    out("\r\n"); auto p1 = ppt();
    demo_type("cd ../");
    demo_pause(350);
    // Hint: first alphabetical sibling dir = "archive/"
    out("\x1b[s");  // save cursor (after "cd ../")
    demo_hint("archive/", 900, false);  // show, then erase — we'll Tab through instead
    demo_pause(200);

    // Simulate Tab cycling through parent dirs
    demo_overwrite(p1, "cd ../archive/"); demo_pause(700);
    demo_overwrite(p1, "cd ../assets/");  demo_pause(700);
    demo_overwrite(p1, "cd ../dist/");    demo_pause(700);
    demo_overwrite(p1, "cd ../docs/");    demo_pause(700);
    // Accept docs — navigate there
    out("\r\n");
    SetCurrentDirectoryW(to_wide(demo_root + "\\docs").c_str());
    demo_pause(500);

    // Show we're in docs, cd back to src with cd -
    out("\r\n"); auto p2 = ppt();
    demo_type("cd -"); demo_pause(450);
    out("\r\n");
    SetCurrentDirectoryW(to_wide(demo_root + "\\src").c_str());
    demo_pause(400);

    // Show narrowing: typing "cd ../a" hints "archive/"
    out("\r\n"); auto p3 = ppt();
    demo_type("cd ../", 80); demo_pause(300);
    demo_type("a", 120);     demo_pause(300);
    demo_hint("rchive/", 1000, true);  // accept
    demo_pause(300); out("\r\n");
    SetCurrentDirectoryW(to_wide(demo_root + "\\archive").c_str());
    demo_pause(600);

    // ── Scene 2: history hinting and UP navigation ─────────────────────────
    SetCurrentDirectoryW(to_wide(demo_root).c_str());

    // Type "ping g" → hint "oogle.com"
    out("\r\n"); auto p4 = ppt();
    demo_type("ping g", 80); demo_pause(350);
    out("\x1b[s");  // save cursor position for UP sim
    demo_hint("oogle.com", 800, false);  // show hint, then erase for UP sim

    // Simulate UP: filtered nav, "ping g" stays in buf, hint cycles
    demo_overwrite(p4, "ping g", "ithub.com");  demo_pause(900);
    demo_overwrite(p4, "ping g", "oogle.com");  demo_pause(900);
    // Accept "ping google.com" via right-arrow (overwrite without gray)
    demo_overwrite(p4, "ping google.com"); demo_pause(400);
    out("\r\n");
    {
        ULONGLONG t0 = GetTickCount64();
        run("ping -n 3 google.com");
        ULONGLONG elapsed = GetTickCount64() - t0;
        if (elapsed >= 2000) {
            char tbuf[32];
            snprintf(tbuf, sizeof(tbuf), "%.1fs", elapsed / 1000.0);
            out(std::string(GRAY) + "[" + tbuf + "]" + RESET + "\r\n");
        }
    }
    demo_pause(1200);

    // ── Scene 3: ls with colors ────────────────────────────────────────────
    out("\r\n"); ppt();
    demo_type("ls"); demo_pause(450); out("\r\n");
    ls("");
    demo_pause(2000);

    // ls inside images subfolder to show only images (magenta)
    out("\r\n"); ppt();
    demo_type("ls assets/images"); demo_pause(450); out("\r\n");
    ls("assets/images");
    demo_pause(1800);

    // ── Scene 4: which ─────────────────────────────────────────────────────
    out("\r\n"); ppt();
    demo_type("which ls"); demo_pause(450); out("\r\n");
    out("ls: pcmd built-in\r\n");
    demo_pause(900);

    out("\r\n"); ppt();
    demo_type("which git"); demo_pause(450); out("\r\n");
    {
        char path_env[32768] = {};
        GetEnvironmentVariableA("PATH", path_env, sizeof(path_env));
        std::stringstream ps(path_env);
        std::string d; bool found = false;
        while (std::getline(ps, d, ';') && !found) {
            if (!d.empty() && d.back() != '\\') d += '\\';
            std::string full = d + "git.exe";
            if (GetFileAttributesA(full.c_str()) != INVALID_FILE_ATTRIBUTES) {
                std::replace(full.begin(), full.end(), '\\', '/');
                out(full + "\r\n"); found = true;
            }
        }
        if (!found) out("git: not found\r\n");
    }
    demo_pause(1200);

    // ── Scene 5: pwd ───────────────────────────────────────────────────────
    out("\r\n"); ppt();
    demo_type("pwd"); demo_pause(450); out("\r\n");
    out(cwd() + "\r\n");
    demo_pause(1000);

    // ── Scene 6: help ──────────────────────────────────────────────────────
    out("\r\n"); ppt();
    demo_type("help"); demo_pause(450); out("\r\n");
    out(GREEN "pwd" RESET "    Print current directory\r\n"
        GREEN "ls" RESET "     Colored directory listing\r\n"
        GREEN "cd" RESET "     Change directory  ~ home  - prev dir\r\n"
        GREEN "which" RESET "  Locate a command in PATH or identify built-ins\r\n"
        GREEN "help" RESET "   Show this help\r\n"
        GREEN "exit" RESET "   Exit pcmd\r\n"
        GRAY "All other commands are passed to cmd.exe" RESET "\r\n");
    demo_pause(2000);

    // ── Done ───────────────────────────────────────────────────────────────
    out(GRAY "\r\n── demo complete ──" RESET "\r\n\r\n");
    SetCurrentDirectoryW(to_wide(start_dir).c_str());
    demo_teardown(demo_root);
}
