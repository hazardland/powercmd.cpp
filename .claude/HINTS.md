# pcmd — Input, Hint & History Navigation Spec

## State variables

| Variable    | Meaning |
|-------------|---------|
| `buf`       | What the user has actually typed. Only this gets executed. |
| `hint`      | Gray ghost text shown to the right of buf. Never executed directly. |
| `hist_idx`  | `-1` = edit mode. `>= 0` = navigating history. |
| `saved`     | Snapshot of buf taken at the moment UP was first pressed. Used as filter prefix. |
| `plain_nav` | When `true`, UP/DOWN ignores buf and travels full history unfiltered. |

---

## Ghost hint (edit mode, `hist_idx == -1`)

Triggered automatically on every keystroke and delete.

- Scans history **backwards** for the most recent entry that **starts with** buf.
- Shows the suffix as **gray text** after the cursor.
- For `cd <path>`: hint comes from **directory completions**, not history.
- For `cd` alone: no hint shown.

### Accepting the hint

| Key | Effect |
|-----|--------|
| `→` or `End` | Appends hint to buf. Clears hint, `hist_idx`, `saved`. Sets `plain_nav = true`. |
| `Enter` | Executes buf only. Hint is ignored. |
| Any char | Hint recalculated from new buf. `plain_nav = false`. |
| Backspace / Del | Hint recalculated from shorter buf. `plain_nav = false`. |
| `Esc` | Clears buf, hint, `hist_idx`, `saved`, `plain_nav`. |

---

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

---

## Tab completion (always independent)

- Completes files and directories at the current path prefix.
- After `cd`: directories only.
- Repeated Tab cycles through all matches.
- Tab always clears `hint` before rendering.
- Tab does not affect `hist_idx`, `saved`, or `plain_nav`.

---

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
