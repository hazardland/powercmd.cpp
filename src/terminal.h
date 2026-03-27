// MODULE: terminal
// Purpose : terminal dimension queries
// Exports : term_width() term_height()
// Depends : common.h

// Returns the visible column count of the terminal window; falls back to 80 if not a real console.
int term_width() {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(out_h, &csbi))
        return csbi.srWindow.Right - csbi.srWindow.Left + 1;
    return 80;
}

int term_height() {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(out_h, &csbi))
        return csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    return 24;
}
