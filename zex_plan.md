# ZEX — Z Explorer Implementation Plan

## Overview

ZEX is a dual-panel file manager built into zcmd, toggled via `Ctrl+O`.
It uses a dedicated Windows console screen buffer so the shell screen is
preserved perfectly and restored on exit — no redraw, no flicker.

---

## Data Structures

```cpp
struct Entry {
    std::wstring name;      // filename
    bool         is_dir;
    bool         is_hidden;
    uint64_t     size;
    FILETIME     modified;
};

struct Panel {
    std::wstring         cwd;           // current directory
    std::vector<Entry>   entries;       // filtered + sorted list
    std::wstring         filter;        // current filter string
    int                  cursor;        // row index (0 = "..")
    int                  scroll;        // top visible row
    std::set<int>        selected;      // indices into entries
    bool                 active;        // has focus
};

struct ZexState {
    Panel        left;
    Panel        right;
    bool         typing_filter;         // are we in filter input mode
    HANDLE       zex_buf;              // our screen buffer
    HANDLE       shell_buf;            // saved shell buffer
};
```

Future refinement:
- ZEX should have an explicit focus mode separate from active panel state
- Active panel answers "which panel is selected"
- Focus mode answers "which UI element currently owns keyboard input"
- Example focus modes: panel navigation, filter input, modal dialog, progress/confirm prompt
- When filter input has focus, panel navigation keys must be disabled
- When a modal dialog has focus, both panel navigation and filter editing must be disabled
- Filter text is separate state from focus; entering filter mode gives focus to filter input, leaving it returns focus to panel navigation

---

## Layout

```
┌─────────────────────────┬─────────────────────────┐
│ C:/src/zcmd/            │ C:/src/zcmd/site/        │
│ [*.cpp                ] │ [*                      ]│
├─────────────────────────┼─────────────────────────┤
│ ..                      │ ..                       │
│ > build.bat             │   App.vue                │
│   todo.md               │   style.css              │
│   zcmd.cpp              │   ...                    │
│                         │                          │
│ [3 selected]            │                          │
└─────────────────────────┴─────────────────────────┘
 F5:Copy  F6:Move  F8:Recycle  S+F8:Delete  Ctrl+O:Exit
```

- Panel width = `(console_width - 1) / 2`
- Active panel border: bright white. Inactive: gray=240
- Entry colors follow zcmd color scheme (blue=dirs, green=exe, etc.)
- Selected entries override color to yellow=229
- Cursor row has background highlight
- ZEX layout must adapt live to console resize in both width and height; redraw against the new active window/buffer size instead of keeping the old frame

---

## Color Scheme

All ZEX colors live in a single block at the top of `zex.h` — one place to change everything.
They reuse the existing macros from `common.h` wherever possible, so ZEX stays consistent
with `ls`, the mp3 visualizer, and the prompt.

```cpp
// ── ZEX color config ─────────────────────────────────────────────────────────
// All colors used by ZEX are defined here. Change values freely.
// Reuses macros from common.h: GRAY BLUE RED YELLOW BRIGHT_YELLOW GREEN MAGENTA RESET

#define ZEX_BORDER_ACTIVE   "\x1b[97m"        // bright white  — active panel border
#define ZEX_BORDER_INACTIVE GRAY               // gray=240      — inactive panel border
#define ZEX_PATH            "\x1b[97m"        // bright white  — current path line
#define ZEX_FILTER          YELLOW             // yellow=229    — filter bar text
#define ZEX_CURSOR_BG       "\x1b[48;5;236m"  // dark gray bg  — cursor row highlight
#define ZEX_SELECTED        YELLOW             // yellow=229    — selected entries (overrides filetype color)
#define ZEX_DOTDOT          GRAY               // gray=240      — the ".." entry
#define ZEX_STATUSBAR       GRAY               // gray=240      — bottom key-hint bar
#define ZEX_BADGE           BRIGHT_YELLOW      // yellow=226    — "[N selected]" badge

// File type colors — kept in sync with ls_color() in commands.h
#define ZEX_COLOR_DIR       BLUE               // blue=75       — directories
#define ZEX_COLOR_EXE       GREEN              // green=114     — executables / .bat .cmd
#define ZEX_COLOR_ARCHIVE   RED                // red=203       — .zip .rar .7z .tar .gz
#define ZEX_COLOR_IMAGE     MAGENTA            // magenta       — .jpg .png .bmp .gif
#define ZEX_COLOR_MEDIA     "\x1b[36m"         // cyan=36       — .mp3 .mp4 .wav .mkv (matches ls)
#define ZEX_COLOR_HIDDEN    GRAY               // gray=240      — files starting with .

// Progress bar colors — reuse mp3 visualizer palette
#define ZEX_PROG_LOW        BLUE               // blue=75       — 0–40%
#define ZEX_PROG_MID        BRIGHT_YELLOW      // yellow=226    — 40–80%
#define ZEX_PROG_HIGH       RED                // red=203       — 80–100%
// ─────────────────────────────────────────────────────────────────────────────
```

---

## Filter Logic

Filter mode opens explicitly with `/`.
Normal printable typing outside filter mode does not filter the list; it performs
a quick jump to the next file or folder whose name starts with the typed prefix.

Inside filter mode there are two matching modes, auto-detected per keystroke:

| Condition          | Mode       | Behavior                                      |
|--------------------|------------|-----------------------------------------------|
| No `*` in filter   | Substring  | Show entries whose name contains filter text  |
| `*` present        | Glob       | Match with `*` (any chars) and `?` (one char) |

Examples:
- `build`     → matches `build.bat`, `old_build/`, `rebuild.sh`
- `*.zip`     → matches `archive.zip`, `backup.zip`
- `2011*07*.zip` → matches `2011_07_backup.zip`, `2011-07-01.zip`
- `b`         → matches any entry containing `b`

Filter is case-insensitive. `..` always shown regardless of filter.

### Quick Jump

- Printable typing outside filter mode builds a temporary jump prefix
- Cursor moves to the next entry whose name starts with that prefix
- Repeated typing extends the prefix while the user is actively typing
- `Backspace` removes the last jump character
- `Esc` clears the jump prefix
- `/` switches from normal navigation into filter input mode

---

## Selection

- `Insert` — toggle selection on cursor row, advance cursor
- Cannot select `..`
- `+` — open pattern prompt, select all entries matching a glob
- `-` — deselect all matching a glob
- Selected entries shown in yellow=229
- Panel header shows `[N selected]` when N > 0
- Selection is cleared when navigating into a new directory

---

## File Operations

### Source resolution (per operation)
1. If active panel has selections → operate on selected set
2. Else → operate on single entry under cursor

### F5 — Copy
- Source: active panel selection / cursor
- Destination: inactive panel `cwd`
- Uses `CopyFileEx` with progress callback
- Progress bar drawn in center of screen per file + overall

### F6 — Move
- Source: active panel selection / cursor
- Destination: inactive panel `cwd`
- Uses `MoveFileEx` (same volume = rename, cross-volume = copy+delete)
- Progress bar for cross-volume moves

### F8 — Delete (Recycle)
- Uses `SHFileOperation` with `FOF_ALLOWUNDO`
- Confirmation dialog before executing

### Shift+F8 — Delete Permanent
- Uses `SHFileOperation` without `FOF_ALLOWUNDO`
- Stronger confirmation dialog

### Confirmation Dialog

**1 item:**
```
 Delete "old_report.zip"?
 [Y] Recycle   [Shift+Y] Permanent   [Esc] Cancel
```

**Multiple items:**
```
 Delete 12 files, 3 folders in C:/src/zcmd/?
 [Y] Recycle   [Shift+Y] Permanent   [Esc] Cancel
```

### Safety Guards
- Block operations on: `C:\Windows`, `C:\Windows\System32`,
  `C:\Program Files`, `C:\Program Files (x86)`, any volume root (`X:\`)
- Never allow `..` to be in the operation set
- Check destination is not inside source (no copy-into-self)

---

## Screen Buffer Toggle (Ctrl+O)

```
On zcmd startup:
  shell_buf = GetStdHandle(STD_OUTPUT_HANDLE)

Ctrl+O pressed (enter ZEX):
  zex_buf = CreateConsoleScreenBuffer(GENERIC_READ | GENERIC_WRITE, ...)
  SetConsoleActiveScreenBuffer(zex_buf)
  hide cursor, enter raw input mode
  draw full ZEX UI

Ctrl+O pressed (exit ZEX):
  sync active panel cwd → zcmd prompt cwd
  SetConsoleActiveScreenBuffer(shell_buf)
  restore cursor, normal input mode

Ctrl+O pressed again (re-enter ZEX):
  SetConsoleActiveScreenBuffer(zex_buf)
  redraw ZEX UI (panel state preserved in memory)
```

Both buffers live in memory the whole session — switching is instant.
If the console window is resized while ZEX is visible, the ZEX buffer and layout
must resize and redraw to match the new console dimensions.

---

## Input Handling

| Key          | Action                                                   |
|--------------|----------------------------------------------------------|
| `Ctrl+O`     | Toggle shell / ZEX                                       |
| `Tab`        | Switch active panel                                      |
| `↑` / `↓`   | Move cursor                                              |
| `PgUp/PgDn` | Scroll by page                                           |
| `Home/End`   | Jump to first / last entry                               |
| `Enter`      | Navigate into dir / `ShellExecute` file                  |
| `Backspace`  | Go up one level (same as selecting `..`)                 |
| `Insert`     | Toggle select current row, advance cursor                |
| `+`          | Pattern-select dialog                                    |
| `-`          | Pattern-deselect dialog                                  |
| `F5`         | Copy                                                     |
| `F6`         | Move                                                     |
| `F8`         | Delete → recycle bin                                     |
| `Shift+F8`   | Delete permanent                                         |
| `/`          | Enter filter input mode                                  |
| `Esc`        | Clear filter if typing, else clear jump/selection        |
| Printable    | Quick-jump by filename prefix when not filtering         |
| `Backspace`  | Filter backspace in filter mode, else quick-jump backspace |

Focus rule:
- Input handling should first route by focus mode, then by active panel
- If focus is on filter input, panel navigation keys do not act on the panel
- If focus is on a dialog, only dialog keys are handled until it closes

---

## Progress Bar

Drawn in the center of the screen during F5/F6 operations:

```
 Copying 47 items...
 ─────────────────────────────────────
 From: C:/src/zcmd/old_builds/
 To:   C:/src/zcmd/site/

 [████████████░░░░░░░░░░░░░░] 43%
 report_2024.zip   2.1 MB / 4.8 MB

 [Esc] Cancel
```

`CopyFileEx` progress callback updates the bar on each chunk.

---

## Implementation Phases

### Phase 1 — Screen buffer + skeleton UI
- Create/switch console screen buffers on `Ctrl+O`
- Draw empty two-panel frame with borders, path, filter bar, status bar
- Hard-coded dummy entries to verify layout and colors
- `Tab` switches active panel highlight
- `Ctrl+O` restores shell perfectly

### Phase 2 — Real directory listing + navigation
- `load_entries(panel)`: reads directory, sorts (dirs first, then files)
- Apply filter on load
- Render real entries with correct colors
- `↑` / `↓` cursor movement with scroll
- `Enter` navigates into dirs, `Backspace` goes up
- `Home` / `End` / `PgUp` / `PgDn`
- Live resize should redraw the frame to the new console size
- Cwd sync to prompt on `Ctrl+O` exit

### Phase 3 — Filter
- `/` enters filter mode for the active panel
- Typing in filter mode appends to `panel.filter`, triggers re-filter
- `Backspace` removes last filter char when filter non-empty
- `Esc` clears filter and exits filter mode
- Auto-detect substring vs glob mode
- Filter bar renders current pattern
- Normal typing outside filter mode performs quick-jump by prefix

### Phase 4 — Selection
- `Insert` toggles selection, advances cursor
- Selected entries render yellow=229
- `[N selected]` badge in panel header
- `+` / `-` pattern select/deselect dialogs
- Selection clears on directory change

### Phase 5 — File operations
- F5 Copy with `CopyFileEx` + progress bar
- F6 Move with `MoveFileEx` + progress bar
- F8 Delete (recycle) with confirmation
- `Shift+F8` permanent delete with confirmation
- Safety path guards
- Refresh both panels after operation

---

## Out of Scope (for now)
- Rename (F2) — can add later
- Create dir (F7) — can add later
- File preview panel
- Sorting options (by name/size/date)
- Bookmarks / saved panel paths
