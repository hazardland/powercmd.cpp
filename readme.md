# Zcmd - Portable Windows Shell Replacement

> Zcmd is a portable, single executable, zero config, performance first Windows shell replacement for `cmd.exe` and PowerShell, with powerful built-in tools for developers.

![Zcmd showcase](./images/zcmd_showcase.gif)

Windows got nicer terminal windows. It did not get a shell that feels properly cared for.

Zcmd is a Windows shell replacement, a `cmd.exe` alternative, and for many developer workflows, a lighter PowerShell alternative. It goes after the part your hands notice: the prompt, history, completion, color, navigation, and the little moments that happen hundreds of times a day.

This is not a Linux shell conversion project. It is not trying to sell Windows users on pretending they are somewhere else. Zcmd is for staying on Windows and having the shell stop acting neglected.

Portable single executable. Zero config for the core shell. No runtime to install. No dependency chain just to get a prompt. Put `zcmd.exe` somewhere and run it.

The shell itself is native C++ and self-contained. Optional tools like `ffmpeg` and `yt-dlp` only matter if you want the media features. If you want a fast Windows terminal shell that is easy to carry between machines, this is the whole idea.

## Why Zcmd feels better immediately

- `ls` is actually pleasant to look at. Folders are blue, executables are green, archives are red, media pops, hidden files fade back.
- History survives restarts, filters as you type, and offers ghost hints instead of making you dig.
- `cd ../someth` + Tab works the way your fingers expect it to.
- The prompt shows time, git branch, dirty state, exit code, and long-command timing without spawning git.
- Paths are shown with `/` everywhere in the UI, because Windows can handle it and your eyes deserve better.
- You can type a folder path and just press Enter. No ceremony.

![Zcmd prompt, history hint, and branch info](./images/01-prompt.png)

![Colored ls output with file types](./images/02-ls.png)

The goal is not to add fifty layers of cleverness. The goal is to remove the low-grade irritation from a normal Windows terminal session.

## Why this Windows shell replacement works without breaking Windows

Zcmd owns the shell experience, but it does not try to replace the entire Windows command world.

```text
Windows Terminal / VS Code / any terminal host
        |
        +-- zcmd.exe
              |
              +-- built-ins handled directly in C++
              |     prompt, history, hints, ls, cat, edit, explore, resmon...
              |
              +-- everything else -> cmd.exe /c <command>
                    batch files, redirection, pipes, &&, ||, %VAR%, existing tooling
```

That split is the trick.

You get a better shell session without giving up normal Windows command compatibility. The boring stuff still works. The annoying stuff stops being annoying.

Zcmd also keeps a few Windows-specific realities in mind:

- GUI apps can launch detached instead of hijacking the shell.
- Common env-mutating wrappers like version managers can update the current session instead of dying in a child process.
- Full-screen tools use the terminal cleanly and return you to the exact shell view you had before.

## More than a prompt upgrade

The prompt and `ls` get your attention. The built-ins are what make Zcmd feel like a place instead of a wrapper.

### `ls` makes the shell readable

Colored listing, useful sorting, hidden-file handling, and filtering with `grep` or `findstr`. It fixes one of the most repeated actions in a Windows shell.

### `cat` is not just `type`

Syntax-highlighted text, inline image rendering, and terminal video playback when `ffmpeg` is available. It makes the terminal a viewer, not just an output box.

![Inline image or code rendering with cat](./images/03-cat.png)

### `explore` gives you a real file workspace

A full-screen two-panel file explorer built into the shell. Sort, filter, select, copy, move, recycle, delete, and stay in the same session the whole time.

![Two-panel file explorer](./images/04-explore.png)

### `play` gives the shell an MP3 player

Play a single MP3, shuffle a folder, jump tracks, pause, resume, change volume, and keep a lightweight now-playing UI inside the terminal.

### `top` is the fast task manager in the terminal

`top` opens immediately, updates immediately, and kills tasks immediately. No visible lag, no heavyweight detour through Task Manager, no feeling that the tool itself is slowing you down while you are trying to fix something.

### `resmon` gives you live system graphs

`resmon` shows CPU, GPU, RAM, battery, and network activity with live history graphs, directly in the terminal, without leaving the shell.

![Live resource monitor](./images/06-resmon.png)

### `edit` and `view` keep momentum alive

Open a file, fix the thing, save, and keep going. No app switch. No editor startup tax for tiny changes. Syntax highlighting is built in.

![Built-in full-screen editor](./images/05-edit.png)

### Other built-in tools

Zcmd also includes `yt`, `ip`, `calc`, `json`, `clip`, `clock`, `stopw`, `matrix`, and `notes`.

It is a very particular kind of terminal ambition: not "be everything," but "make the session weirdly capable."

## The commands people tend to keep

This is not the full manual. These are the ones that usually stick:

- `ls -al`, `ls -tr`, `ls | grep foo`
- `cd -`, `cd --`, `cd ~~`
- `cat file.cpp`, `cat image.png`
- `edit path/to/file`
- `top`
- `explore`
- `resmon`
- `play folder/of/mp3s`
- `yt mp3 <url>`
- `which <command>`
- `alias ll=ls -l`

Everything else still falls through to `cmd.exe`, so existing Windows habits, batch files, and toolchains keep working.

## Install and run

Download `zcmd.exe` from [Releases](../../releases), put it somewhere stable, and point your terminal profile at it. Installation is basically: download the single executable, put it in a folder, and run it.

Windows Terminal:

```json
{
  "commandline": "D:/tools/zcmd/zcmd.exe"
}
```

VS Code:

```json
{
  "terminal.integrated.profiles.windows": {
    "Zcmd": {
      "path": ["D:/tools/zcmd/zcmd.exe"]
    }
  },
  "terminal.integrated.defaultProfile.windows": "Zcmd"
}
```

Optional extras:

- `ffmpeg` enables terminal video playback and powers `yt`
- `yt-dlp` enables `yt mp3` and `yt mp4`
- a terminal with ANSI and Unicode support makes Zcmd feel the way it is supposed to

## Build

Build requirements:

- Windows
- `g++` on PATH, typically from MinGW-w64
- standard Windows system libraries available on the machine: `advapi32`, `shell32`, `iphlpapi`, `psapi`, `winmm`, `dxgi`, `pdh`

Run:

```bat
build.bat
```

That builds `zcmd.exe` and bumps the patch version on success.
