# Powerline / Power CMD — Dev Rules

## Git
- Always work on a **separate branch**, never touch `master` or `minimal` directly
- Current working branch: `claude`
- Only commit when explicitly asked by the user
- Never delete files without permission

## Project Structure
- **Batch system** (`init.bat`, `_set.bat`, `alias.bat`, etc.) — keep untouched, it is the stable base
- **pcmd.cpp** — C++ alternative called "Power CMD", single file executable, lives in the same folder
- Both systems are independent; pcmd.cpp does not replace the batch system

## C++ Conventions (pcmd.cpp)
- `snake_case` for all variable names
- Single word for function and variable names where there is no ambiguity
- Single `.cpp` file, no external dependencies, Windows SDK only
- Compile via `build.bat` which auto-bumps the patch version

## Versioning
- Format: `0.0.X` — only the third number is bumped
- `version.txt` stores the current patch number
- `build.bat` increments it automatically on each successful build and rolls back on failure
- User resets or promotes version manually when needed (e.g. `0.1.0`, `1.0.0`)
- Release titles use `pcmd v0.0.X` format (not "Power CMD v0.0.X")

## Prompt / UI
- Tool name: **Power CMD**
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
- Test by compiling to `pcmd2.exe` when `pcmd.exe` is running/locked

## Postponed ideas
- **Tab-complete executables from PATH** — postponed: mixing PATH executables with local folder contents in Tab completion is ambiguous and confusing. Tab is for folder/file navigation only.
