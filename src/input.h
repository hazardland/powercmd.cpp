// MODULE: input
// Purpose : input loop, cursor, multiline, history nav, hint display, tab, paste, redraw
// Exports : struct input | find_hint() redraw() readline()
// Depends : common.h, signal.h (ctrl_c_fired), complete.h, terminal.h, append_history() fwd-decl from signal.h

// All mutable state for one input session. hist persists across prompts; all other
// fields are reset after each Enter so each new line starts clean.
struct input {
    std::wstring buf;           // what the user has typed; multiline content uses embedded \n
    int pos        = 0;         // cursor position within buf (not screen column)
    int prev_pos   = 0;         // buf position as of last redraw; used to compute how many rows to move up
    int cursor_row = 0;         // physical row the cursor is on after last redraw (0 = prompt row)
    int prompt_vis = 0;         // visual width of prompt_str (no ANSI codes); needed for screen-column math
    std::string prompt_str;     // stored so redraw can reprint it without recomputing

    std::vector<std::wstring> hist; // history list oldest-first; deduplicated so each command appears once
    int hist_idx  = -1;         // -1 = edit mode; >= 0 = index into hist during UP/DOWN navigation
    std::wstring saved;         // prefix filter for navigation (buf at UP time, or empty for plain nav)
    std::wstring draft;         // uncommitted buf snapshot taken when UP is first pressed; restored on DOWN to floor
    bool plain_nav = false;     // set true after accepting a hint with ?/End so the next UP ignores buf as filter
    std::wstring hint;          // gray ghost suffix after cursor; in nav mode it holds the suffix of the matching history entry

    bool tab_on = false;                    // true while Tab is being cycled; any non-Tab key resets this
    std::vector<std::wstring> tab_matches;  // all completions found on first Tab press
    int tab_idx   = 0;                      // which completion is currently shown
    int tab_start = 0;                      // start offset of the token being completed within buf
    std::wstring tab_pre;                   // buf content before the completion token; preserved across cycles
    std::wstring tab_suf;                   // buf content after cursor at Tab-press time; reappended each cycle

    std::wstring pending_save;              // history entry waiting to be written to disk after execution result is known

    std::wstring hint_cwd;                  // cwd used for cached filesystem hint matches
    std::wstring hint_dir;                  // directory prefix for cached filesystem hint matches
    bool hint_dirs_only = false;            // completion mode used for cached filesystem hint matches
    std::vector<std::wstring> hint_matches; // cached entries for the current hint directory
};

static void refresh_prompt(input& e) {
    std::string dir  = cwd();
    std::string name = folder(dir);
    std::string t    = cur_time();
    std::wstring git_root;
    std::string b    = branch(git_root);
    bool d           = b.empty() ? false : dirty(git_root);

    SetConsoleTitleA(name.c_str());

    auto p = make_prompt(g_prompt_elev, t, name, b, d, g_prompt_code);
    e.prompt_str = p.str;
    e.prompt_vis = p.vis;
}

struct token_info {
    std::wstring text;
    int start = 0;
};

static std::vector<token_info> split_tokens(const std::wstring& s, bool& trailing_space) {
    std::vector<token_info> out;
    std::wstring cur;
    int start = -1;
    wchar_t quote = 0;
    trailing_space = false;
    for (int i = 0; i < (int)s.size(); i++) {
        wchar_t c = s[i];
        if (quote) {
            if (c == quote) {
                quote = 0;
            } else {
                if (start == -1) start = i;
                cur += c;
            }
            trailing_space = false;
            continue;
        }
        if (c == L'"' || c == L'\'') {
            quote = c;
            if (start == -1) start = i + 1;
            trailing_space = false;
            continue;
        }
        if (c == L' ' || c == L'\t') {
            if (!cur.empty() || start != -1) {
                out.push_back({cur, start == -1 ? i : start});
                cur.clear();
                start = -1;
            }
            trailing_space = true;
            continue;
        }
        if (start == -1) start = i;
        cur += c;
        trailing_space = false;
    }
    if (!cur.empty() || start != -1)
        out.push_back({cur, start == -1 ? (int)s.size() : start});
    return out;
}

static bool yt_path_token(const std::wstring& s, int& start, std::wstring& token) {
    bool trailing_space = false;
    std::vector<token_info> parts = split_tokens(s, trailing_space);
    if (parts.size() < 2) return false;
    std::wstring cmd = parts[0].text;
    std::wstring mode = parts[1].text;
    std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::towlower);
    std::transform(mode.begin(), mode.end(), mode.begin(), ::towlower);
    if (cmd != L"yt" || (mode != L"mp3" && mode != L"mp4")) return false;
    if (parts.size() == 3 && trailing_space) {
        start = (int)s.size();
        token.clear();
        return true;
    }
    if (parts.size() == 4) {
        start = parts[3].start;
        token = parts[3].text;
        return true;
    }
    return false;
}

static void reset_hint_cache(input& e) {
    e.hint_cwd.clear();
    e.hint_dir.clear();
    e.hint_dirs_only = false;
    e.hint_matches.clear();
}

static bool hint_starts_with_ci(const std::wstring& text, const std::wstring& prefix) {
    if (prefix.size() > text.size()) return false;
    for (size_t i = 0; i < prefix.size(); i++)
        if (::towlower(text[i]) != ::towlower(prefix[i])) return false;
    return true;
}

static std::vector<std::wstring> complete_cached(input& e, const std::wstring& prefix, bool dirs_only = false) {
    std::wstring dir, name;
    size_t sep = prefix.find_last_of(L"\\/");
    if (sep == std::wstring::npos) { dir = L""; name = prefix; }
    else { dir = prefix.substr(0, sep + 1); name = prefix.substr(sep + 1); }

    std::wstring cur = to_wide(cwd());
    std::replace(cur.begin(), cur.end(), L'\\', L'/');

    if (e.hint_cwd != cur || e.hint_dir != dir || e.hint_dirs_only != dirs_only) {
        e.hint_matches = complete(dir, dirs_only);
        e.hint_cwd = cur;
        e.hint_dir = dir;
        e.hint_dirs_only = dirs_only;
    }

    if (name.empty()) return e.hint_matches;

    std::vector<std::wstring> out;
    out.reserve(e.hint_matches.size());
    for (const auto& full : e.hint_matches) {
        if (full.size() < dir.size()) continue;
        std::wstring tail = full.substr(dir.size());
        if (hint_starts_with_ci(tail, name))
            out.push_back(full);
    }
    return out;
}

static std::wstring find_git_hint(const input& e) {
    std::wstring buf = e.buf;
    std::wstring lower = buf;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    if (lower != L"git" && !(lower.size() >= 4 && lower.substr(0, 4) == L"git "))
        return L"";

    std::wstring rest = (buf.size() <= 4) ? L"" : buf.substr(4);
    if (rest.find(L' ') != std::wstring::npos || rest.find(L'\t') != std::wstring::npos)
        return L"";

    static const std::vector<std::wstring> git_commands = {
        L"status", L"commit", L"add", L"push", L"pull", L"fetch", L"switch", L"checkout",
        L"branch", L"merge", L"rebase", L"diff", L"log", L"restore", L"reset", L"stash",
        L"clone", L"init", L"remote", L"show", L"tag", L"blame", L"grep", L"bisect"
    };

    std::wstring rest_lower = rest;
    std::transform(rest_lower.begin(), rest_lower.end(), rest_lower.begin(), ::towlower);
    for (const std::wstring& cmd : git_commands) {
        if (!hint_starts_with_ci(cmd, rest_lower))
            continue;
        if (buf.size() == 3)
            return L" " + cmd;
        if (rest.empty())
            return cmd;
        if (cmd.size() > rest.size())
            return cmd.substr(rest.size());
        return L"";
    }

    return L"";
}

// Calculates the ghost hint for the current buf in edit mode (hist_idx == -1).
// For "cd <path>" uses filesystem completions (dirs only); for "ls <path>" files+dirs;
// for bare paths (X:/, ./, ../) uses filesystem completions directly;
// for everything else scans history backwards so the most recent matching command wins.
void find_hint(input& e) {
    e.hint.clear();
    if (e.buf.empty() || e.hist_idx != -1) return;
    if (e.buf.find(L'\n') != std::wstring::npos) return; // no hint in multiline mode
    std::wstring lower = e.buf;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    if (lower == L"git" || (lower.size() >= 4 && lower.substr(0, 4) == L"git ")) {
        e.hint = find_git_hint(e);
        return;
    }
    if (lower == L"cd" || (lower.size() >= 3 && lower.substr(0, 3) == L"cd ")) {
        if (lower.size() >= 3) {
            std::wstring token = e.buf.substr(3);
            auto matches = complete_cached(e, token, true);
            if (!matches.empty() && matches[0].size() > token.size())
                e.hint = matches[0].substr(token.size());
        }
        return;
    }
    if (lower.size() >= 3 && lower.substr(0, 3) == L"ls ") {
        std::wstring token = e.buf.substr(3);
        auto matches = complete_cached(e, token, true);
        if (!matches.empty() && matches[0].size() > token.size())
            e.hint = matches[0].substr(token.size());
        return;
    }
    if (lower.size() >= 4 && lower.substr(0, 4) == L"cat ") {
        std::wstring token = e.buf.substr(4);
        auto matches = complete_cached(e, token, false);
        if (!matches.empty() && matches[0].size() > token.size())
            e.hint = matches[0].substr(token.size());
        return;
    }
    if (lower.size() >= 4 && lower.substr(0, 4) == L"img ") {
        std::wstring token = e.buf.substr(4);
        auto matches = complete_cached(e, token, false);
        if (!matches.empty() && matches[0].size() > token.size())
            e.hint = matches[0].substr(token.size());
        return;
    }
    if (lower.size() >= 5 && lower.substr(0, 5) == L"edit ") {
        std::wstring token = e.buf.substr(5);
        auto matches = complete_cached(e, token, false);
        if (!matches.empty() && matches[0].size() > token.size())
            e.hint = matches[0].substr(token.size());
        return;
    }
    if (lower.size() >= 5 && lower.substr(0, 5) == L"play ") {
        int cmd_len = 5;
        std::wstring token = e.buf.substr(cmd_len);
        std::wstring token_lower = token;
        std::transform(token_lower.begin(), token_lower.end(), token_lower.begin(), ::towlower);
        if (token_lower == L"pause" || token_lower == L"resume" || token_lower == L"stop" ||
            token_lower == L"status" || token_lower == L"ui" ||
            token_lower.substr(0, 4) == L"vol ") {
            return;
        }
        auto matches = complete_cached(e, token, false);
        if (!matches.empty() && matches[0].size() > token.size())
            e.hint = matches[0].substr(token.size());
        return;
    }
    if (lower.size() >= 5 && lower.substr(0, 5) == L"json ") {
        std::wstring token = e.buf.substr(5);
        auto matches = complete_cached(e, token, false);
        if (!matches.empty() && matches[0].size() > token.size())
            e.hint = matches[0].substr(token.size());
        return;
    }
    if (lower.size() >= 5 && lower.substr(0, 5) == L"clip ") {
        std::wstring token = e.buf.substr(5);
        auto matches = complete_cached(e, token, false);
        if (!matches.empty() && matches[0].size() > token.size())
            e.hint = matches[0].substr(token.size());
        return;
    }
    {
        int start = 0;
        std::wstring token;
        if (yt_path_token(e.buf, start, token)) {
            auto matches = complete_cached(e, token, true);
            if (!matches.empty() && matches[0].size() > token.size())
                e.hint = matches[0].substr(token.size());
            return;
        }
    }
    // bare path prefixes: X:/ or ./ or ../
    bool is_path = (e.buf.size() >= 3 && iswalpha(e.buf[0]) && e.buf[1] == L':' && e.buf[2] == L'/') ||
                   (e.buf.size() >= 2 && e.buf[0] == L'.' && e.buf[1] == L'/') ||
                   (e.buf.size() >= 3 && e.buf[0] == L'.' && e.buf[1] == L'.' && e.buf[2] == L'/');
    if (is_path) {
        auto matches = complete_cached(e, e.buf, false);
        if (!matches.empty() && matches[0].size() > e.buf.size())
            e.hint = matches[0].substr(e.buf.size());
        return;
    }
    for (int i = (int)e.hist.size() - 1; i >= 0; i--) {
        if (e.hist[i].find(L'\n') != std::wstring::npos) continue; // skip multiline entries
        if (e.hist[i].size() > e.buf.size() &&
            e.hist[i].substr(0, e.buf.size()) == e.buf) {
            e.hint = e.hist[i].substr(e.buf.size());
            return;
        }
    }
}

// ---- multiline helpers ----
// All helpers treat \n embedded in buf as logical line separators.
// Continuation rows are rendered with a 2-char "  " prefix by redraw().

// Logical row index (0-based) at flat buffer offset p.
static int row_of(const std::wstring& buf, int p) {
    int r = 0;
    for (int i = 0; i < p && i < (int)buf.size(); i++)
        if (buf[i] == L'\n') r++;
    return r;
}

// Column within the current logical row at flat offset p (0-based).
static int col_of(const std::wstring& buf, int p) {
    int c = 0;
    for (int i = 0; i < p && i < (int)buf.size();) {
        if (buf[i] == L'\n') {
            c = 0;
            i++;
        } else {
            int units = 0;
            uint32_t cp = ui_codepoint_at(buf, i, &units);
            if (units <= 0) break;
            c += ui_char_width(cp);
            i += units;
        }
    }
    return c;
}

// Flat offset of the first character of logical row r.
static int row_start(const std::wstring& buf, int r) {
    if (r == 0) return 0;
    int cur = 0;
    for (int i = 0; i < (int)buf.size(); i++)
        if (buf[i] == L'\n' && ++cur == r) return i + 1;
    return (int)buf.size();
}

// Total logical row count (always >= 1).
static int row_count(const std::wstring& buf) {
    int n = 1;
    for (wchar_t c : buf) if (c == L'\n') n++;
    return n;
}

// Physical screen rows from the start of input down to flat offset p.
static int phys_rows(const std::wstring& buf, int p, int prompt_vis, int width) {
    int rows = 0, col = prompt_vis;
    for (int i = 0; i < p && i < (int)buf.size();) {
        if (buf[i] == L'\n') {
            rows++;
            col = 2;
            i++;
        } else {
            int units = 0;
            uint32_t cp = ui_codepoint_at(buf, i, &units);
            if (units <= 0) break;
            int cw = std::max(1, ui_char_width(cp));
            if (col + cw > width) {
                rows++;
                col = 0;
            }
            col += cw;
            if (col >= width) {
                rows++;
                col = 0;
            }
            i += units;
        }
    }
    return rows;
}

// Physical screen column (0-based) at flat offset p. Continuation rows start at col 2.
static int phys_col(const std::wstring& buf, int p, int prompt_vis, int width) {
    int col = prompt_vis;
    for (int i = 0; i < p && i < (int)buf.size();) {
        if (buf[i] == L'\n') {
            col = 2;
            i++;
        } else {
            int units = 0;
            uint32_t cp = ui_codepoint_at(buf, i, &units);
            if (units <= 0) break;
            int cw = std::max(1, ui_char_width(cp));
            if (col + cw > width)
                col = 0;
            col += cw;
            if (col >= width)
                col = 0;
            i += units;
        }
    }
    return col;
}

static std::wstring shown_hint(const input& e) {
    if (e.hint.empty() || e.buf.find(L'\n') != std::wstring::npos)
        return L"";
    int width = term_width();
    int end_col = phys_col(e.buf, (int)e.buf.size(), e.prompt_vis, width);
    int remaining = width - end_col;
    if (remaining <= 1)
        return L"";
    int keep = 0;
    int used = 0;
    for (int i = 0; i < (int)e.hint.size();) {
        int units = 0;
        uint32_t cp = ui_codepoint_at(e.hint, i, &units);
        if (units <= 0) break;
        int cw = std::max(1, ui_char_width(cp));
        if (used + cw > remaining - 1)
            break;
        used += cw;
        i += units;
        keep = i;
    }
    return e.hint.substr(0, keep);
}

// Redraws the entire input line in place: moves up to the prompt row, clears to end of screen,
// reprints prompt + buf + gray hint, then repositions the cursor at e.pos.
// Handles multiline buf (embedded \n) with 2-space continuation indent. Batches output to avoid flicker.
void redraw(input& e) {
    int width   = term_width();
    int cur_row = e.cursor_row;

    int max_up = term_height() - 1;
    if (cur_row > max_up) cur_row = max_up;

    std::string s;
    if (cur_row > 0) {
        char esc[32];
        snprintf(esc, sizeof(esc), "\x1b[%dA", cur_row);
        s += esc;
    }
    s += "\r\x1b[J";   // col 0 of prompt row, clear to end of screen
    s += e.prompt_str;

    // Render buf: continuation markers (\ ^) go gray; \n becomes \r\n + 2-space indent
    std::string buf_utf8 = to_utf8(e.buf);
    for (size_t i = 0; i < buf_utf8.size(); i++) {
        char c = buf_utf8[i];
        if (c == '\n') {
            s += "\r\n  ";
        } else if ((c == '\\' || c == '^') && i + 1 < buf_utf8.size() && buf_utf8[i + 1] == '\n') {
            s += GRAY; s += c; s += RESET;
        } else {
            s += c;
        }
    }

    // Gray hint - only shown in single-line mode to avoid complexity
    std::wstring hint = shown_hint(e);
    if (!hint.empty())
        s += GRAY + to_utf8(hint) + RESET;

    // Position cursor at e.pos
    int total_rows = phys_rows(e.buf, (int)e.buf.size(), e.prompt_vis, width);
    int pos_row    = phys_rows(e.buf, e.pos, e.prompt_vis, width);
    int pos_col    = phys_col(e.buf, e.pos, e.prompt_vis, width);
    int rows_up    = total_rows - pos_row;
    if (rows_up > 0) {
        char esc[32];
        snprintf(esc, sizeof(esc), "\x1b[%dA", rows_up);
        s += esc;
    }
    char col_esc[32];
    snprintf(col_esc, sizeof(col_esc), "\x1b[%dG", pos_col + 1);
    s += col_esc;

    out(s);
    e.prev_pos   = e.pos;
    e.cursor_row = pos_row;
}

static bool fast_backspace_paint(input& e, const std::wstring& old_buf) {
    if (old_buf.find(L'\n') != std::wstring::npos || e.buf.find(L'\n') != std::wstring::npos)
        return false;
    if (ui_text_width(old_buf) != (int)old_buf.size() || ui_text_width(e.buf) != (int)e.buf.size())
        return false;

    int width = term_width();
    if (phys_rows(old_buf, (int)old_buf.size(), e.prompt_vis, width) != 0)
        return false;
    if (phys_rows(e.buf, (int)e.buf.size(), e.prompt_vis, width) != 0)
        return false;

    std::wstring hint = shown_hint(e);
    std::string s = "\b\x1b[J";
    if (!hint.empty()) {
        s += GRAY + to_utf8(hint) + RESET;
        char esc[32];
        snprintf(esc, sizeof(esc), "\x1b[%dD", ui_text_width(hint));
        s += esc;
    }
    out(s);
    e.prev_pos = e.pos;
    e.cursor_row = 0;
    return true;
}

// Inserts clipboard text at the current cursor position.
// Normalises \r\n / lone \r to \n, strips trailing newlines.
// If every non-last segment ends with a \ or ^ continuation marker, strips and joins into one line.
static void paste_text(input& e, const wchar_t* raw) {
    std::wstring text;
    for (const wchar_t* p = raw; *p; p++) {
        if (*p == L'\r') {
            text += L'\n';
            if (*(p + 1) == L'\n') p++;
        } else {
            text += *p;
        }
    }
    while (!text.empty() && text.back() == L'\n') text.pop_back();
    if (text.empty()) return;

    if (text.find(L'\n') != std::wstring::npos) {
        bool all_cont = true;
        size_t pos = 0;
        while (pos < text.size()) {
            size_t nl = text.find(L'\n', pos);
            if (nl == std::wstring::npos) break;
            size_t e2 = nl;
            while (e2 > pos && text[e2 - 1] == L' ') e2--;
            if (e2 == pos || (text[e2 - 1] != L'\\' && text[e2 - 1] != L'^'))
                { all_cont = false; break; }
            pos = nl + 1;
        }
        if (all_cont) {
            std::wstring joined;
            pos = 0;
            while (pos <= text.size()) {
                size_t nl = text.find(L'\n', pos);
                std::wstring seg = (nl == std::wstring::npos) ? text.substr(pos) : text.substr(pos, nl - pos);
                while (!seg.empty() && seg.back() == L' ') seg.pop_back();
                if (!seg.empty() && (seg.back() == L'\\' || seg.back() == L'^')) {
                    seg.pop_back();
                    while (!seg.empty() && seg.back() == L' ') seg.pop_back();
                }
                if (!joined.empty() && !seg.empty()) joined += L' ';
                joined += seg;
                if (nl == std::wstring::npos) break;
                pos = nl + 1;
            }
            text = joined;
        }
    }

    e.hist_idx  = -1;
    e.plain_nav = false;
    e.hint.clear();
    e.buf.insert(e.pos, text);
    e.pos += (int)text.size();
    find_hint(e);
    redraw(e);
}

// Reads clipboard and passes it to paste_text().
// Flushes the console input queue before and after to discard ConHost's async-injected KEY_EVENTs.
static void do_paste(input& e) {
    FlushConsoleInputBuffer(in_h);
    Sleep(5);
    FlushConsoleInputBuffer(in_h);
    std::wstring text = clipboard_get();
    if (!text.empty()) paste_text(e, text.c_str());
    FlushConsoleInputBuffer(in_h);
}

// Advances history navigation one step in direction dir (-1 = UP/older, +1 = DOWN/newer).
// No wrap-around: UP stops at the oldest entry, DOWN past the newest restores the pre-nav draft.
static void nav_step(input& e, int dir) {
    int n = (int)e.hist.size();
    if (n == 0) return;

    if (dir == -1) {  // UP ? older
        if (e.hist_idx == 0) return;
        int start = (e.hist_idx == n) ? n - 1 : e.hist_idx - 1;
        if (e.saved.empty()) {
            e.hist_idx = start;
            e.buf = e.hist[start]; e.hint.clear();
        } else {
            int found = -1;
            for (int i = start; i >= 0; i--)
                if (e.hist[i].size() >= e.saved.size() &&
                    e.hist[i].substr(0, e.saved.size()) == e.saved) { found = i; break; }
            if (found == -1) return;
            e.hist_idx = found;
            if (e.hist[found].find(L'\n') != std::wstring::npos) {
                e.buf = e.hist[found]; e.hint.clear(); // multiline: show as-is, no hint split
            } else {
                e.buf  = e.saved;
                e.hint = e.hist[found].substr(e.saved.size());
            }
        }
    } else {  // DOWN ? newer
        int start = e.hist_idx + 1;
        auto restore_draft = [&]() {
            e.buf = e.draft; e.hint.clear();
            e.hist_idx = -1; e.saved.clear(); e.draft.clear(); e.plain_nav = false;
            e.pos = (int)e.buf.size(); redraw(e);
        };
        if (start >= n) { restore_draft(); return; }
        if (e.saved.empty()) {
            e.hist_idx = start;
            e.buf = e.hist[start]; e.hint.clear();
        } else {
            int found = -1;
            for (int i = start; i < n; i++)
                if (e.hist[i].size() >= e.saved.size() &&
                    e.hist[i].substr(0, e.saved.size()) == e.saved) { found = i; break; }
            if (found == -1) return;
            e.hist_idx = found;
            if (e.hist[found].find(L'\n') != std::wstring::npos) {
                e.buf = e.hist[found]; e.hint.clear(); // multiline: show as-is, no hint split
            } else {
                e.buf  = e.saved;
                e.hint = e.hist[found].substr(e.saved.size());
            }
        }
    }
    e.pos = (int)e.buf.size();
    redraw(e);
}

// Transitions into history-nav mode on the first UP key press.
static void enter_nav(input& e) {
    e.draft = e.buf;
    e.saved = e.plain_nav ? L"" : e.buf;
    if (!e.saved.empty()) {
        bool any = false;
        for (auto& h : e.hist)
            if (h.size() >= e.saved.size() &&
                h.substr(0, e.saved.size()) == e.saved) { any = true; break; }
        if (!any) e.saved.clear();
    }
    e.hist_idx = (int)e.hist.size();
    if (!e.hint.empty() && !e.saved.empty()) {
        std::wstring full = e.buf + e.hint;
        for (int i = (int)e.hist.size() - 1; i >= 0; i--)
            if (e.hist[i] == full) { e.hist_idx = i; break; }
    }
}

// Raw input loop: reads INPUT_RECORDs and drives the full input state machine.
// Returns the completed command on Enter, or empty string on Ctrl+C.
std::string readline(input& e) {
    while (true) {
        INPUT_RECORD ir;
        DWORD count;
        if (!ReadConsoleInputW(in_h, &ir, 1, &count)) break;

        if (ir.EventType == MOUSE_EVENT) {
            auto& me = ir.Event.MouseEvent;
            if ((me.dwButtonState & RIGHTMOST_BUTTON_PRESSED) && me.dwEventFlags == 0) {
                CONSOLE_SELECTION_INFO sel = {};
                GetConsoleSelectionInfo(&sel);
                bool has_sel = (sel.dwFlags & CONSOLE_SELECTION_IN_PROGRESS) &&
                               (sel.dwFlags & CONSOLE_MOUSE_SELECTION);
                if (!has_sel) do_paste(e);
            }
            continue;
        }
        if (ir.EventType != KEY_EVENT || !ir.Event.KeyEvent.bKeyDown) continue;

        WORD vk     = ir.Event.KeyEvent.wVirtualKeyCode;
        wchar_t ch  = ir.Event.KeyEvent.uChar.UnicodeChar;
        DWORD state = ir.Event.KeyEvent.dwControlKeyState;
        bool ctrl   = (state & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;

        if (vk != VK_TAB) e.tab_on = false;

        if (vk == VK_RETURN) {
            size_t last_nl = e.buf.rfind(L'\n');
            std::wstring cur_line = (last_nl == std::wstring::npos)
                                    ? e.buf
                                    : e.buf.substr(last_nl + 1);
            std::wstring trimmed = cur_line;
            while (!trimmed.empty() && trimmed.back() == L' ') trimmed.pop_back();
            if (!trimmed.empty() && (trimmed.back() == L'\\' || trimmed.back() == L'^')) {
                e.hint.clear();
                e.buf += L'\n';
                e.pos = (int)e.buf.size();
                redraw(e);
                continue;
            }

            if (e.hist_idx != -1 && !e.hint.empty()) e.buf += e.hint;
            e.hint.clear(); e.pos = (int)e.buf.size(); redraw(e);
            out("\r\n");

            std::wstring full;
            if (e.buf.find(L'\n') != std::wstring::npos) {
                std::wstring seg;
                for (size_t i = 0; i <= e.buf.size(); i++) {
                    if (i == e.buf.size() || e.buf[i] == L'\n') {
                        while (!seg.empty() && seg.back() == L' ') seg.pop_back();
                        if (!seg.empty() && (seg.back() == L'\\' || seg.back() == L'^'))
                            seg.pop_back();
                        while (!seg.empty() && seg.back() == L' ') seg.pop_back();
                        if (!full.empty()) full += L' ';
                        full += seg;
                        seg.clear();
                    } else {
                        seg += e.buf[i];
                    }
                }
            } else {
                full = e.buf;
            }

            std::string line = to_utf8(full);
            if (!full.empty()) {
                // Store the display form (e.buf, multiline preserved) not the joined form.
                // e.buf still has \n at this point; e.buf.clear() happens below.
                std::wstring hist_entry = e.buf;
                e.hist.erase(std::remove(e.hist.begin(), e.hist.end(), hist_entry), e.hist.end());
                e.hist.push_back(hist_entry);
                e.pending_save = hist_entry;
            }
            e.buf.clear();
            e.pos       = 0;
            e.hist_idx  = -1;
            e.saved.clear();
            e.plain_nav = false;
            return line;
        }

        if (vk == VK_BACK) {
            if (e.pos > 0) {
                std::wstring old_buf = e.buf;
                std::wstring old_hint = e.hint;
                bool used_hist_hint = (e.hist_idx != -1 && !e.hint.empty());
                if (used_hist_hint) { e.buf += e.hint; }
                bool backspace_at_end = (e.pos == (int)e.buf.size());
                e.hist_idx = -1; e.plain_nav = false;
                e.buf.erase(e.pos - 1, 1); e.pos--;
                bool reuse_hint = !used_hist_hint &&
                                  backspace_at_end &&
                                  old_buf.find(L'\n') == std::wstring::npos &&
                                  !old_hint.empty();
                if (reuse_hint) {
                    std::wstring old_full = old_buf + old_hint;
                    if (old_full.size() > e.buf.size() && old_full.substr(0, e.buf.size()) == e.buf)
                        e.hint = old_full.substr(e.buf.size());
                    else
                        find_hint(e);
                } else {
                    find_hint(e);
                }
                if (!backspace_at_end || !fast_backspace_paint(e, old_buf))
                    redraw(e);
            }
            continue;
        }

        if (vk == VK_DELETE) {
            if (e.pos < (int)e.buf.size()) {
                if (e.hist_idx != -1 && !e.hint.empty()) { e.buf += e.hint; }
                e.hist_idx = -1; e.plain_nav = false;
                e.buf.erase(e.pos, 1); find_hint(e); redraw(e);
            }
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
        if (vk == VK_HOME) {
            if (e.buf.find(L'\n') != std::wstring::npos)
                e.pos = row_start(e.buf, row_of(e.buf, e.pos));
            else
                e.pos = 0;
            redraw(e); continue;
        }
        if (vk == VK_END) {
            if (e.buf.find(L'\n') != std::wstring::npos) {
                int rs = row_start(e.buf, row_of(e.buf, e.pos));
                size_t nl = e.buf.find(L'\n', rs);
                e.pos = (nl != std::wstring::npos) ? (int)nl : (int)e.buf.size();
            } else {
                if (e.pos == (int)e.buf.size() && !e.hint.empty()) {
                    e.buf += e.hint;
                    e.hint.clear();
                    e.hist_idx  = -1;
                    e.saved.clear();
                    e.plain_nav = true;
                }
                e.pos = (int)e.buf.size();
            }
            redraw(e); continue;
        }

        if (vk == VK_UP) {
            if (e.buf.find(L'\n') != std::wstring::npos && row_of(e.buf, e.pos) > 0) {
                int r  = row_of(e.buf, e.pos);
                int c  = col_of(e.buf, e.pos);
                int rs = row_start(e.buf, r - 1);
                size_t nl = e.buf.find(L'\n', rs);
                int prev_len = (nl != std::wstring::npos) ? (int)(nl - rs) : (int)(e.buf.size() - rs);
                e.pos = rs + std::min(c, prev_len);
                redraw(e); continue;
            }
            if (e.hist.empty()) continue;
            if (e.hist_idx == -1) enter_nav(e);
            nav_step(e, -1);
            continue;
        }

        if (vk == VK_DOWN) {
            int rcount = row_count(e.buf);
            if (e.buf.find(L'\n') != std::wstring::npos && row_of(e.buf, e.pos) < rcount - 1) {
                int r  = row_of(e.buf, e.pos);
                int c  = col_of(e.buf, e.pos);
                int rs = row_start(e.buf, r + 1);
                size_t nl = e.buf.find(L'\n', rs);
                int next_len = (nl != std::wstring::npos) ? (int)(nl - rs) : (int)(e.buf.size() - rs);
                e.pos = rs + std::min(c, next_len);
                redraw(e); continue;
            }
            if (e.hist_idx == -1) continue;
            nav_step(e, +1);
            continue;
        }

        if (vk == VK_TAB) {
            if (!e.tab_on) {
                std::wstring before = e.buf.substr(0, e.pos);
                std::wstring lower_buf = e.buf;
                std::transform(lower_buf.begin(), lower_buf.end(), lower_buf.begin(), ::towlower);
                size_t space = before.find_last_of(L" \t");
                int start = (space == std::wstring::npos) ? 0 : (int)space + 1;
                std::wstring token = before.substr(start);
                bool dirs_only = (lower_buf.substr(0, 3) == L"cd " || lower_buf == L"cd" ||
                                  lower_buf.substr(0, 3) == L"ls " || lower_buf == L"ls");
                if (lower_buf.substr(0, 5) == L"play ") {
                    int cmd_len = 5;
                    std::wstring play_arg = before.substr(cmd_len);
                    std::wstring play_lower = play_arg;
                    std::transform(play_lower.begin(), play_lower.end(), play_lower.begin(), ::towlower);
                    if (play_lower == L"pause" || play_lower == L"resume" || play_lower == L"stop" ||
                        play_lower == L"status" || play_lower == L"ui" ||
                        play_lower.substr(0, 4) == L"vol ")
                        continue;
                }
                if (yt_path_token(before, start, token))
                    dirs_only = true;
                e.tab_matches = complete(token, dirs_only);
                if (e.tab_matches.empty()) continue;
                e.tab_on    = true;
                e.tab_idx   = 0;
                e.tab_start = start;
                e.tab_pre   = e.buf.substr(0, start);
                e.tab_suf   = e.buf.substr(e.pos);
            } else {
                std::wstring cur = e.tab_matches[e.tab_idx];
                if (e.tab_matches.size() == 1 && !cur.empty() && cur.back() == L'/') {
                    std::wstring lower_buf = e.buf;
                    std::transform(lower_buf.begin(), lower_buf.end(), lower_buf.begin(), ::towlower);
                    bool dirs_only = (lower_buf.substr(0, 3) == L"cd " || lower_buf == L"cd" ||
                                      lower_buf.substr(0, 3) == L"ls " || lower_buf == L"ls");
                    std::wstring path_token = cur;
                    std::wstring before = e.tab_pre + cur;
                    int yt_start = 0;
                    std::wstring yt_token;
                    if (yt_path_token(before, yt_start, yt_token)) {
                        dirs_only = true;
                        path_token = yt_token;
                    }
                    std::wstring new_token = e.tab_pre + cur;
                    size_t space = new_token.find_last_of(L" \t");
                    if (!dirs_only || !yt_path_token(before, yt_start, yt_token))
                        path_token = (space == std::wstring::npos) ? new_token : new_token.substr(space + 1);
                    auto matches = complete(path_token, dirs_only);
                    if (!matches.empty()) {
                        e.tab_matches = matches;
                        e.tab_idx     = 0;
                        e.tab_pre     = (space == std::wstring::npos) ? L"" : new_token.substr(0, space + 1);
                        e.tab_suf     = L"";
                    } else {
                        e.tab_idx = 0;
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

        if (vk == VK_ESCAPE) {
            e.buf.clear(); e.pos = 0; e.hint.clear(); e.hist_idx = -1; e.saved.clear(); e.draft.clear();
            redraw(e);
            continue;
        }

        if (ctrl && vk == 'C') {
            out("^C\r\n");
            e.buf.clear();
            e.pos = 0;
            return "";
        }

        if ((ctrl && vk == 'O') || vk == VK_F10) {
            explore_toggle();
            e.tab_on = false;
            reset_hint_cache(e);
            find_hint(e);
            refresh_prompt(e);
            redraw(e);
            continue;
        }

        {
            bool shift = (state & SHIFT_PRESSED) != 0;
            if ((ctrl && vk == 'V') || (vk == VK_INSERT && shift)) {
                do_paste(e);
                continue;
            }
        }

        if (ch >= 32 && ch != 127) {
            bool typed_at_end = (e.pos == (int)e.buf.size());
            bool had_nav_hint = (e.hist_idx != -1 || !e.hint.empty());
            if (e.hist_idx != -1 && !e.hint.empty()) { e.buf += e.hint; }
            e.hist_idx = -1;
            e.plain_nav = false;
            e.buf.insert(e.pos, 1, ch);
            e.pos++;
            find_hint(e);
            DWORD nevents = 0;
            GetNumberOfConsoleInputEvents(in_h, &nevents);
            // Single-char paint is only safe for plain end-of-line typing.
            // Mid-line edits or hint/nav transitions need a full redraw or some hosts
            // (notably VSCode/ConPTY) can look like overtype mode.
            bool fast_paint = typed_at_end && !had_nav_hint && e.hint.empty();
            if (fast_paint && nevents > 0) {
                e.cursor_row = phys_rows(e.buf, e.pos, e.prompt_vis, term_width());
                out(to_utf8(std::wstring(1, ch)));
            } else {
                redraw(e);
            }
        }
    }
    return "";
}

