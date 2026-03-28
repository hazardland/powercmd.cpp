# Powerline / Zcmd — Dev Rules

## Git
- Never commit directly to `master` — always create a new branch when starting work
- Only commit when explicitly asked by the user
- Never delete files without permission

## Project Structure
- **Batch system** (`init.bat`, `_set.bat`, `alias.bat`, etc.) — keep untouched, it is the stable base
- **zcmd.cpp** — C++ alternative called "Zcmd", single file executable, lives in the same folder
- Both systems are independent; zcmd.cpp does not replace the batch system

## C++ Conventions (zcmd.cpp)
- `snake_case` for all variable names
- Single word for function and variable names where there is no ambiguity
- Single `.cpp` file, no external dependencies, Windows SDK only
- Compile via `build.bat` which auto-bumps the patch version

## Versioning
- Format: `0.0.X` — only the third number is bumped
- `version.txt` stores the current patch number
- `build.bat` increments it automatically on each successful build and rolls back on failure
- User resets or promotes version manually when needed (e.g. `0.1.0`, `1.0.0`)
- Release titles use `zcmd v0.0.X` format
- Always attach `zcmd.exe` as a binary asset when creating a GitHub release

## Prompt / UI
- Tool name: **Zcmd**
- Prompt format: `[time]folder[branch*]> `
- Colors: gray=240, blue=75 (normal), red=203 (elevated), yellow=229 (branch)
- Path separators displayed as `/` (forward slash) everywhere (prompt, pwd, tab completion)

## Color guide (use consistently across all built-ins)
- Directories: blue=75
- Executables / commands: green=114
- Archives: red=203
- Images: magenta=\x1b[38;5;170m
- Audio/video: cyan=\x1b[38;5;51m
- Hidden files: gray=240
- These are defined as macros: GRAY, BLUE, RED, YELLOW, GREEN, RESET

## General
- No breaking changes without checking with user first
- Keep changes simple and focused — no over-engineering
- `build.bat` kills any running `zcmd.exe` before building, so no manual process kill needed

## Postponed ideas
- **Tab-complete executables from PATH** — postponed: mixing PATH executables with local folder contents in Tab completion is ambiguous and confusing. Tab is for folder/file navigation only.

---

# Input, Hint & History Navigation Spec

## State variables

| Variable    | Meaning |
|-------------|---------|
| `buf`       | What the user has actually typed. Only this gets executed. |
| `hint`      | Gray ghost text shown to the right of buf. Never executed directly. |
| `hist_idx`  | `-1` = edit mode. `>= 0` = navigating history. |
| `saved`     | Snapshot of buf taken at the moment UP was first pressed. Used as filter prefix. |
| `plain_nav` | When `true`, UP/DOWN ignores buf and travels full history unfiltered. |

## Ghost hint (edit mode, `hist_idx == -1`)

Triggered automatically on every keystroke and delete.

- Scans history **backwards** for the most recent entry that **starts with** buf.
- Shows the suffix as **gray text** after the cursor.
- For `cd <path>` and `ls <path>`: hint comes from **directory completions**, not history.
- For `cd` or `ls` alone: no hint shown.

### Accepting the hint

| Key | Effect |
|-----|--------|
| `→` or `End` | Appends hint to buf. Clears hint, `hist_idx`, `saved`. Sets `plain_nav = true`. |
| `Enter` | Executes buf only. Hint is ignored. |
| Any char | Hint recalculated from new buf. `plain_nav = false`. |
| Backspace / Del | Hint recalculated from shorter buf. `plain_nav = false`. |
| `Esc` | Clears buf, hint, `hist_idx`, `saved`, `plain_nav`. |

## History navigation (UP / DOWN)

### Entering nav mode

- UP when `hist_idx == -1`:
  - If `plain_nav == true`: `saved = ""` → plain navigation.
  - Otherwise: `saved = buf` → filtered navigation.
  - Sets `hist_idx = hist.size()` then searches backward.
- DOWN when `hist_idx == -1`: no-op.

### Filtered navigation (`saved` is non-empty)

- UP: searches backward (with wrap-around) for entries starting with `saved`.
- DOWN: searches forward (with wrap-around) for entries starting with `saved`.
- Matching entry: buf stays as `saved`, suffix shown as gray hint.
- No matches at all: falls back to plain cycle.
- `→` / `End`: accepts hint, sets `plain_nav = true`, exits nav mode.
- `Enter`: auto-accepts hint + executes full command.
- Any char or backspace: exits nav mode, `plain_nav = false`, hint recalculated.

### Plain navigation (`saved` is empty)

- UP / DOWN: cycles through all history with wrap-around.
- Full entry shown in buf, no hint split.
- `→` / `End`: no hint to accept, cursor moves normally.

## Tab completion (always independent)

- Completes files and directories at the current path prefix.
- After `cd` or `ls`: directories only, no files mixed in.
- Repeated Tab cycles through all matches.
- Auto-dive: if the only match is a directory (ends with `/`), the next Tab re-initializes completion inside it instead of cycling.
- Tab always clears `hint` before rendering. Tab never reads or accepts the hint — Right/End do that.
- Tab does not affect `hist_idx`, `saved`, or `plain_nav`.

## Key reference

| Key        | Edit mode                              | Nav mode                              |
|------------|----------------------------------------|---------------------------------------|
| `↑`        | Enter nav mode (filtered or plain)     | Previous match (wraps)                |
| `↓`        | No-op                                  | Next match (wraps)                    |
| `→` / End  | Accept hint → `plain_nav = true`       | Accept hint → exit nav, `plain_nav = true` |
| `Enter`    | Execute buf (hint ignored)             | Auto-accept hint + execute            |
| `Esc`      | Clear all state                        | Clear all state                       |
| `Tab`      | File/dir completion, clears hint       | File/dir completion, clears hint      |
| Char input | Recalc hint, `plain_nav = false`       | Exit nav, recalc hint                 |
| Backspace  | Recalc hint, `plain_nav = false`       | Exit nav, recalc hint                 |
