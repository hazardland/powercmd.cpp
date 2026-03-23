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

## Prompt / UI
- Tool name: **Power CMD**
- Prompt format: `[time]folder[branch*]> `
- Colors: gray=240, blue=75 (normal), red=203 (elevated), yellow=229 (branch)
- Path separators displayed as `/` (forward slash) in tab completion

## General
- No breaking changes without checking with user first
- Keep changes simple and focused — no over-engineering
- Test by compiling to `pcmd2.exe` when `pcmd.exe` is running/locked
