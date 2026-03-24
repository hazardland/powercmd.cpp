// pcmd_test.cpp — automated tests for pcmd editor state machine.
// Compile: g++ pcmd_test.cpp -o pcmd_test.exe -DVERSION_MINOR=0 -ladvapi32 -lshell32
// Run:     pcmd_test.exe

#define PCMD_TEST

#include <string>
#include <cstdio>

// Stub out/err — tests run without a real console; output is captured but ignored.
static std::string g_output;
void out(const std::string& s) { g_output += s; }
void err(const std::string& s) {}

#include "pcmd.cpp"

// ---- minimal test framework ----

static int pass_count = 0, fail_count = 0;

static void check(bool cond, const char* name) {
    if (cond) { printf("  PASS  %s\n", name); pass_count++; }
    else       { printf("  FAIL  %s\n", name); fail_count++; }
}

static editor make_editor(std::vector<std::wstring> hist, std::wstring buf = L"") {
    editor e;
    e.hist = std::move(hist);
    e.buf  = std::move(buf);
    e.pos  = (int)e.buf.size();
    return e;
}

// ---- tests ----

static void test_find_hint() {
    printf("\nfind_hint\n");

    {   // empty buf -> no hint
        auto e = make_editor({L"ping google.com"});
        find_hint(e);
        check(e.hint.empty(), "empty buf -> no hint");
    }
    {   // buf is prefix of history entry -> hint is suffix
        auto e = make_editor({L"ping google.com"}, L"ping ");
        find_hint(e);
        check(e.hint == L"google.com", "prefix match -> hint is suffix");
    }
    {   // most recent entry wins when multiple share prefix
        auto e = make_editor({L"ping google.com", L"ping github.com"}, L"ping ");
        find_hint(e);
        check(e.hint == L"github.com", "most recent match wins");
    }
    {   // exact match (entry == buf, not longer) -> no hint
        auto e = make_editor({L"ping"}, L"ping");
        find_hint(e);
        check(e.hint.empty(), "exact match -> no hint");
    }
    {   // nav mode (hist_idx != -1) -> no hint, returns early
        auto e = make_editor({L"ping google.com"}, L"ping ");
        e.hist_idx = 0;
        find_hint(e);
        check(e.hint.empty(), "nav mode -> no hint");
    }
    {   // no history entry matches prefix -> no hint
        auto e = make_editor({L"ls", L"pwd"}, L"ping ");
        find_hint(e);
        check(e.hint.empty(), "no matching prefix -> no hint");
    }
    {   // older entry does not shadow a newer non-matching one
        auto e = make_editor({L"ping google.com", L"ls"}, L"ping ");
        find_hint(e);
        // backwards scan hits "ls" first (no match), then "ping google.com"
        check(e.hint == L"google.com", "older entry found when newer doesn't match");
    }
}

static void test_nav_step() {
    printf("\nnav_step\n");

    {   // plain UP from past-end -> last entry
        auto e = make_editor({L"a", L"b", L"c"});
        e.hist_idx = 3;
        nav_step(e, -1);
        check(e.buf == L"c" && e.hist_idx == 2, "plain UP from end -> last entry");
    }
    {   // plain UP wraps around from index 0
        auto e = make_editor({L"a", L"b", L"c"});
        e.hist_idx = 0;
        nav_step(e, -1);
        check(e.buf == L"c" && e.hist_idx == 2, "plain UP wraps around");
    }
    {   // plain DOWN from index 0 -> second entry
        auto e = make_editor({L"a", L"b", L"c"});
        e.hist_idx = 0;
        nav_step(e, +1);
        check(e.buf == L"b" && e.hist_idx == 1, "plain DOWN steps forward");
    }
    {   // plain DOWN wraps around from last index
        auto e = make_editor({L"a", L"b", L"c"});
        e.hist_idx = 2;
        nav_step(e, +1);
        check(e.buf == L"a" && e.hist_idx == 0, "plain DOWN wraps around");
    }
    {   // filtered: finds most recent match (hint split)
        auto e = make_editor({L"ping 8.8.8.8", L"ls", L"ping google.com"});
        e.saved    = L"ping ";
        e.hist_idx = 3; // past-end
        nav_step(e, -1);
        check(e.buf == L"ping " && e.hint == L"google.com" && e.hist_idx == 2,
              "filtered UP -> most recent match, hint split");
    }
    {   // filtered: stepping again reaches earlier match
        auto e = make_editor({L"ping 8.8.8.8", L"ls", L"ping google.com"});
        e.saved    = L"ping ";
        e.hist_idx = 2; // at "ping google.com"
        nav_step(e, -1);
        check(e.buf == L"ping " && e.hint == L"8.8.8.8" && e.hist_idx == 0,
              "filtered UP -> earlier match");
    }
    {   // filtered: wrap-around from earliest match back to latest
        auto e = make_editor({L"ping 8.8.8.8", L"ls", L"ping google.com"});
        e.saved    = L"ping ";
        e.hist_idx = 0; // at earliest match, step UP should wrap to latest
        nav_step(e, -1);
        check(e.buf == L"ping " && e.hint == L"google.com" && e.hist_idx == 2,
              "filtered UP wraps to latest match");
    }
    {   // filtered: no match at all -> falls back to plain cycle
        auto e = make_editor({L"ls", L"pwd"});
        e.saved    = L"ping ";
        e.hist_idx = 2; // past-end
        nav_step(e, -1);
        check(e.buf == L"pwd" && e.hint.empty(), "filtered fallback to plain when no match");
    }
    {   // plain nav shows full entry, no hint split
        auto e = make_editor({L"ping google.com", L"ls"});
        e.hist_idx = 2; // past-end, saved empty = plain
        nav_step(e, -1);
        check(e.buf == L"ls" && e.hint.empty(), "plain nav: full entry in buf, no hint");
    }
}

static void test_enter_nav() {
    printf("\nenter_nav\n");

    {   // normal: snapshots buf as saved, hist_idx set to size
        auto e = make_editor({L"ping google.com", L"ls"}, L"ping ");
        enter_nav(e);
        check(e.saved == L"ping " && e.hist_idx == 2,
              "enter_nav: snapshots buf as saved, hist_idx=size");
    }
    {   // hint visible -> anchors hist_idx at the hinted entry
        auto e = make_editor({L"ping google.com", L"ls"}, L"ping ");
        e.hint = L"google.com"; // "ping google.com" is at index 0
        enter_nav(e);
        check(e.hist_idx == 0, "enter_nav: anchors at hinted entry");
    }
    {   // plain_nav=true -> saved="" so next nav is unfiltered
        auto e = make_editor({L"ping google.com", L"ls"}, L"ping ");
        e.plain_nav = true;
        enter_nav(e);
        check(e.saved.empty(), "enter_nav: plain_nav -> saved is empty");
    }
    {   // hint present but plain_nav -> saved stays empty, no anchoring
        auto e = make_editor({L"ping google.com", L"ls"}, L"ping ");
        e.hint      = L"google.com";
        e.plain_nav = true;
        enter_nav(e);
        check(e.saved.empty() && e.hist_idx == 2,
              "enter_nav: plain_nav with hint -> saved empty, no anchoring");
    }
}

static void test_enter_nav_then_nav_step() {
    printf("\nenter_nav + nav_step (UP skip-current-hint)\n");

    {   // UP when hint already shows most-recent match must immediately move to PREV entry
        auto e = make_editor({L"ping 8.8.8.8", L"ls", L"ping google.com"}, L"ping ");
        e.hint = L"google.com"; // hint already showing "ping google.com"
        enter_nav(e);           // anchors at index 2
        nav_step(e, -1);        // must step past it -> "ping 8.8.8.8" at index 0
        check(e.buf == L"ping " && e.hint == L"8.8.8.8",
              "UP with visible hint skips to prev entry immediately");
    }
    {   // UP with no hint starts from most-recent matching entry
        auto e = make_editor({L"ping 8.8.8.8", L"ls", L"ping google.com"}, L"ping ");
        enter_nav(e);    // hist_idx=3 (past-end), saved="ping "
        nav_step(e, -1); // should land on "ping google.com"
        check(e.buf == L"ping " && e.hint == L"google.com",
              "UP with no hint lands on most-recent match");
    }
}

static void test_accept_hint() {
    printf("\naccept hint (Right/End)\n");

    {   // Right at end of buf with hint -> appends, sets plain_nav, clears hint/nav state
        auto e = make_editor({L"ping google.com"}, L"ping ");
        e.hint = L"google.com";
        // simulate the Right-key handler state transitions
        e.buf += e.hint;
        e.pos = (int)e.buf.size();
        e.hint.clear();
        e.hist_idx  = -1;
        e.saved.clear();
        e.plain_nav = true;
        check(e.buf == L"ping google.com" && e.plain_nav && e.hint.empty() && e.hist_idx == -1,
              "Right: buf completed, plain_nav set, state cleared");
    }
    {   // After accepting hint, next UP is plain (unfiltered)
        auto e = make_editor({L"ping 8.8.8.8", L"ls", L"ping google.com"}, L"ping ");
        e.hint      = L"google.com";
        e.plain_nav = false;
        // accept hint
        e.buf += e.hint; e.hint.clear(); e.hist_idx = -1; e.saved.clear(); e.plain_nav = true;
        // first UP
        enter_nav(e);    // plain_nav=true -> saved=""
        nav_step(e, -1); // plain nav -> most recent = "ping google.com"
        check(e.saved.empty() && e.buf == L"ping google.com",
              "after accept hint: next UP is plain (unfiltered)");
    }
}

static void test_escape() {
    printf("\nEscape\n");

    {   // Escape clears all editor state
        auto e = make_editor({L"ping google.com"}, L"ping ");
        e.hint      = L"google.com";
        e.hist_idx  = 0;
        e.saved     = L"ping ";
        e.plain_nav = true;
        // simulate Escape handler
        e.buf.clear(); e.pos = 0; e.hint.clear(); e.hist_idx = -1; e.saved.clear(); e.plain_nav = false;
        check(e.buf.empty() && e.hint.empty() && e.hist_idx == -1 &&
              e.saved.empty() && !e.plain_nav,
              "Escape: all state cleared");
    }
}

// ---- entry point ----

int main() {
    printf("pcmd tests\n");
    printf("==========");

    test_find_hint();
    test_nav_step();
    test_enter_nav();
    test_enter_nav_then_nav_step();
    test_accept_hint();
    test_escape();

    printf("\n==========\n");
    printf("%d passed, %d failed\n", pass_count, fail_count);
    return fail_count > 0 ? 1 : 0;
}
