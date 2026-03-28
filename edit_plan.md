# edit.h — Implementation Plan

## Context

This is a built-in `edit` command for **Zcmd** (`zcmd.exe`), a single-file C++ terminal
shell for Windows. The codebase uses a **unity build**: `zcmd.cpp` includes all modules in
order, so every module has access to everything defined before it — no separate compilation,
no linker issues, just `#include` order matters.

---

## Current module structure (post-refactor)

```
src/common.h     — includes, color macros, handles, to_utf8/to_wide, out/err,
                   normalize_path(), clipboard_get(), clipboard_set()
src/terminal.h   — term_width(), term_height()
src/signal.h     — ctrl_c_fired, g_input, ctrl_handler
src/info.h       — cwd, branch, dirty
src/prompt.h     — make_prompt
src/complete.h   — tab completion
src/input.h      — struct input, readline loop (the prompt input, NOT the file editor)
src/persist.h    — history, aliases
src/commands.h   — cd, ls, run, which, rule
src/highlight.h  — detect_lang(), colorize_inline(), colorize_line()
src/image.h      — cat_image
src/video.h      — cat_video
src/cat.h        — cat() command
src/edit.h       — TO BE WRITTEN
```

**`zcmd.cpp` include order** — add `edit.h` last:
```cpp
#include "src/common.h"
#include "src/terminal.h"
#include "src/signal.h"
#include "src/info.h"
#include "src/prompt.h"
#include "src/complete.h"
#include "src/input.h"
#include "src/persist.h"
#include "src/commands.h"
#include "src/highlight.h"
#include "src/image.h"
#include "src/video.h"
#include "src/cat.h"
#include "src/edit.h"   // <-- add this
```

### `edit.h` dependencies
- `common.h` — out(), color macros, normalize_path(), clipboard_get(), clipboard_set()
- `terminal.h` — term_width(), term_height()
- `highlight.h` — detect_lang(), colorize_line()

**No dependency on `input.h`** — the two modules are fully parallel and independent.

---

## Color macros (from common.h)

```cpp
#define GRAY   "\x1b[38;5;240m"
#define BLUE   "\x1b[38;5;75m"
#define RED    "\x1b[38;5;203m"
#define YELLOW "\x1b[38;5;229m"
#define GREEN  "\x1b[38;5;114m"
#define RESET  "\x1b[0m"
```

---

## C++ conventions

- `snake_case` for all variable and function names
- Single word for function names where unambiguous
- No external dependencies — Windows SDK only
- All code goes in `src/edit.h`

---

## Module header

```cpp
// MODULE: edit
// Purpose : full-screen terminal file editor
// Exports : edit_file()
// Depends : common.h, terminal.h, highlight.h
```

---

## Data structure

```cpp
struct file_buf {
    std::vector<std::string> lines;  // one entry per line, no line ending chars
    bool crlf         = false;       // original line ending (true = CRLF, false = LF)
    bool trailing_nl  = true;        // whether original file ended with a newline
    std::string path;
    bool modified     = false;

    int cur_row  = 0;   // cursor row (index into lines)
    int cur_col  = 0;   // cursor column (byte offset into current line)
    int top_row  = 0;   // first visible row (vertical scroll)
    int left_col = 0;   // first visible column (horizontal scroll, wrap=off only)
    bool wrap    = false;

    int sel_row  = -1;  // selection anchor row; -1 = no selection
    int sel_col  = -1;  // selection anchor col
};
```

---

## File load / save

### Load (binary mode — preserves line endings)

```
open file in ios::binary
read all bytes into string
detect: if any "\r\n" found → crlf = true, else crlf = false
detect: trailing_nl = last char is '\n'
strip all '\r'
split on '\n' into lines vector
```

If file does not exist: start with one empty line, crlf = false, trailing_nl = true (new file).

### Save

```
open file in ios::binary
for each line:
    write line bytes
    write "\r\n" or "\n" based on crlf flag
skip final newline if trailing_nl was false
set modified = false
```

**Never converts line endings.** File is always saved with its original style.

---

## Screen rendering

Full repaint on every keypress. No incremental diffing — simple and flicker-free when
batched into a single out() call.

### Layout

```
row 1..H-1  : file content (H = term_height())
row H       : status bar
```

### Gutter

4-char line number gutter: `" 42 "` in GRAY. Always visible.

### Content rendering (per visible row)

```
for row in [top_row .. top_row + visible_rows - 1]:
    move cursor to screen row
    print gutter (line number, gray)
    if wrap=off: print line[left_col .. left_col + visible_width]
    if wrap=on:  print line (terminal wraps naturally)
    apply selection highlight with \x1b[7m (reverse video) over the selected range
    apply colorize_line() for syntax colors
```

**Important:** colorize_line() returns ANSI-colored string. ANSI codes have zero width,
so cursor positioning must use absolute `\x1b[row;colH` escapes, not rely on cursor drift.

### Status bar (last row)

```
\x1b[H;1H   filename  [+]  row:col  CRLF/LF  WRAP
```

- `[+]` shown only when modified
- `CRLF` or `LF` shows detected line ending
- `WRAP` shown when word wrap is on
- Colors: filename in GREEN, modified marker in YELLOW, rest in GRAY

### Cursor placement

After drawing all content:
```
screen_row = cur_row - top_row + 1
screen_col = cur_col - left_col + gutter_width + 1
emit \x1b[screen_row;screen_colH
```

When wrap=on, screen_row may be offset by wrapped lines above cursor — account for this.

---

## Scrolling invariants

After every cursor move, enforce:
```
if cur_row < top_row:            top_row = cur_row
if cur_row >= top_row+visible:   top_row = cur_row - visible + 1
if wrap=off:
    if cur_col < left_col:       left_col = cur_col
    if cur_col >= left_col+vis_w: left_col = cur_col - vis_w + 1
```

---

## Key handling

Use `ReadConsoleInputW(in_h, &ir, 1, &count)` — same Win32 pattern as `input.h`.
Process only `KEY_EVENT` with `bKeyDown == true`.

Extract:
```cpp
WORD  vk    = ir.Event.KeyEvent.wVirtualKeyCode;
wchar_t ch  = ir.Event.KeyEvent.uChar.UnicodeChar;
DWORD state = ir.Event.KeyEvent.dwControlKeyState;
bool ctrl   = (state & (LEFT_CTRL_PRESSED  | RIGHT_CTRL_PRESSED)) != 0;
bool shift  = (state & SHIFT_PRESSED) != 0;
bool alt    = (state & (LEFT_ALT_PRESSED   | RIGHT_ALT_PRESSED))  != 0;
```

### Navigation

| Key | Action |
|-----|--------|
| `←` `→` `↑` `↓` | move cursor; clamp col to line length |
| `Home` | cur_col = 0 |
| `End` | cur_col = line.size() |
| `Ctrl+←` | jump word left |
| `Ctrl+→` | jump word right |
| `PgUp` | cur_row -= visible_rows - 1 |
| `PgDn` | cur_row += visible_rows - 1 |
| `Ctrl+Home` | cur_row = 0, cur_col = 0 |
| `Ctrl+End` | cur_row = last line, cur_col = last col |
| `Ctrl+PgUp` | same as Ctrl+Home |
| `Ctrl+PgDn` | same as Ctrl+End |

### Editing

| Key | Action |
|-----|--------|
| Printable char | insert at cur_col, advance col, set modified |
| `Backspace` | delete char before cursor; if col=0 and row>0 → merge with previous line |
| `Delete` | delete char at cursor; if at line end → merge with next line |
| `Enter` | split line at cur_col → two lines; cursor to col 0 of new line |
| `Tab` | insert 4 spaces (simple, no smart indent) |

### Selection (Shift + navigation)

- Shift+arrow / Shift+Home / Shift+End: if no selection → set anchor=(cur_row,cur_col), then move cursor
- Selection = range between anchor and cursor (order-independent)
- Any non-shift key (except Ctrl+C/X) clears selection (sel_row = -1)

### Clipboard

| Key | Action |
|-----|--------|
| `Ctrl+C` | copy selected text → clipboard_set() |
| `Ctrl+X` | cut: copy then delete selection |
| `Ctrl+V` | paste: clipboard_get(), insert at cursor (handle \n for multiline) |

### Other

| Key | Action |
|-----|--------|
| `Ctrl+S` | save file; clear modified |
| `Alt+Z` | toggle word wrap; redraw |
| `Ctrl+Q` or `Esc` | if modified → show "Unsaved changes. Quit? (y/n)"; else exit |

---

## Selection rendering

Selection is rendered by walking visible line chars and wrapping selected range in
`\x1b[7m` (reverse video) / `\x1b[27m` (reverse off). This composites over syntax colors.

Helper needed:
```cpp
bool in_selection(const file_buf& f, int row, int col);
// returns true if (row,col) falls between anchor and cursor
```

---

## Clipboard helpers (already in common.h)

```cpp
std::wstring clipboard_get();
void clipboard_set(const std::wstring& text);
```

`edit.h` uses these directly. For multi-line copy, join lines with `\r\n` (matches Windows
clipboard convention regardless of file's line ending style).

---

## Wire-up in zcmd.cpp

### Add include
```cpp
#include "src/edit.h"   // edit_file() — after cat.h
```

### Add dispatch in main loop
```cpp
if (lower.size() >= 5 && lower.substr(0, 5) == "edit ") {
    std::string arg = line.substr(5);
    while (!arg.empty() && arg.front() == ' ') arg.erase(arg.begin());
    while (!arg.empty() && arg.back()  == ' ') arg.pop_back();
    last_code = edit_file(arg);
    continue;
}
```

### Add to help text
```cpp
GREEN "edit" RESET "    Edit a file  edit path/to/file\r\n"
```

---

## Implementation order

Build and test after each step.

1. **Load / save** — `load()`, `save()`, binary mode, line ending detection, `file_buf` struct
2. **Basic draw** — clear screen, gutter, raw text (no color, no wrap yet), status bar, cursor placement
3. **Navigation** — all arrow/Home/End/PgUp/PgDn/Ctrl variants + scroll enforcement
4. **Editing** — insert char, backspace, delete, enter, tab
5. **Syntax highlight** — plug `colorize_line()` into draw
6. **Word wrap** — wrap=on rendering path, scroll adjustment
7. **Selection** — anchor tracking, `in_selection()`, reverse-video rendering
8. **Clipboard** — Ctrl+C copy, Ctrl+X cut, Ctrl+V paste
9. **Save / quit** — Ctrl+S, Ctrl+Q/Esc with dirty check, Alt+Z wrap toggle
10. **Wire up** — zcmd.cpp dispatch + help text

---

## Things NOT included (keep it simple)

- Undo / redo
- Find / replace
- Multiple files / tabs
- Smart indentation
- Line wrapping that counts tab width (tabs treated as 1 char)
