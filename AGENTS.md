# Zcmd Rules

## Git
- Never commit directly to `master`; use a branch for work.
- Only commit when explicitly asked by the user.
- When the user asks for a commit, stage and commit everything currently shown by `git status` unless the user says otherwise.
- Never delete files without permission.

## Project Boundaries
- `zcmd.cpp` and the `src/*.h` modules are the C++ side of the project.

## Implementation
- Keep changes simple and focused; avoid over-engineering.
- No breaking changes without checking with the user first.
- Prefer Windows SDK / standard Windows APIs over extra dependencies.
- Prefer native Windows APIs and standard library solutions whenever possible.
- Avoid third-party library dependencies unless there is a clear need and the user agrees.
- Build through `build.bat`.

## Naming
- Use `snake_case` for variables and functions.
- Prefer single-word function and variable names where there is no ambiguity.
- Prefer single-word module / command names when possible.
- Use command-style names for user-facing tools when it fits the project style.
  Examples: `play`, `edit`, `explore`.
- Use PascalCase / camel-cased type names for structs and other types.
  Examples: `ExploreState`, `ExploreDialog`.
- Use all-caps enum types and enum values for explorer-style mode constants.
  Examples: `EXPLORER_SORT_MODE`, `EXPLORER_SORT_NAME`.
- Use a clear module prefix for related helpers.
  Example: `explore_toggle()`, `explore_draw()`, `explore_load_entries()`.

## UI And Paths
- Tool name is `Zcmd`.
- Prompt format is `[time]folder[branch*]> `.
- Display paths with `/` separators everywhere in UI output.
- Keep built-in command UX keyboard-friendly and consistent.

## Color Rules
- Keep the built-in color language consistent.
- Directories: blue `75`
- Executables / commands: green `114`
- Archives: red `203`
- Images: magenta `38;5;170`
- Audio/video: cyan `38;5;51`
- Hidden files: gray `240`
- Shared macros stay the source of truth: `GRAY`, `BLUE`, `RED`, `YELLOW`, `GREEN`, `RESET`

## Versioning
- Version format is `0.0.X`.
- Only the third number is auto-bumped during normal builds.
- `version.txt` stores the current patch version.
- `build.bat` increments on successful build and rolls back on failure.
- Major/minor version jumps are manual.

## Releases
- By default, a user request to release means the binary is already built and `version.txt` already contains the intended release patch version.
- For a normal release request, do not run `build.bat`.
- For a normal release request, do not edit `version.txt`.
- For a normal release request, do not commit, merge, tag, or push unless the user explicitly asks for that as a separate step.
- Use release tags in `v0.0.X` format and release titles in `zcmd v0.0.X` format.
- Always attach `zcmd.exe` as the GitHub release asset.
- For a normal release request, include short release notes that summarize what changed in that version, not just the version number.
- If the user does not provide release notes, infer a concise high-signal summary from the recent completed work.
- Default release flow:
  Read `version.txt`
  Release `.\zcmd.exe` as `v0.0.X`
  Example: `gh release create v0.0.X .\zcmd.exe --title "zcmd v0.0.X" --notes "Highlights: ..."`
- If the asset needs to be replaced on an existing release, use:
  `gh release upload v0.0.X .\zcmd.exe --clobber`
- Commit, push, branch cleanup, and tag/source alignment are separate follow-up tasks and should not be done as part of a normal release request unless explicitly requested.
- This shortcut flow may leave the GitHub release tag temporarily pointing at the current remote state instead of the local unpushed commit; fixing that later is a separate explicit task.

## Notes
- `build.bat` already kills running `zcmd.exe` before building.
- Tab completion should stay focused on file/folder navigation; PATH executable completion is postponed by design.
