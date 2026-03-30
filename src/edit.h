// MODULE: edit
// Purpose : full-screen terminal file editor
// Exports : edit_file(), view_file()
// Depends : common.h, terminal.h, highlight.h

static const int GUTTER = 4; // "%3d " — 3-digit line number + space

struct edit_op {
    int r0, c0;            // range start (byte offsets into lines)
    int r1, c1;            // range end
    std::string old_text;  // text that was removed (may contain \n)
    std::string new_text;  // text that replaced it (may contain \n)
    int cur_row_before, cur_col_before;
    int cur_row_after,  cur_col_after;
};

static const int MAX_UNDO = 500;

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

    std::vector<edit_op> undo_stack;
    std::vector<edit_op> redo_stack;
    bool last_was_char = false;  // consecutive word-char typing → merge
    int  last_del_dir  = 0;      // -1=backspace, +1=fwd-delete, 0=none
    int  saved_gen     = 0;      // undo_stack.size() at last save
};

static bool looks_binary_bytes(const std::string& data) {
    if (data.empty()) return false;

    int suspicious = 0;
    for (unsigned char c : data) {
        if (c == 0) return true;
        if (c < 32 && c != '\n' && c != '\r' && c != '\t' && c != '\f')
            suspicious++;
    }
    return suspicious > (int)data.size() / 10;
}

static bool probe_binary_file(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return false;

    char buf[4096];
    ifs.read(buf, sizeof(buf));
    std::streamsize got = ifs.gcount();
    if (got <= 0) return false;
    return looks_binary_bytes(std::string(buf, (size_t)got));
}

static std::string clip_plain(const std::string& s, int width) {
    if (width <= 0) return "";
    if ((int)s.size() <= width) return s;
    if (width <= 3) return std::string(width, '.');
    return s.substr(0, width - 3) + "...";
}

static int show_binary_notice(const std::string& path, bool readonly) {
    auto redraw = [&]() {
        int H = term_height();
        int W = term_width();
        std::string disp = path;
        std::replace(disp.begin(), disp.end(), '\\', '/');

        out("\x1b[2J\x1b[H");
        int row = std::max(1, H / 2 - 1);
        auto line = [&](int y, const std::string& text, const std::string& color = "") {
            char esc[32];
            snprintf(esc, sizeof(esc), "\x1b[%d;1H\x1b[K", y);
            std::string body = clip_plain(text, std::max(1, W));
            out(std::string(esc) + color + body + RESET);
        };

        line(row, readonly ? "Binary file view is not supported yet." : "Binary file edit is not supported.", YELLOW);
        line(row + 1, disp, GRAY);
        line(row + 3, "Press any key to return.", BLUE);
    };

    redraw();
    while (true) {
        INPUT_RECORD ir;
        DWORD count = 0;
        if (!ReadConsoleInputW(in_h, &ir, 1, &count)) break;
        if (ir.EventType == WINDOW_BUFFER_SIZE_EVENT) {
            redraw();
            continue;
        }
        if (ir.EventType == KEY_EVENT && ir.Event.KeyEvent.bKeyDown)
            break;
    }
    out("\x1b[2J\x1b[H\x1b[?25h");
    return 1;
}

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

// Number of display columns from byte 0 to byte_pos (each codepoint = 1 col)
static int utf8_display_col(const std::string& s, int byte_pos) {
    int col = 0, i = 0;
    while (i < byte_pos && i < (int)s.size()) {
        unsigned char c = (unsigned char)s[i];
        i += (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
        col++;
    }
    return col;
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

// ---- Undo / redo helpers ----

// Extract text from f.lines in range (r0,c0)..(r1,c1) as \n-joined string.
static std::string range_text(const file_buf& f, int r0, int c0, int r1, int c1) {
    if (r0 == r1) return f.lines[r0].substr(c0, c1 - c0);
    std::string txt = f.lines[r0].substr(c0);
    for (int r = r0 + 1; r < r1; r++) { txt += '\n'; txt += f.lines[r]; }
    txt += '\n';
    txt += f.lines[r1].substr(0, c1);
    return txt;
}

// Replace range (r0,c0)..(r1,c1) in f.lines with text (which may contain \n).
static void apply_range(file_buf& f, int r0, int c0, int r1, int c1,
                        const std::string& text) {
    // split replacement text into lines
    std::vector<std::string> parts;
    std::string cur;
    for (char c : text) {
        if (c == '\n') { parts.push_back(cur); cur.clear(); }
        else cur += c;
    }
    parts.push_back(cur);

    std::string prefix = f.lines[r0].substr(0, c0);
    std::string suffix = f.lines[r1].substr(c1);

    // build replacement rows
    std::vector<std::string> rows;
    rows.reserve(parts.size());
    for (int i = 0; i < (int)parts.size(); i++) {
        if (i == 0 && i == (int)parts.size() - 1)
            rows.push_back(prefix + parts[i] + suffix);
        else if (i == 0)
            rows.push_back(prefix + parts[i]);
        else if (i == (int)parts.size() - 1)
            rows.push_back(parts[i] + suffix);
        else
            rows.push_back(parts[i]);
    }

    f.lines.erase(f.lines.begin() + r0, f.lines.begin() + r1 + 1);
    f.lines.insert(f.lines.begin() + r0, rows.begin(), rows.end());
    if (f.lines.empty()) f.lines.push_back("");
}

// Push current state onto undo stack and clear redo stack.
static void push_undo(file_buf& f, int r0, int c0, int r1, int c1,
                      const std::string& old_text, const std::string& new_text,
                      int cr_before, int cc_before,
                      int cr_after,  int cc_after) {
    if ((int)f.undo_stack.size() >= MAX_UNDO)
        f.undo_stack.erase(f.undo_stack.begin());
    f.undo_stack.push_back({r0, c0, r1, c1, old_text, new_text,
                            cr_before, cc_before, cr_after, cc_after});
    f.redo_stack.clear();
}

static void do_undo(file_buf& f) {
    if (f.undo_stack.empty()) return;
    const edit_op& op = f.undo_stack.back();
    // compute end of new_text as inserted (to know the range to reverse)
    int nr = op.r0, nc = op.c0;
    for (char c : op.new_text) { if (c == '\n') { nr++; nc = 0; } else nc++; }
    apply_range(f, op.r0, op.c0, nr, nc, op.old_text);
    f.cur_row = op.cur_row_before;
    f.cur_col = op.cur_col_before;
    f.redo_stack.push_back(op);
    f.undo_stack.pop_back();
    f.sel_row = -1; f.sel_col = -1;
    f.last_was_char = false;
    f.last_del_dir  = 0;
    f.modified = ((int)f.undo_stack.size() != f.saved_gen);
}

static void do_redo(file_buf& f) {
    if (f.redo_stack.empty()) return;
    const edit_op& op = f.redo_stack.back();
    // compute end of old_text as it sits (to know the range to replace)
    int nr = op.r0, nc = op.c0;
    for (char c : op.old_text) { if (c == '\n') { nr++; nc = 0; } else nc++; }
    apply_range(f, op.r0, op.c0, nr, nc, op.new_text);
    f.cur_row = op.cur_row_after;
    f.cur_col = op.cur_col_after;
    f.undo_stack.push_back(op);
    f.redo_stack.pop_back();
    f.sel_row = -1; f.sel_col = -1;
    f.last_was_char = false;
    f.last_del_dir  = 0;
    f.modified = ((int)f.undo_stack.size() != f.saved_gen);
}

// ---- Cursor clamping & scrolling ----

static void clamp_col(file_buf& f) {
    if (f.cur_row < 0) f.cur_row = 0;
    if (f.cur_row >= (int)f.lines.size()) f.cur_row = (int)f.lines.size() - 1;
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
        int dc = utf8_display_col(f.lines[f.cur_row], f.cur_col);
        if (dc < f.left_col)              f.left_col = dc;
        if (dc >= f.left_col + vis_w)     f.left_col = dc - vis_w + 1;
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
            int disp   = utf8_display_col(f.lines[fr], (int)f.lines[fr].size());
            int chunks = (disp == 0) ? 1 : (disp + vis_w - 1) / vis_w;
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

    // selection bounds in visible-space coords (convert byte offsets to display cols)
    int ss = (fr == ar) ? utf8_display_col(raw, ac) : 0;
    int se = (fr == br) ? utf8_display_col(raw, bc) : utf8_display_col(raw, (int)raw.size());
    int vis_ss = std::max(0, ss - col_start);
    int vis_se = std::min(vis_w, se - col_start);

    return overlay_sel(clipped, vis_ss, vis_se);
}

static void draw(const file_buf& f, lang l, bool readonly = false) {
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
        if (readonly) s += "  VIEW";
        s += RESET;

    }

    // cursor position
    {
        int sc_row = 1, sc_col = GUTTER + 1;
        int dc = utf8_display_col(f.lines[f.cur_row], f.cur_col);
        for (int sr = 0; sr < (int)rows.size(); sr++) {
            const row_info& ri = rows[sr];
            if (ri.file_row != f.cur_row) continue;
            int next_cs = (sr + 1 < (int)rows.size() && rows[sr + 1].file_row == f.cur_row)
                          ? rows[sr + 1].col_start : INT_MAX;
            if (dc >= ri.col_start && dc < next_cs) {
                sc_row = sr + 1;
                sc_col = GUTTER + (dc - ri.col_start) + 1;
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

static int edit_file_mode(const std::string& path, bool readonly) {
    file_buf f;
    f.path = normalize_path(path);
    if (probe_binary_file(f.path))
        return show_binary_notice(f.path, readonly);
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
    draw(f, l, readonly);

    bool quit = false;
    while (!quit) {
        INPUT_RECORD ir;
        DWORD count;
        if (!ReadConsoleInputW(in_h, &ir, 1, &count)) break;
        if (ir.EventType == WINDOW_BUFFER_SIZE_EVENT) {
            refresh_dims();
            clamp_scroll(f, vis_rows, vis_w);
            draw(f, l, readonly);
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
            if (readonly) {
                draw(f, l, readonly);
                continue;
            }
            save(f);
            f.saved_gen = (int)f.undo_stack.size();
            draw(f, l, readonly);
            continue;
        }

        if (alt && vk == 'Z') {
            f.wrap = !f.wrap;
            f.left_col = 0;
            clamp_scroll(f, vis_rows, vis_w);
            draw(f, l, readonly);
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
                        draw(f, l, readonly); show_prompt(); continue;
                    }
                    if (ir2.EventType != KEY_EVENT || !ir2.Event.KeyEvent.bKeyDown) continue;
                    wchar_t ch2 = ir2.Event.KeyEvent.uChar.UnicodeChar;
                    WORD vk2 = ir2.Event.KeyEvent.wVirtualKeyCode;
                    if (ch2 == 'y' || ch2 == 'Y') { save(f); quit = true; break; }
                    if (ch2 == 'n' || ch2 == 'N') { quit = true; break; }
                    if (ch2 == 'c' || ch2 == 'C' || vk2 == VK_ESCAPE) { draw(f, l, readonly); break; }
                }
            } else {
                quit = true;
            }
            continue;
        }

        // ---- Undo / redo ----

        if (ctrl && !shift && vk == 'Z') {
            do_undo(f);
            clamp_scroll(f, vis_rows, vis_w); draw(f, l, readonly); continue;
        }

        if ((ctrl && !shift && vk == 'Y') || (ctrl && shift && vk == 'Z')) {
            do_redo(f);
            clamp_scroll(f, vis_rows, vis_w); draw(f, l, readonly); continue;
        }

        // ---- Clipboard ----

        if (ctrl && vk == 'A') {
            f.sel_row = 0; f.sel_col = 0;
            f.cur_row = (int)f.lines.size() - 1;
            f.cur_col = (int)f.lines[f.cur_row].size();
            clamp_scroll(f, vis_rows, vis_w); draw(f, l, readonly); continue;
        }

        if (ctrl && vk == 'C') {
            if (f.sel_row >= 0) clipboard_set(to_wide(get_selection(f)));
            draw(f, l, readonly);
            continue;
        }

        if (ctrl && vk == 'X') {
            if (readonly) {
                draw(f, l, readonly);
                continue;
            }
            if (f.sel_row >= 0) {
                int ar = f.sel_row, ac = f.sel_col, br = f.cur_row, bc = f.cur_col;
                if (ar > br || (ar==br && ac>bc)) { std::swap(ar,br); std::swap(ac,bc); }
                clipboard_set(to_wide(get_selection(f)));
                push_undo(f, ar, ac, br, bc, range_text(f, ar, ac, br, bc), "",
                          f.cur_row, f.cur_col, ar, ac);
                f.last_was_char = false; f.last_del_dir = 0;
                delete_selection(f);
                clamp_scroll(f, vis_rows, vis_w);
            }
            draw(f, l, readonly);
            continue;
        }

        if ((ctrl && vk == 'D') || (ctrl && shift && vk == 'K')) {
            if (readonly) {
                draw(f, l, readonly);
                continue;
            }
            // delete current line (Ctrl+D primary, Ctrl+Shift+K VSCode alias)
            int cr = f.cur_row, cc = f.cur_col;
            f.last_was_char = false; f.last_del_dir = 0;
            if ((int)f.lines.size() > 1) {
                if (cr < (int)f.lines.size() - 1) {
                    push_undo(f, cr, 0, cr+1, 0, f.lines[cr]+"\n", "",
                              cr, cc, cr, 0);
                    f.lines.erase(f.lines.begin() + cr);
                } else {
                    int prev_len = (int)f.lines[cr-1].size();
                    push_undo(f, cr-1, prev_len, cr, (int)f.lines[cr].size(),
                              "\n"+f.lines[cr], "", cr, cc, cr-1, 0);
                    f.lines.erase(f.lines.begin() + cr);
                    f.cur_row = cr - 1;
                }
            } else {
                push_undo(f, 0, 0, 0, (int)f.lines[0].size(), f.lines[0], "",
                          0, cc, 0, 0);
                f.lines[0].clear();
            }
            f.cur_col = 0; f.sel_row = -1; f.modified = true;
            clamp_scroll(f, vis_rows, vis_w); draw(f, l, readonly); continue;
        }

        if (ctrl && vk == 'V') {
            if (readonly) {
                draw(f, l, readonly);
                continue;
            }
            std::wstring wclip = clipboard_get();
            if (!wclip.empty()) {
                // normalize clipboard to \n — same logic as do_paste — to use as new_text
                std::string raw = to_utf8(wclip);
                std::string new_text;
                new_text.reserve(raw.size());
                for (size_t i = 0; i < raw.size(); i++) {
                    if (raw[i] == '\r') { new_text += '\n'; if (i+1 < raw.size() && raw[i+1] == '\n') i++; }
                    else new_text += raw[i];
                }
                int r0 = f.cur_row, c0 = f.cur_col;
                int r1 = f.cur_row, c1 = f.cur_col;
                std::string old_text;
                if (f.sel_row >= 0) {
                    int ar = f.sel_row, ac = f.sel_col, br = f.cur_row, bc = f.cur_col;
                    if (ar > br || (ar==br && ac>bc)) { std::swap(ar,br); std::swap(ac,bc); }
                    r0=ar; c0=ac; r1=br; c1=bc;
                    old_text = range_text(f, r0, c0, r1, c1);
                }
                int cr_before = f.cur_row, cc_before = f.cur_col;
                f.last_was_char = false; f.last_del_dir = 0;
                do_paste(f);
                push_undo(f, r0, c0, r1, c1, old_text, new_text,
                          cr_before, cc_before, f.cur_row, f.cur_col);
            }
            clamp_scroll(f, vis_rows, vis_w);
            draw(f, l, readonly);
            continue;
        }

        // ---- Navigation ----

        if (vk == VK_UP) {
            if (shift) sel_begin(f); else sel_clear(f);
            if (f.cur_row > 0) { f.cur_row--; clamp_col(f); }
            clamp_scroll(f, vis_rows, vis_w); draw(f, l, readonly); continue;
        }

        if (vk == VK_DOWN) {
            if (shift) sel_begin(f); else sel_clear(f);
            if (f.cur_row < (int)f.lines.size() - 1) { f.cur_row++; clamp_col(f); }
            clamp_scroll(f, vis_rows, vis_w); draw(f, l, readonly); continue;
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
            clamp_scroll(f, vis_rows, vis_w); draw(f, l, readonly); continue;
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
            clamp_scroll(f, vis_rows, vis_w); draw(f, l, readonly); continue;
        }

        if (vk == VK_HOME) {
            if (shift) sel_begin(f); else sel_clear(f);
            f.cur_col = ctrl ? (f.cur_row = 0, 0) : 0;
            clamp_scroll(f, vis_rows, vis_w); draw(f, l, readonly); continue;
        }

        if (vk == VK_END) {
            if (shift) sel_begin(f); else sel_clear(f);
            if (ctrl) {
                f.cur_row = (int)f.lines.size() - 1;
                f.cur_col = (int)f.lines[f.cur_row].size();
            } else {
                f.cur_col = (int)f.lines[f.cur_row].size();
            }
            clamp_scroll(f, vis_rows, vis_w); draw(f, l, readonly); continue;
        }

        if (vk == VK_PRIOR) {  // Page Up
            sel_clear(f);
            if (ctrl) { f.cur_row = 0; f.cur_col = 0; }
            else { f.cur_row -= vis_rows - 1; clamp_col(f); }
            clamp_scroll(f, vis_rows, vis_w); draw(f, l, readonly); continue;
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
            clamp_scroll(f, vis_rows, vis_w); draw(f, l, readonly); continue;
        }

        // ---- Editing ----

        if (vk == VK_RETURN) {
            if (readonly) {
                draw(f, l, readonly);
                continue;
            }
            f.last_was_char = false; f.last_del_dir = 0;
            if (f.sel_row >= 0) {
                int ar = f.sel_row, ac = f.sel_col, br = f.cur_row, bc = f.cur_col;
                if (ar > br || (ar==br && ac>bc)) { std::swap(ar,br); std::swap(ac,bc); }
                push_undo(f, ar, ac, br, bc, range_text(f, ar, ac, br, bc), "\n",
                          f.cur_row, f.cur_col, ar+1, 0);
                delete_selection(f);
            } else {
                push_undo(f, f.cur_row, f.cur_col, f.cur_row, f.cur_col, "", "\n",
                          f.cur_row, f.cur_col, f.cur_row+1, 0);
            }
            std::string tail = f.lines[f.cur_row].substr(f.cur_col);
            f.lines[f.cur_row].erase(f.cur_col);
            f.lines.insert(f.lines.begin() + f.cur_row + 1, tail);
            f.cur_row++;
            f.cur_col  = 0;
            f.modified = true;
            clamp_scroll(f, vis_rows, vis_w); draw(f, l, readonly); continue;
        }

        if (vk == VK_BACK) {
            if (readonly) {
                draw(f, l, readonly);
                continue;
            }
            if (f.sel_row >= 0) {
                int ar = f.sel_row, ac = f.sel_col, br = f.cur_row, bc = f.cur_col;
                if (ar > br || (ar==br && ac>bc)) { std::swap(ar,br); std::swap(ac,bc); }
                push_undo(f, ar, ac, br, bc, range_text(f, ar, ac, br, bc), "",
                          f.cur_row, f.cur_col, ar, ac);
                f.last_was_char = false; f.last_del_dir = 0;
                delete_selection(f);
            } else if (f.cur_col > 0) {
                int prev = utf8_prev(f.lines[f.cur_row], f.cur_col);
                std::string del = f.lines[f.cur_row].substr(prev, f.cur_col - prev);
                bool can_group = f.last_del_dir == -1 && !f.undo_stack.empty() &&
                                 f.undo_stack.back().new_text.empty() &&
                                 f.undo_stack.back().r0 == f.cur_row &&
                                 f.undo_stack.back().c0 == f.cur_col;
                if (can_group) {
                    f.undo_stack.back().old_text = del + f.undo_stack.back().old_text;
                    f.undo_stack.back().c0 = prev;
                    f.undo_stack.back().cur_col_after = prev;
                    f.redo_stack.clear();
                } else {
                    push_undo(f, f.cur_row, prev, f.cur_row, f.cur_col, del, "",
                              f.cur_row, f.cur_col, f.cur_row, prev);
                    f.last_was_char = false; f.last_del_dir = -1;
                }
                f.lines[f.cur_row].erase(prev, f.cur_col - prev);
                f.cur_col  = prev;
                f.modified = true;
            } else if (f.cur_row > 0) {
                int prev_len = (int)f.lines[f.cur_row - 1].size();
                push_undo(f, f.cur_row-1, prev_len, f.cur_row, 0, "\n", "",
                          f.cur_row, f.cur_col, f.cur_row-1, prev_len);
                f.last_was_char = false; f.last_del_dir = 0;
                f.lines[f.cur_row - 1] += f.lines[f.cur_row];
                f.lines.erase(f.lines.begin() + f.cur_row);
                f.cur_row--;
                f.cur_col  = prev_len;
                f.modified = true;
            }
            clamp_scroll(f, vis_rows, vis_w); draw(f, l, readonly); continue;
        }

        if (vk == VK_DELETE) {
            if (readonly) {
                draw(f, l, readonly);
                continue;
            }
            if (f.sel_row >= 0) {
                int ar = f.sel_row, ac = f.sel_col, br = f.cur_row, bc = f.cur_col;
                if (ar > br || (ar==br && ac>bc)) { std::swap(ar,br); std::swap(ac,bc); }
                push_undo(f, ar, ac, br, bc, range_text(f, ar, ac, br, bc), "",
                          f.cur_row, f.cur_col, ar, ac);
                f.last_was_char = false; f.last_del_dir = 0;
                delete_selection(f);
            } else if (f.cur_col < (int)f.lines[f.cur_row].size()) {
                int next = utf8_next(f.lines[f.cur_row], f.cur_col);
                std::string del = f.lines[f.cur_row].substr(f.cur_col, next - f.cur_col);
                bool can_group = f.last_del_dir == 1 && !f.undo_stack.empty() &&
                                 f.undo_stack.back().new_text.empty() &&
                                 f.undo_stack.back().r0 == f.cur_row &&
                                 f.undo_stack.back().c0 == f.cur_col;
                if (can_group) {
                    f.undo_stack.back().old_text += del;
                    f.undo_stack.back().c1 = next;
                    f.redo_stack.clear();
                } else {
                    push_undo(f, f.cur_row, f.cur_col, f.cur_row, next, del, "",
                              f.cur_row, f.cur_col, f.cur_row, f.cur_col);
                    f.last_was_char = false; f.last_del_dir = 1;
                }
                f.lines[f.cur_row].erase(f.cur_col, next - f.cur_col);
                f.modified = true;
            } else if (f.cur_row < (int)f.lines.size() - 1) {
                push_undo(f, f.cur_row, f.cur_col, f.cur_row+1, 0, "\n", "",
                          f.cur_row, f.cur_col, f.cur_row, f.cur_col);
                f.last_was_char = false; f.last_del_dir = 0;
                f.lines[f.cur_row] += f.lines[f.cur_row + 1];
                f.lines.erase(f.lines.begin() + f.cur_row + 1);
                f.modified = true;
            }
            clamp_scroll(f, vis_rows, vis_w); draw(f, l, readonly); continue;
        }

        if (vk == VK_TAB && shift) {
            if (readonly) {
                draw(f, l, readonly);
                continue;
            }
            // dedent: remove up to 4 leading spaces from each affected line
            auto dedent = [&](int row) -> int {
                int removed = 0;
                while (removed < 4 && !f.lines[row].empty() && f.lines[row][0] == ' ') {
                    f.lines[row].erase(0, 1);
                    removed++;
                }
                return removed;
            };
            int cr = f.cur_row, cc = f.cur_col;
            f.last_was_char = false; f.last_del_dir = 0;
            if (f.sel_row >= 0) {
                int ar = f.sel_row, br = f.cur_row;
                if (ar > br) std::swap(ar, br);
                int old_br_len = (int)f.lines[br].size();
                std::string old_text = range_text(f, ar, 0, br, old_br_len);
                for (int row = ar; row <= br; row++) dedent(row);
                if (f.sel_col > (int)f.lines[f.sel_row].size()) f.sel_col = (int)f.lines[f.sel_row].size();
                std::string new_text = range_text(f, ar, 0, br, (int)f.lines[br].size());
                push_undo(f, ar, 0, br, old_br_len, old_text, new_text, cr, cc, f.cur_row, f.cur_col);
            } else {
                std::string old_line = f.lines[cr];
                dedent(cr);
                if (f.lines[cr] != old_line)
                    push_undo(f, cr, 0, cr, (int)old_line.size(), old_line, f.lines[cr], cr, cc, cr, cc);
            }
            f.modified = true;
            clamp_scroll(f, vis_rows, vis_w); draw(f, l, readonly); continue;
        }

        if (vk == VK_TAB) {
            if (readonly) {
                draw(f, l, readonly);
                continue;
            }
            int cr = f.cur_row, cc = f.cur_col;
            f.last_was_char = false; f.last_del_dir = 0;
            if (f.sel_row >= 0) {
                // indent every selected line by 4 spaces
                int ar = f.sel_row, br = f.cur_row;
                if (ar > br) std::swap(ar, br);
                int old_br_len = (int)f.lines[br].size();
                std::string old_text = range_text(f, ar, 0, br, old_br_len);
                for (int row = ar; row <= br; row++) f.lines[row].insert(0, 4, ' ');
                f.sel_col += 4; f.cur_col += 4;
                std::string new_text = range_text(f, ar, 0, br, (int)f.lines[br].size());
                push_undo(f, ar, 0, br, old_br_len, old_text, new_text, cr, cc, f.cur_row, f.cur_col);
            } else {
                push_undo(f, cr, cc, cr, cc, "", "    ", cr, cc, cr, cc+4);
                f.lines[cr].insert(cc, 4, ' ');
                f.cur_col += 4;
            }
            f.modified = true;
            clamp_scroll(f, vis_rows, vis_w); draw(f, l, readonly); continue;
        }

        // printable character
        if (ch >= 32 && ch != 127 && !ctrl && !alt) {
            if (readonly) {
                draw(f, l, readonly);
                continue;
            }
            // drain any immediately pending chars — if the input buffer already has more,
            // this is a paste arriving char-by-char; batch the whole burst into one undo op
            std::wstring wbuf(1, ch);
            {
                DWORD pending = 0;
                while (GetNumberOfConsoleInputEvents(in_h, &pending) && pending > 0) {
                    INPUT_RECORD ir2; DWORD r2 = 0;
                    if (!PeekConsoleInputW(in_h, &ir2, 1, &r2) || r2 == 0) break;
                    if (ir2.EventType != KEY_EVENT || !ir2.Event.KeyEvent.bKeyDown) {
                        ReadConsoleInputW(in_h, &ir2, 1, &r2); continue;
                    }
                    wchar_t ch2 = ir2.Event.KeyEvent.uChar.UnicodeChar;
                    DWORD   st2 = ir2.Event.KeyEvent.dwControlKeyState;
                    WORD    vk2 = ir2.Event.KeyEvent.wVirtualKeyCode;
                    bool    ct2 = (st2 & (LEFT_CTRL_PRESSED|RIGHT_CTRL_PRESSED)) != 0;
                    bool    al2 = (st2 & (LEFT_ALT_PRESSED|RIGHT_ALT_PRESSED)) != 0;
                    if (ch2 >= 32 && ch2 != 127 && !ct2 && !al2) {
                        ReadConsoleInputW(in_h, &ir2, 1, &r2); wbuf += ch2;
                    } else if (vk2 == VK_RETURN && !ct2 && !al2) {
                        ReadConsoleInputW(in_h, &ir2, 1, &r2); wbuf += L'\n';
                    } else {
                        break;  // non-paste event: leave it in the queue
                    }
                }
            }
            std::string bytes = to_utf8(wbuf);
            bool is_batch  = (wbuf.size() > 1);
            bool is_word   = !is_batch &&
                             ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                              (ch >= '0' && ch <= '9') || ch == '_' || ch > 127);

            // capture before-state
            bool prev_was_char = f.last_was_char;
            int cr_before = f.cur_row, cc_before = f.cur_col;
            int r0 = f.cur_row, c0 = f.cur_col, r1 = f.cur_row, c1 = f.cur_col;
            std::string old_text;
            if (f.sel_row >= 0) {
                int ar = f.sel_row, ac = f.sel_col, br = f.cur_row, bc = f.cur_col;
                if (ar > br || (ar==br && ac>bc)) { std::swap(ar,br); std::swap(ac,bc); }
                r0=ar; c0=ac; r1=br; c1=bc;
                old_text = range_text(f, r0, c0, r1, c1);
                delete_selection(f);
            }
            f.last_was_char = false; f.last_del_dir = 0;

            // insert bytes — handles embedded newlines from batched paste
            for (size_t i = 0; i < bytes.size(); ) {
                if (bytes[i] == '\n') {
                    std::string tail = f.lines[f.cur_row].substr(f.cur_col);
                    f.lines[f.cur_row].erase(f.cur_col);
                    f.lines.insert(f.lines.begin() + f.cur_row + 1, tail);
                    f.cur_row++; f.cur_col = 0; i++;
                } else {
                    size_t nl = bytes.find('\n', i);
                    if (nl == std::string::npos) nl = bytes.size();
                    std::string chunk = bytes.substr(i, nl - i);
                    f.lines[f.cur_row].insert(f.cur_col, chunk);
                    f.cur_col += (int)chunk.size();
                    i = nl;
                }
            }

            // undo: batch or selection-replace → always own op; single word char → try group
            bool can_group = is_word && prev_was_char && old_text.empty() &&
                             !f.undo_stack.empty() &&
                             f.undo_stack.back().old_text.empty() &&
                             f.undo_stack.back().r0 == r0 &&
                             f.undo_stack.back().cur_col_after == cc_before;
            if (can_group) {
                f.undo_stack.back().new_text += bytes;
                f.undo_stack.back().cur_col_after = f.cur_col;
                f.redo_stack.clear();
            } else {
                push_undo(f, r0, c0, r1, c1, old_text, bytes,
                          cr_before, cc_before, f.cur_row, f.cur_col);
            }
            if (is_word) f.last_was_char = true;

            f.modified = true;
            clamp_scroll(f, vis_rows, vis_w); draw(f, l, readonly); continue;
        }
    }

    // clear editor screen before returning to shell
    out("\x1b[2J\x1b[H\x1b[?25h");
    return 0;
}

int edit_file(const std::string& path) {
    return edit_file_mode(path, false);
}

int view_file(const std::string& path) {
    return edit_file_mode(path, true);
}
