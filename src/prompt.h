// MODULE: prompt
// Purpose : prompt string builder — assembles colored [time]folder[branch*][code]> with visual width
// Exports : struct prompt_t | make_prompt()
// Depends : common.h

// Prompt string paired with its visual width (ANSI escape codes excluded).
// vis is needed by redraw to correctly compute screen column positions.
struct prompt_t {
    std::string str; // full ANSI string
    int vis;         // printable character count (no escape codes)
};

// Current shell prompt context, refreshed by main before each readline() call.
static bool g_prompt_elev = false;
static int  g_prompt_code = 0;

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
