// MODULE: edit
// Purpose : full-screen terminal file editor
// Exports : edit_file()
// Depends : common.h, terminal.h, highlight.h

static const int GUTTER = 4; // "%3d " — 3-digit line number + space

struct file_buf {
    std::vector<std::string> lines;
    bool crlf        = false;
    bool trailing_nl = true;
    std::string path;
    bool modified    = false;

    int cur_row  = 0;
    int cur_col  = 0;
    int top_row  = 0;
    int left_col = 0;
    bool wrap    = false;

    int sel_row = -1;  // anchor row; -1 = no selection
    int sel_col = -1;  // anchor col
};

// ---- UTF-8 helpers ----

static int utf8_next(const std::string& s, int pos) {
    if (pos >= (int)s.size()) return pos;
    int n = pos + 1;
    while (n < (int)s.size() && (s[n] & 0xC0) == 0x80) n++;
    return n;
}

static int utf8_prev(const std::string& s, int pos) {
    if (pos <= 0) return 0;
    int p = pos - 1;
    while (p > 0 && (s[p] & 0xC0) == 0x80) p--;
    return p;
}

// ---- Load / save ----

static void load(file_buf& f) {
    std::ifstream ifs(f.path, std::ios::binary);
    if (!ifs) {
        f.lines.push_back("");
        f.crlf        = false;
        f.trailing_nl = true;
        return;
    }
    std::string data((std::istreambuf_iterator<char>(ifs)),
                      std::istreambuf_iterator<char>());
    f.crlf        = (data.find("\r\n") != std::string::npos);
    f.trailing_nl = !data.empty() && data.back() == '\n';
    data.erase(std::remove(data.begin(), data.end(), '\r'), data.end());
    std::istringstream ss(data);
    std::string line;
    while (std::getline(ss, line)) f.lines.push_back(line);
    if (f.lines.empty()) f.lines.push_back("");
}

static void save(file_buf& f) {
    std::ofstream ofs(f.path, std::ios::binary);
    if (!ofs) return;
    const std::string nl = f.crlf ? "\r\n" : "\n";
    for (int i = 0; i < (int)f.lines.size(); i++) {
        ofs.write(f.lines[i].c_str(), (std::streamsize)f.lines[i].size());
        if (i < (int)f.lines.size() - 1 || f.trailing_nl)
            ofs.write(nl.c_str(), (std::streamsize)nl.size());
    }
    f.modified = false;
}

// ---- Selection helpers ----

static bool in_selection(const file_buf& f, int row, int col) {
    if (f.sel_row < 0) return false;
    int ar = f.sel_row, ac = f.sel_col;
    int br = f.cur_row, bc = f.cur_col;
    if (ar > br || (ar == br && ac > bc)) { std::swap(ar, br); std::swap(ac, bc); }
    if (row < ar || row > br) return false;
    if (row == ar && col < ac) return false;
    if (row == br && col >= bc) return false;
    return true;
}

static void sel_begin(file_buf& f) {
    if (f.sel_row < 0) { f.sel_row = f.cur_row; f.sel_col = f.cur_col; }
}

static void sel_clear(file_buf& f) { f.sel_row = -1; f.sel_col = -1; }

static std::string get_selection(const file_buf& f) {
    if (f.sel_row < 0) return "";
    int ar = f.sel_row, ac = f.sel_col;
    int br = f.cur_row, bc = f.cur_col;
    if (ar > br || (ar == br && ac > bc)) { std::swap(ar, br); std::swap(ac, bc); }
    std::string result;
    for (int row = ar; row <= br; row++) {
        int cs = (row == ar) ? ac : 0;
        int ce = (row == br) ? bc : (int)f.lines[row].size();
        result += f.lines[row].substr(cs, ce - cs);
        if (row < br) result += "\n";
    }
    return result;
}

static void delete_selection(file_buf& f) {
    if (f.sel_row < 0) return;
    int ar = f.sel_row, ac = f.sel_col;
    int br = f.cur_row, bc = f.cur_col;
    if (ar > br || (ar == br && ac > bc)) { std::swap(ar, br); std::swap(ac, bc); }
    std::string merged = f.lines[ar].substr(0, ac) + f.lines[br].substr(bc);
    f.lines.erase(f.lines.begin() + ar + 1, f.lines.begin() + br + 1);
    f.lines[ar] = merged;
    f.cur_row   = ar;
    f.cur_col   = ac;
    sel_clear(f);
    f.modified = true;
}

// ---- Cursor clamping & scrolling ----

static void clamp_col(file_buf& f) {
    int len = (int)f.lines[f.cur_row].size();
    if (f.cur_col < 0)   f.cur_col = 0;
    if (f.cur_col > len) f.cur_col = len;
}

static void clamp_scroll(file_buf& f, int vis_rows, int vis_w) {
    if (f.cur_row < 0)                      f.cur_row = 0;
    if (f.cur_row >= (int)f.lines.size())   f.cur_row = (int)f.lines.size() - 1;
    clamp_col(f);
    if (f.cur_row < f.top_row)              f.top_row = f.cur_row;
    if (f.cur_row >= f.top_row + vis_rows)  f.top_row = f.cur_row - vis_rows + 1;
    if (f.top_row < 0) f.top_row = 0;
    if (!f.wrap) {
        if (f.cur_col < f.left_col)              f.left_col = f.cur_col;
        if (f.cur_col >= f.left_col + vis_w)     f.left_col = f.cur_col - vis_w + 1;
        if (f.left_col < 0) f.left_col = 0;
    }
}

// ---- Word movement ----

static void word_left(file_buf& f) {
    if (f.cur_col > 0) {
        const std::string& line = f.lines[f.cur_row];
        int c = f.cur_col;
        while (c > 0 && line[c - 1] == ' ') c--;
        while (c > 0 && line[c - 1] != ' ') c--;
        f.cur_col = c;
    } else if (f.cur_row > 0) {
        f.cur_row--;
        f.cur_col = (int)f.lines[f.cur_row].size();
    }
}

static void word_right(file_buf& f) {
    const std::string& line = f.lines[f.cur_row];
    if (f.cur_col < (int)line.size()) {
        int c = f.cur_col;
        while (c < (int)line.size() && line[c] != ' ') c++;
        while (c < (int)line.size() && line[c] == ' ') c++;
        f.cur_col = c;
    } else if (f.cur_row < (int)f.lines.size() - 1) {
        f.cur_row++;
        f.cur_col = 0;
    }
}

// ---- Screen row mapping ----

struct row_info { int file_row; int col_start; };

static std::vector<row_info> build_screen(const file_buf& f, int vis_rows, int vis_w) {
    std::vector<row_info> v;
    v.reserve(vis_rows);
    for (int fr = f.top_row; fr < (int)f.lines.size() && (int)v.size() < vis_rows; fr++) {
        if (f.wrap) {
            int len    = (int)f.lines[fr].size();
            int chunks = (len == 0) ? 1 : (len + vis_w - 1) / vis_w;
            for (int c = 0; c < chunks && (int)v.size() < vis_rows; c++)
                v.push_back({fr, c * vis_w});
        } else {
            v.push_back({fr, f.left_col});
        }
    }
    return v;
}

// ---- Rendering ----

// Clip an ANSI-colored string to the visible window [col_start, col_start+vis_w).
// Skips the first col_start visible chars but accumulates any ANSI escape sequences
// encountered along the way, re-emitting them at the start of the output so colors
// that began before the visible area carry through correctly.
static std::string clip_ansi(const std::string& s, int col_start, int vis_w) {
    std::string prefix;   // ANSI state accumulated before visible area
    std::string result;
    int col = 0;
    size_t i = 0, n = s.size();

    // helper: consume one ANSI escape sequence starting at i, return it, advance i
    auto take_esc = [&]() -> std::string {
        size_t j = i;
        while (j < n && s[j] != 'm') j++;
        if (j < n) j++;
        std::string e = s.substr(i, j - i);
        i = j;
        return e;
    };

    // helper: consume one UTF-8 codepoint starting at i, return its bytes, advance i
    auto take_char = [&]() -> std::string {
        std::string bytes;
        unsigned char c = (unsigned char)s[i];
        int len = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
        while (len-- && i < n) bytes += s[i++];
        return bytes;
    };

    // phase 1: skip col_start visible chars, collect ANSI state
    while (i < n && col < col_start) {
        if (s[i] == '\x1b' && i + 1 < n && s[i + 1] == '[') {
            prefix += take_esc();
        } else {
            take_char();
            col++;
        }
    }

    // phase 2: emit vis_w visible chars, prepend collected ANSI state
    if (!prefix.empty()) result += prefix;
    int emitted = 0;
    while (i < n && emitted < vis_w) {
        if (s[i] == '\x1b' && i + 1 < n && s[i + 1] == '[') {
            result += take_esc();
        } else {
            result += take_char();
            emitted++;
        }
    }

    return result;
}

// Overlay selection highlight (reverse-video) onto an already-clipped colored string.
// sel_start/sel_end are visual column offsets within the clipped string.
static std::string overlay_sel(const std::string& s, int sel_start, int sel_end) {
    if (sel_start >= sel_end) return s;
    std::string result;
    int col = 0;
    bool in_sel = false;
    size_t i = 0, n = s.size();

    while (i < n) {
        if (s[i] == '\x1b' && i + 1 < n && s[i + 1] == '[') {
            size_t j = i;
            while (j < n && s[j] != 'm') j++;
            if (j < n) j++;
            result += s.substr(i, j - i);
            i = j;
            continue;
        }
        if (!in_sel && col == sel_start) { result += "\x1b[7m";  in_sel = true;  }
        if ( in_sel && col == sel_end)   { result += "\x1b[27m"; in_sel = false; }

        unsigned char c = (unsigned char)s[i];
        int len = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
        while (len-- && i < n) result += s[i++];
        col++;
    }
    if (in_sel) result += "\x1b[27m";
    return result;
}

static std::string render_content(const std::string& raw, int col_start, int vis_w,
                                  bool has_sel, int fr,
                                  int ar, int ac, int br, int bc,
                                  lang l) {
    // colorize the full line so tokens that start before the visible area
    // still apply their color inside it
    std::string colored = colorize_line(raw, l);
    std::string clipped = clip_ansi(colored, col_start, vis_w);

    if (!has_sel || fr < ar || fr > br) return clipped;

    // selection bounds in visible-space coords
    int ss = (fr == ar) ? ac : 0;
    int se = (fr == br) ? bc : (int)raw.size();
    int vis_ss = std::max(0, ss - col_start);
    int vis_se = std::min(vis_w, se - col_start);

    return overlay_sel(clipped, vis_ss, vis_se);
}

static void draw(const file_buf& f, lang l) {
    int H        = term_height();
    int W        = term_width();
    int vis_rows = H - 1;
    int vis_w    = W - GUTTER;
    if (vis_w < 1) vis_w = 1;

    auto rows = build_screen(f, vis_rows, vis_w);

    // normalize selection for rendering
    bool has_sel = f.sel_row >= 0;
    int ar = f.sel_row, ac = f.sel_col;
    int br = f.cur_row, bc = f.cur_col;
    if (has_sel && (ar > br || (ar == br && ac > bc))) { std::swap(ar, br); std::swap(ac, bc); }

    std::string s;
    s.reserve(64 * 1024);
    s += "\x1b[?25l";  // hide cursor during redraw
    s += "\x1b[H";     // top-left

    for (int sr = 0; sr < vis_rows; sr++) {
        char esc[32];
        snprintf(esc, sizeof(esc), "\x1b[%d;1H", sr + 1);
        s += esc;

        if (sr < (int)rows.size()) {
            const row_info& ri = rows[sr];
            const std::string& raw = f.lines[ri.file_row];

            // gutter
            char gut[8];
            if (ri.col_start == 0)
                snprintf(gut, sizeof(gut), "%3d ", ri.file_row + 1);
            else
                memcpy(gut, "    \0", 5);
            s += GRAY + std::string(gut) + RESET;

            // content
            int col_end = f.wrap ? (int)raw.size() : ri.col_start + vis_w;
            s += render_content(raw, ri.col_start, col_end, has_sel, ri.file_row,
                                 ar, ac, br, bc, l);
        } else {
            s += GRAY "    " RESET;
        }
        s += "\x1b[K";  // clear to end of line
    }

    // status bar (last row)
    {
        char esc[32];
        snprintf(esc, sizeof(esc), "\x1b[%d;1H", H);
        s += esc + std::string("\x1b[K");

        // left side: filename + modified marker
        std::string disp = f.path;
        std::replace(disp.begin(), disp.end(), '\\', '/');
        size_t sl = disp.rfind('/');
        std::string fname = (sl != std::string::npos) ? disp.substr(sl + 1) : disp;
        if (fname.empty()) fname = disp;

        s += BLUE + fname + RESET;
        if (f.modified) s += BLUE "*" RESET;

        char info[48];
        snprintf(info, sizeof(info), "  %d:%d  ", f.cur_row + 1, f.cur_col + 1);
        s += GRAY + std::string(info);
        s += f.crlf ? "CRLF" : "LF";
        if (f.wrap) s += "  WRAP";
        s += RESET;

    }

    // cursor position
    {
        int sc_row = 1, sc_col = GUTTER + 1;
        for (int sr = 0; sr < (int)rows.size(); sr++) {
            const row_info& ri = rows[sr];
            if (ri.file_row != f.cur_row) continue;
            int next_cs = (sr + 1 < (int)rows.size() && rows[sr + 1].file_row == f.cur_row)
                          ? rows[sr + 1].col_start : INT_MAX;
            if (f.cur_col >= ri.col_start && f.cur_col < next_cs) {
                sc_row = sr + 1;
                sc_col = GUTTER + (f.cur_col - ri.col_start) + 1;
                break;
            }
        }
        char esc[32];
        snprintf(esc, sizeof(esc), "\x1b[%d;%dH", sc_row, sc_col);
        s += esc;
    }

    s += "\x1b[?25h";  // show cursor
    out(s);
}

// ---- Clipboard paste ----

static void do_paste(file_buf& f) {
    std::wstring wtext = clipboard_get();
    if (wtext.empty()) return;
    std::string text = to_utf8(wtext);
    // normalize \r\n and lone \r to \n
    std::string norm;
    norm.reserve(text.size());
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] == '\r') {
            norm += '\n';
            if (i + 1 < text.size() && text[i + 1] == '\n') i++;
        } else {
            norm += text[i];
        }
    }
    if (f.sel_row >= 0) delete_selection(f);
    for (char c : norm) {
        if (c == '\n') {
            std::string tail = f.lines[f.cur_row].substr(f.cur_col);
            f.lines[f.cur_row].erase(f.cur_col);
            f.lines.insert(f.lines.begin() + f.cur_row + 1, tail);
            f.cur_row++;
            f.cur_col = 0;
        } else {
            f.lines[f.cur_row].insert(f.cur_col, 1, c);
            f.cur_col++;
        }
    }
    f.modified = true;
}

// ---- Main entry point ----

int edit_file(const std::string& path) {
    file_buf f;
    f.path = normalize_path(path);
    load(f);
    lang l = detect_lang(f.path);

    int H, W, vis_rows, vis_w;
    auto refresh_dims = [&]() {
        H        = term_height();
        W        = term_width();
        vis_rows = H - 1;  // last row = status bar
        vis_w    = W - GUTTER;
        if (vis_rows < 1) vis_rows = 1;
        if (vis_w < 1)    vis_w    = 1;
    };
    refresh_dims();
    clamp_scroll(f, vis_rows, vis_w);
    draw(f, l);

    bool quit = false;
    while (!quit) {
        INPUT_RECORD ir;
        DWORD count;
        if (!ReadConsoleInputW(in_h, &ir, 1, &count)) break;
        if (ir.EventType == WINDOW_BUFFER_SIZE_EVENT) {
            refresh_dims();
            clamp_scroll(f, vis_rows, vis_w);
            draw(f, l);
            continue;
        }
        if (ir.EventType != KEY_EVENT || !ir.Event.KeyEvent.bKeyDown) continue;

        WORD    vk    = ir.Event.KeyEvent.wVirtualKeyCode;
        wchar_t ch    = ir.Event.KeyEvent.uChar.UnicodeChar;
        DWORD   state = ir.Event.KeyEvent.dwControlKeyState;
        bool    ctrl  = (state & (LEFT_CTRL_PRESSED  | RIGHT_CTRL_PRESSED)) != 0;
        bool    shift = (state & SHIFT_PRESSED) != 0;
        bool    alt   = (state & (LEFT_ALT_PRESSED   | RIGHT_ALT_PRESSED))  != 0;

        refresh_dims();

        // ---- Save / quit / wrap toggle ----

        if (vk == VK_F2 || (ctrl && vk == 'S')) {
            save(f);
            draw(f, l);
            continue;
        }

        if (alt && vk == 'Z') {
            f.wrap = !f.wrap;
            f.left_col = 0;
            clamp_scroll(f, vis_rows, vis_w);
            draw(f, l);
            continue;
        }

        if ((ctrl && vk == 'Q') || vk == VK_ESCAPE) {
            if (f.modified) {
                // show 3-way prompt in hint bar
                auto show_prompt = [&]() {
                    char esc2[32];
                    snprintf(esc2, sizeof(esc2), "\x1b[%d;1H\x1b[K", term_height());
                    out(std::string(esc2) +
                        BLUE "Save?" RESET " "
                        "Y" GRAY "es" RESET " "
                        "N" GRAY "o" RESET);
                };
                show_prompt();
                while (true) {
                    INPUT_RECORD ir2; DWORD c2;
                    if (!ReadConsoleInputW(in_h, &ir2, 1, &c2)) { quit = true; break; }
                    if (ir2.EventType == WINDOW_BUFFER_SIZE_EVENT) {
                        refresh_dims(); clamp_scroll(f, vis_rows, vis_w);
                        draw(f, l); show_prompt(); continue;
                    }
                    if (ir2.EventType != KEY_EVENT || !ir2.Event.KeyEvent.bKeyDown) continue;
                    wchar_t ch2 = ir2.Event.KeyEvent.uChar.UnicodeChar;
                    WORD vk2 = ir2.Event.KeyEvent.wVirtualKeyCode;
                    if (ch2 == 'y' || ch2 == 'Y') { save(f); quit = true; break; }
                    if (ch2 == 'n' || ch2 == 'N') { quit = true; break; }
                    if (ch2 == 'c' || ch2 == 'C' || vk2 == VK_ESCAPE) { draw(f, l); break; }
                }
            } else {
                quit = true;
            }
            continue;
        }

        // ---- Clipboard ----

        if (ctrl && vk == 'A') {
            f.sel_row = 0; f.sel_col = 0;
            f.cur_row = (int)f.lines.size() - 1;
            f.cur_col = (int)f.lines[f.cur_row].size();
            clamp_scroll(f, vis_rows, vis_w); draw(f, l); continue;
        }

        if (ctrl && vk == 'C') {
            if (f.sel_row >= 0) clipboard_set(to_wide(get_selection(f)));
            draw(f, l);
            continue;
        }

        if (ctrl && vk == 'X') {
            if (f.sel_row >= 0) {
                clipboard_set(to_wide(get_selection(f)));
                delete_selection(f);
                clamp_scroll(f, vis_rows, vis_w);
            }
            draw(f, l);
            continue;
        }

        if (ctrl && vk == 'V') {
            do_paste(f);
            clamp_scroll(f, vis_rows, vis_w);
            draw(f, l);
            continue;
        }

        // ---- Navigation ----

        if (vk == VK_UP) {
            if (shift) sel_begin(f); else sel_clear(f);
            if (f.cur_row > 0) { f.cur_row--; clamp_col(f); }
            clamp_scroll(f, vis_rows, vis_w); draw(f, l); continue;
        }

        if (vk == VK_DOWN) {
            if (shift) sel_begin(f); else sel_clear(f);
            if (f.cur_row < (int)f.lines.size() - 1) { f.cur_row++; clamp_col(f); }
            clamp_scroll(f, vis_rows, vis_w); draw(f, l); continue;
        }

        if (vk == VK_LEFT) {
            if (shift) sel_begin(f); else sel_clear(f);
            if (ctrl) {
                word_left(f);
            } else if (f.cur_col > 0) {
                f.cur_col = utf8_prev(f.lines[f.cur_row], f.cur_col);
            } else if (f.cur_row > 0) {
                f.cur_row--;
                f.cur_col = (int)f.lines[f.cur_row].size();
            }
            clamp_scroll(f, vis_rows, vis_w); draw(f, l); continue;
        }

        if (vk == VK_RIGHT) {
            if (shift) sel_begin(f); else sel_clear(f);
            if (ctrl) {
                word_right(f);
            } else if (f.cur_col < (int)f.lines[f.cur_row].size()) {
                f.cur_col = utf8_next(f.lines[f.cur_row], f.cur_col);
            } else if (f.cur_row < (int)f.lines.size() - 1) {
                f.cur_row++;
                f.cur_col = 0;
            }
            clamp_scroll(f, vis_rows, vis_w); draw(f, l); continue;
        }

        if (vk == VK_HOME) {
            if (shift) sel_begin(f); else sel_clear(f);
            f.cur_col = ctrl ? (f.cur_row = 0, 0) : 0;
            clamp_scroll(f, vis_rows, vis_w); draw(f, l); continue;
        }

        if (vk == VK_END) {
            if (shift) sel_begin(f); else sel_clear(f);
            if (ctrl) {
                f.cur_row = (int)f.lines.size() - 1;
                f.cur_col = (int)f.lines[f.cur_row].size();
            } else {
                f.cur_col = (int)f.lines[f.cur_row].size();
            }
            clamp_scroll(f, vis_rows, vis_w); draw(f, l); continue;
        }

        if (vk == VK_PRIOR) {  // Page Up
            sel_clear(f);
            if (ctrl) { f.cur_row = 0; f.cur_col = 0; }
            else { f.cur_row -= vis_rows - 1; clamp_col(f); }
            clamp_scroll(f, vis_rows, vis_w); draw(f, l); continue;
        }

        if (vk == VK_NEXT) {  // Page Down
            sel_clear(f);
            if (ctrl) {
                f.cur_row = (int)f.lines.size() - 1;
                f.cur_col = (int)f.lines[f.cur_row].size();
            } else {
                f.cur_row += vis_rows - 1;
                clamp_col(f);
            }
            clamp_scroll(f, vis_rows, vis_w); draw(f, l); continue;
        }

        // ---- Editing ----

        if (vk == VK_RETURN) {
            if (f.sel_row >= 0) delete_selection(f);
            std::string tail = f.lines[f.cur_row].substr(f.cur_col);
            f.lines[f.cur_row].erase(f.cur_col);
            f.lines.insert(f.lines.begin() + f.cur_row + 1, tail);
            f.cur_row++;
            f.cur_col  = 0;
            f.modified = true;
            clamp_scroll(f, vis_rows, vis_w); draw(f, l); continue;
        }

        if (vk == VK_BACK) {
            if (f.sel_row >= 0) {
                delete_selection(f);
            } else if (f.cur_col > 0) {
                int prev = utf8_prev(f.lines[f.cur_row], f.cur_col);
                f.lines[f.cur_row].erase(prev, f.cur_col - prev);
                f.cur_col  = prev;
                f.modified = true;
            } else if (f.cur_row > 0) {
                int prev_len = (int)f.lines[f.cur_row - 1].size();
                f.lines[f.cur_row - 1] += f.lines[f.cur_row];
                f.lines.erase(f.lines.begin() + f.cur_row);
                f.cur_row--;
                f.cur_col  = prev_len;
                f.modified = true;
            }
            clamp_scroll(f, vis_rows, vis_w); draw(f, l); continue;
        }

        if (vk == VK_DELETE) {
            if (f.sel_row >= 0) {
                delete_selection(f);
            } else if (f.cur_col < (int)f.lines[f.cur_row].size()) {
                int next = utf8_next(f.lines[f.cur_row], f.cur_col);
                f.lines[f.cur_row].erase(f.cur_col, next - f.cur_col);
                f.modified = true;
            } else if (f.cur_row < (int)f.lines.size() - 1) {
                f.lines[f.cur_row] += f.lines[f.cur_row + 1];
                f.lines.erase(f.lines.begin() + f.cur_row + 1);
                f.modified = true;
            }
            clamp_scroll(f, vis_rows, vis_w); draw(f, l); continue;
        }

        if (vk == VK_TAB && shift) {
            // dedent: remove up to 4 leading spaces from each affected line
            auto dedent = [&](int row) -> int {
                int removed = 0;
                while (removed < 4 && !f.lines[row].empty() && f.lines[row][0] == ' ') {
                    f.lines[row].erase(0, 1);
                    removed++;
                }
                return removed;
            };
            if (f.sel_row >= 0) {
                int ar = f.sel_row, br = f.cur_row;
                if (ar > br) std::swap(ar, br);
                for (int row = ar; row <= br; row++) dedent(row);
                // clamp cols since lines may have shrunk
                if (f.sel_col > (int)f.lines[f.sel_row].size()) f.sel_col = (int)f.lines[f.sel_row].size();
            } else {
                dedent(f.cur_row);
            }
            f.modified = true;
            clamp_scroll(f, vis_rows, vis_w); draw(f, l); continue;
        }

        if (vk == VK_TAB) {
            if (f.sel_row >= 0) {
                // indent every selected line by 4 spaces
                int ar = f.sel_row, br = f.cur_row;
                if (ar > br) std::swap(ar, br);
                for (int row = ar; row <= br; row++)
                    f.lines[row].insert(0, 4, ' ');
                f.sel_col += 4;
                f.cur_col += 4;
            } else {
                f.lines[f.cur_row].insert(f.cur_col, 4, ' ');
                f.cur_col += 4;
            }
            f.modified = true;
            clamp_scroll(f, vis_rows, vis_w); draw(f, l); continue;
        }

        // printable character
        if (ch >= 32 && ch != 127 && !ctrl && !alt) {
            if (f.sel_row >= 0) delete_selection(f);
            std::string bytes = to_utf8(std::wstring(1, ch));
            f.lines[f.cur_row].insert(f.cur_col, bytes);
            f.cur_col += (int)bytes.size();
            f.modified = true;
            clamp_scroll(f, vis_rows, vis_w); draw(f, l); continue;
        }
    }

    // clear editor screen before returning to shell
    out("\x1b[2J\x1b[H\x1b[?25h");
    return 0;
}
