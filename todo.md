# Todo

## Media

- `cat *.mp4`
  This is for future, but we should come up with some clever and high-performance solution to add sound to video playback.

- `vid`
  The `vid` tool is slow.

## Shell

- `Pipes`
  Make built-in commands work with pipes with outer tools. Outside `grep` does not see our command output right now, but if we implement this we should not compromise performance or ruin the already established comfortable experience.

- `rm`, `cp`, `mv`
  Standard Windows commands should be followed with autocomplete when they require a path.

- External task control
  When executing a task from the shell which opens a new window, for example `image.jpg`, `Ctrl+C` in the terminal does not kill the task or release the terminal prompt.

- Multiline history hints
  Should multiline history entry hinting work? Did we intentionally disable it?

## Explorer

- Long names
  Long names should hide if there is no space in the tab. They are hiding now, but filenames with Unicode chars could still have edge cases.

- Operation cancel/resume on mid copy or move between disks with progress functions (?)

# I like VSCode terminal color scheme any chance we make it permanent? Zed One Theme Dark
