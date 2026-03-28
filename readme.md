# Zcmd — A single executable shell for Windows

![Zcmd demo](./images/pcmd_cat_video.gif)

## Why

Windows developers have always had a rough deal on the terminal front. Microsoft eventually got tired of `cmd.exe` too and pushed PowerShell — but PowerShell is an elephant. You can feel the startup latency in your chest before the first prompt even appears. The alternatives people reach for on Windows — [Nushell](https://www.nushell.sh/), [Fish for WSL](https://fishshell.com/), [Clink](https://chrisant996.github.io/clink/) — are either built on scripting runtimes that spawn cascades of child processes, require WSL, or bolt features onto cmd in ways that still feel bolted on. Fast fingers notice all of it.

So the question became: why not build exactly what I need, nothing more?

`cmd.exe` is helpless. Microsoft added `↑` to browse history. They added Tab completion for the current folder. They were *this close*. But they never taught it to save history to a file on exit. They never added `cd ../` tab completion. They never added color to `dir`. Tiny things. Maddening things.

What I was actually missing:

- **Colored `ls`** — the jewel of the whole project. Seeing folders in blue, executables in green, archives in red, the same way CentOS does it — it genuinely warms my heart every time
- **History that persists** — close the window, open it again, your history is still there
- **History hints** — gray ghost text as you type, filtered `↑`/`↓` that only shows commands matching what you started typing
- **Folder tab completion** — `cd ../pr` + Tab and it just works, including parent paths; `ls` gets the same hints and tab cycling
- **`cd -`** — jump back to where you were, like every Linux shell takes for granted; `cd --` restores the directory from the previous session; `cd ~~` jumps to wherever zcmd.exe lives
- **Elapsed time** — always want to know how long that build or install actually took
- **`pwd`, `which`** — small tools that should just exist
- **Multiline paste** — pasting a curl command from Chrome without the terminal losing its mind

Built it from scratch in C++ with the help of Claude. Single file, no dependencies, Windows SDK only. The process was surprisingly dopamine-driven — each feature was a small, satisfying puzzle: *what is the next useful thing I can add without stealing a millisecond of startup time?* That constraint kept everything honest — every feature had to earn its place without adding a millisecond to startup or execution.

## Architecture

```
Windows Terminal
└── zcmd.exe  (permanent process, entire session)
        │
        ├── built-ins handled directly in C++
        │   cd, ls, pwd, which, version, help, auto-cd, hints...
        │
        └── everything else → cmd.exe /c <command>  (spawned per command, exits when done)
                                    │
                                    └── piping, redirection, %VAR% expansion,
                                        batch files, dir, echo, set, &&, ||...
```

`zcmd.exe` owns the input loop and UX. `cmd.exe` is a temporary worker used for execution — you get full Windows command compatibility without `zcmd.exe` needing to reimplement any of it.

## Features

**Prompt**
- `[time]folder[branch*]>` format with 256-color ANSI
- Git branch and dirty indicator (reads `.git/HEAD` and `.git/index` directly — no process spawn)
- Exit code shown in red `[1]` when the last command failed, cleared on next success
- Red folder color when running elevated (admin)
- Window title shows folder name at rest, command name while running
- Prompt never appears mid-line — detects partial output and adds newline automatically
- Elapsed time shown in gray (e.g. `[3.2s]`) after commands that take longer than 2 seconds

**Input**
- Full line editing with cursor movement (`←` `→` `Home` `End`)
- `Ctrl+Left` / `Ctrl+Right` — jump word by word
- `Ctrl+C` — cancel input or interrupt a running command
- Multiline paste — `^` and `\` line continuation, each segment shown with `>` prompt
- Forward slashes everywhere — paths always displayed as `d:/src/project`
- Full Unicode support — Georgian, emoji, anything

**History hints**
- Gray ghost text appears as you type, pulled from history
- `→` or `End` to accept, or keep typing to ignore
- `↑` / `↓` filters history by what you have typed — only matching commands cycle
- After accepting a hint with `→`, `↑` / `↓` switches to plain full-history navigation
- History deduplicated on load — no duplicates across sessions
- Saved on `exit`, window close, logoff, and shutdown

**Tab completion**
- Completes files and directories from the current path
- After `cd` or `ls` — directories only, no files mixed in
- Repeated Tab cycles through all matches
- Auto-dive — if the only match is a directory, the next Tab steps inside it and starts cycling its contents
- Tab and hint are independent — Tab never accepts the hint, Right/End do that

**Built-in commands**

`ls [flags] [path] [| grep <word>]` — colored directory listing

| Flag | Effect |
|------|--------|
| `-a` | show hidden files (gray) |
| `-l` | long format: size + time columns, sorted alphabetically |
| `-s` | sort by size descending, show size column |
| `-t` | sort by time descending, show time column (`yyyy-mm-dd HH:MM:SS`) |
| `-r` | reverse sort order (global, combines with any flag) |

Flags combine freely: `ls -al`, `ls -tr`, `ls -lt`, `ls -a -s -r`. When both `-s` and `-t` are given, the first one sets the sort (`-st` sorts by size, `-ts` by time). `-l` alone shows both columns without changing sort order. Directories always sort above files within each group.

`ls | grep <word>` / `ls | findstr <word>` — filter results by name (case-insensitive substring). Combines with flags: `ls -tr | grep cpp`.

`ls --help` — full flag reference.

Colors: dirs blue · executables green · archives red · images magenta · audio/video cyan · hidden gray

---

`cat [path] [| grep <word>]` — print file contents with syntax highlighting; renders images inline

| Extension | Language |
|-----------|----------|
| `.cpp` `.c` `.h` `.hpp` | C / C++ |
| `.py` | Python |
| `.js` `.ts` `.jsx` `.tsx` | JavaScript / TypeScript |
| `.json` | JSON |
| `.md` | Markdown |
| `.bat` `.cmd` | Batch |
| `.sol` | Solidity |
| `.php` | PHP |
| `.go` | Go |
| `.rs` | Rust |
| `.cs` | C# |
| `.java` | Java |
| `.sh` `.bash` | Bash / Shell |
| `.html` `.htm` `.xml` `.svg` | HTML / XML |

`cat file.cpp | grep <word>` — print only lines containing word (case-insensitive).

**Image rendering** — `cat` detects image extensions (`.jpg` `.jpeg` `.png` `.bmp` `.gif` `.tga` `.psd`) and renders them directly in the terminal using 24-bit color and Unicode quadrant-block characters (`▀▄▌▐` etc.). The image is scaled to fit the terminal width and height, preserving aspect ratio. Requires a terminal with true-color and Unicode support (Windows Terminal works great).

**Video playback** — `cat` also plays video files (`.mp4` `.mkv` `.avi` `.mov` `.webm` `.flv` `.wmv`) directly in the terminal using the same block-art renderer. Frames are piped from ffmpeg at 24 fps and scaled to fit the terminal. Press `Esc` or `Ctrl+C` to stop. Requires [ffmpeg](https://ffmpeg.org) to be installed and on PATH.

---

- `cd <dir>` — with `/d` flag, `~` for home, `-` for previous directory, `--` for last session's directory, `~~` for zcmd.exe's directory
- `pwd` — print current directory with forward slashes
- `which <cmd>` — locate a command in PATH, or identify zcmd built-ins
- `version` — print current zcmd version
- `help` — list all built-in commands
- Auto-cd — type a directory path and press Enter, no `cd` needed

**Execution**
- Blank Enter refreshes the prompt and clock (impossible in pure batch)
- `Ctrl+C` correctly stops child processes
- Progress bars, color output, interactive tools all work — child process inherits the console directly

## Setup

### Windows Terminal

Point your profile's command line directly at `zcmd.exe`:

```json
{
    "commandline": "d:/src/powerline/zcmd.exe",
}
```

### VSCode

```json
{
    "terminal.integrated.profiles.windows": {
        "zcmd": {
            "path": [
                "d:/src/powerline/zcmd.exe"
            ]
        }
    },
    "terminal.integrated.defaultProfile.windows": "zcmd"
}
```

## Release

The release is a single file: **`zcmd.exe`**. No runtime, no DLLs, no config files required. Built against the Windows SDK only.

Download the latest `zcmd-v0.0.X.zip` from the [Releases](../../releases) page, extract, and point your terminal profile at `zcmd.exe`.

To build from source:
```
g++ zcmd.cpp -o zcmd.exe -DVERSION_MINOR=X -ladvapi32 -lshell32
```
or just run `build.bat` which auto-increments the version.
