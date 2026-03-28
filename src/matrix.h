// MODULE: matrix
// Purpose : Matrix digital rain screensaver
// Exports : matrix()
// Depends : common.h, terminal.h

static void matrix() {
    srand((unsigned)time(nullptr));

    int cols = term_width();
    int rows = term_height();

    // half-width katakana (U+FF66–U+FF9F) + digits
    static const char* glyphs[] = {
        "0","1","2","3","4","5","6","7","8","9",
        "\xEF\xBD\xA6","\xEF\xBD\xA7","\xEF\xBD\xA8","\xEF\xBD\xA9","\xEF\xBD\xAA",
        "\xEF\xBD\xAB","\xEF\xBD\xAC","\xEF\xBD\xAD","\xEF\xBD\xAE","\xEF\xBD\xAF",
        "\xEF\xBD\xB0","\xEF\xBD\xB1","\xEF\xBD\xB2","\xEF\xBD\xB3","\xEF\xBD\xB4",
        "\xEF\xBD\xB5","\xEF\xBD\xB6","\xEF\xBD\xB7","\xEF\xBD\xB8","\xEF\xBD\xB9",
        "\xEF\xBD\xBA","\xEF\xBD\xBB","\xEF\xBD\xBC","\xEF\xBD\xBD","\xEF\xBD\xBE",
        "\xEF\xBD\xBF","\xEF\xBE\x80","\xEF\xBE\x81","\xEF\xBE\x82","\xEF\xBE\x83",
        "\xEF\xBE\x84","\xEF\xBE\x85","\xEF\xBE\x86","\xEF\xBE\x87","\xEF\xBE\x88",
        "\xEF\xBE\x89","\xEF\xBE\x8A","\xEF\xBE\x8B","\xEF\xBE\x8C","\xEF\xBE\x8D",
        "\xEF\xBE\x8E","\xEF\xBE\x8F","\xEF\xBE\x90","\xEF\xBE\x91","\xEF\xBE\x92",
        "\xEF\xBE\x93","\xEF\xBE\x94","\xEF\xBE\x95","\xEF\xBE\x96","\xEF\xBE\x97",
        "\xEF\xBE\x98","\xEF\xBE\x99","\xEF\xBE\x9F",
    };
    static const int nglyph = (int)(sizeof(glyphs) / sizeof(glyphs[0]));
    auto rg = [&]() { return glyphs[rand() % nglyph]; };

    // move cursor to row r, col c (0-based → 1-based ANSI)
    auto pos = [](std::string& s, int r, int c) {
        char buf[32];
        snprintf(buf, sizeof(buf), "\x1b[%d;%dH", r + 1, c + 1);
        s += buf;
    };

    struct drop {
        int head;   // current row of head (-1 = waiting)
        int len;    // trail length
        int speed;  // ticks between moves
        int tick;
        int delay;  // ticks before (re)start
    };

    auto init_drops = [&](std::vector<drop>& d, int c, int r) {
        d.assign(c, { -1, 0, 0, 0, 0 });
        for (int i = 0; i < c; i++)
            d[i] = { -1, 6 + rand() % 15, 1 + rand() % 3, 0, rand() % r };
    };

    std::vector<drop> drops;
    init_drops(drops, cols, rows);

    // hide cursor, clear screen
    out("\x1b[?25l\x1b[2J");

    while (true) {
        // handle resize
        int new_cols = term_width(), new_rows = term_height();
        if (new_cols != cols || new_rows != rows) {
            cols = new_cols; rows = new_rows;
            init_drops(drops, cols, rows);
            out("\x1b[2J");
        }

        // non-blocking keypress check — any key exits
        DWORD n = 0;
        GetNumberOfConsoleInputEvents(in_h, &n);
        while (n-- > 0) {
            INPUT_RECORD ir; DWORD rd;
            ReadConsoleInputW(in_h, &ir, 1, &rd);
            if (ir.EventType == KEY_EVENT && ir.Event.KeyEvent.bKeyDown)
                goto done;
        }

        {
            std::string frame;
            frame.reserve(cols * 40);

            for (int c = 0; c < cols; c++) {
                drop& d = drops[c];

                if (d.head == -1) {
                    if (d.delay-- > 0) continue;
                    d.head = 0;
                    d.len   = 6 + rand() % 15;
                    d.speed = 1 + rand() % 3;
                    d.tick  = 0;
                }

                if (++d.tick < d.speed) continue;
                d.tick = 0;

                int h = d.head;
                int l = d.len;

                // bright white head
                if (h < rows) {
                    pos(frame, h, c);
                    frame += "\x1b[97m";
                    frame += rg();
                }
                // bright green just behind head
                if (h - 1 >= 0 && h - 1 < rows) {
                    pos(frame, h - 1, c);
                    frame += "\x1b[92m";
                    frame += rg();
                }
                // dim green at mid-trail
                int mid = h - l / 2;
                if (mid >= 0 && mid < rows) {
                    pos(frame, mid, c);
                    frame += "\x1b[2;32m";
                    frame += rg();
                }
                // erase tail
                int tail = h - l;
                if (tail >= 0 && tail < rows) {
                    pos(frame, tail, c);
                    frame += "\x1b[0m ";
                }

                d.head++;
                if (d.head - d.len > rows) {
                    d.head  = -1;
                    d.delay = rand() % 10;
                }
            }

            // random flicker: redraw a few trail chars with fresh glyphs
            int flickers = std::max(1, cols / 6);
            for (int i = 0; i < flickers; i++) {
                int c = rand() % cols;
                drop& d = drops[c];
                if (d.head < 2) continue;
                int r = d.head - 1 - rand() % std::max(1, d.len - 1);
                if (r < 0 || r >= rows) continue;
                int dist = d.head - 1 - r;
                pos(frame, r, c);
                if      (dist < d.len / 3)     frame += "\x1b[92m";
                else if (dist < 2 * d.len / 3) frame += "\x1b[32m";
                else                           frame += "\x1b[2;32m";
                frame += rg();
            }

            out(frame);
        }

        Sleep(40);
    }

done:
    // restore terminal
    out("\x1b[0m\x1b[2J\x1b[H\x1b[?25h");
}
