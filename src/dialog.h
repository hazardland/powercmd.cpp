#pragma once

#include <cstdint>

// MODULE: dialog
// Purpose : shared VT dialog state, palette, rendering, and key handling for fullscreen tools
// Exports : DialogPalette DialogButton DialogState DialogEvent | dialog_open_*() dialog_close() dialog_draw() dialog_overlay_draw() dialog_handle_key() dialog_style_vt()
// Depends : common.h

enum DIALOG_LAYOUT {
    DIALOG_LAYOUT_MESSAGE = 0,
    DIALOG_LAYOUT_INPUT,
    DIALOG_LAYOUT_PROGRESS,
};

enum DIALOG_BUTTON_ROLE {
    DIALOG_BUTTON_CONFIRM = 0,
    DIALOG_BUTTON_CAUTION,
    DIALOG_BUTTON_CANCEL,
};

enum DIALOG_EVENT_KIND {
    DIALOG_EVENT_NONE = 0,
    DIALOG_EVENT_REDRAW,
    DIALOG_EVENT_SUBMIT,
};

enum DIALOG_STYLE : WORD {
    DIALOG_STYLE_NONE = 0x4000,
    DIALOG_STYLE_BORDER,
    DIALOG_STYLE_FILL,
    DIALOG_STYLE_TITLE,
    DIALOG_STYLE_SUMMARY,
    DIALOG_STYLE_DETAIL,
    DIALOG_STYLE_LABEL,
    DIALOG_STYLE_INPUT,
    DIALOG_STYLE_INPUT_SELECTED,
    DIALOG_STYLE_INPUT_CURSOR,
    DIALOG_STYLE_PROGRESS_TEXT,
    DIALOG_STYLE_PROGRESS_BAR,
    DIALOG_STYLE_PROGRESS_FILL,
    DIALOG_STYLE_CONFIRM_KEY,
    DIALOG_STYLE_CONFIRM_TEXT,
    DIALOG_STYLE_CAUTION_KEY,
    DIALOG_STYLE_CAUTION_TEXT,
    DIALOG_STYLE_CANCEL_KEY,
    DIALOG_STYLE_CANCEL_TEXT,
};

struct DialogPalette {
    int border_fg = 75;
    int border_bg = -1;
    int fill_fg = 229;
    int fill_bg = -1;
    int title_fg = 75;
    int title_bg = -1;
    int summary_fg = 229;
    int summary_bg = -1;
    int detail_fg = 229;
    int detail_bg = -1;
    int label_fg = 226;
    int label_bg = -1;
    int input_fg = 75;
    int input_bg = 236;
    int input_selected_fg = 16;
    int input_selected_bg = 75;
    int input_cursor_fg = 16;
    int input_cursor_bg = 226;
    int progress_text_fg = 229;
    int progress_text_bg = -1;
    int progress_bar_fg = 75;
    int progress_bar_bg = 236;
    int progress_fill_fg = 16;
    int progress_fill_bg = 226;
    int confirm_key_fg = 16;
    int confirm_key_bg = 75;
    int confirm_text_fg = 250;
    int confirm_text_bg = -1;
    int caution_key_fg = 16;
    int caution_key_bg = 203;
    int caution_text_fg = 250;
    int caution_text_bg = -1;
    int cancel_key_fg = 16;
    int cancel_key_bg = 240;
    int cancel_text_fg = 250;
    int cancel_text_bg = -1;
};

struct DialogButton {
    int id = 0;
    std::wstring key;
    std::wstring text;
    DIALOG_BUTTON_ROLE role = DIALOG_BUTTON_CONFIRM;
    WORD vk = 0;
    wchar_t ch = 0;
    bool ctrl = false;
    bool shift = false;
};

static DialogButton dialog_button(int id, const std::wstring& key, const std::wstring& text,
    DIALOG_BUTTON_ROLE role, WORD vk, bool ctrl = false, bool shift = false, wchar_t ch = 0) {
    DialogButton button;
    button.id = id;
    button.key = key;
    button.text = text;
    button.role = role;
    button.vk = vk;
    button.ch = ch;
    button.ctrl = ctrl;
    button.shift = shift;
    return button;
}

struct DialogState {
    bool visible = false;
    DIALOG_LAYOUT layout = DIALOG_LAYOUT_MESSAGE;
    std::wstring title;
    std::wstring summary;
    std::wstring detail;
    std::wstring input_label;
    std::wstring input_value;
    int input_cursor = 0;
    bool input_selected = false;
    uint64_t progress_current = 0;
    uint64_t progress_total = 0;
    std::wstring progress_footer;
    std::vector<DialogButton> buttons;
    DialogPalette palette;
};

struct DialogEvent {
    DIALOG_EVENT_KIND kind = DIALOG_EVENT_NONE;
    int button_id = 0;
    std::wstring input_value;
};

static DialogPalette dialog_palette_default() {
    return DialogPalette();
}

static DialogPalette dialog_palette_warning() {
    DialogPalette palette = dialog_palette_default();
    palette.border_fg = 203;
    palette.title_fg = 203;
    palette.title_bg = -1;
    return palette;
}

static DialogPalette dialog_palette_error() {
    DialogPalette palette = dialog_palette_default();
    palette.border_fg = 203;
    palette.title_fg = 203;
    palette.fill_bg = 52;
    palette.fill_fg = 229;
    palette.summary_bg = 52;
    palette.detail_bg = 52;
    palette.progress_text_bg = 52;
    return palette;
}

static void dialog_close(DialogState& dialog) {
    dialog = DialogState();
}

static void dialog_open_message(DialogState& dialog, const std::wstring& title, const std::wstring& summary,
    const std::wstring& detail, const std::vector<DialogButton>& buttons, const DialogPalette& palette = dialog_palette_default()) {
    dialog.visible = true;
    dialog.layout = DIALOG_LAYOUT_MESSAGE;
    dialog.title = title;
    dialog.summary = summary;
    dialog.detail = detail;
    dialog.buttons = buttons;
    dialog.palette = palette;
    dialog.input_label.clear();
    dialog.input_value.clear();
    dialog.input_cursor = 0;
    dialog.input_selected = false;
    dialog.progress_current = 0;
    dialog.progress_total = 0;
    dialog.progress_footer.clear();
}

static void dialog_open_input(DialogState& dialog, const std::wstring& title, const std::wstring& summary,
    const std::wstring& detail, const std::wstring& input_label, const std::wstring& input_value,
    const std::vector<DialogButton>& buttons, const DialogPalette& palette = dialog_palette_default()) {
    dialog.visible = true;
    dialog.layout = DIALOG_LAYOUT_INPUT;
    dialog.title = title;
    dialog.summary = summary;
    dialog.detail = detail;
    dialog.input_label = input_label;
    dialog.input_value = input_value;
    dialog.input_cursor = (int)dialog.input_value.size();
    dialog.input_selected = !dialog.input_value.empty();
    dialog.buttons = buttons;
    dialog.palette = palette;
    dialog.progress_current = 0;
    dialog.progress_total = 0;
    dialog.progress_footer.clear();
}

static void dialog_open_progress(DialogState& dialog, const std::wstring& title, const std::wstring& summary,
    const std::wstring& detail, uint64_t current, uint64_t total,
    const std::wstring& footer = L"Working...", const DialogPalette& palette = dialog_palette_default()) {
    dialog.visible = true;
    dialog.layout = DIALOG_LAYOUT_PROGRESS;
    dialog.title = title;
    dialog.summary = summary;
    dialog.detail = detail;
    dialog.palette = palette;
    dialog.buttons.clear();
    dialog.input_label.clear();
    dialog.input_value.clear();
    dialog.input_cursor = 0;
    dialog.input_selected = false;
    dialog.progress_current = current;
    dialog.progress_total = total;
    dialog.progress_footer = footer;
}

static std::wstring dialog_fit(const std::wstring& s, int width) {
    if (width <= 0) return L"";
    if ((int)s.size() <= width) return s;
    if (width <= 3) return std::wstring(width, L'.');
    return s.substr(0, width - 3) + L"...";
}

static void dialog_put(std::vector<wchar_t>& chars, std::vector<WORD>& attrs,
    int width, int height, int x, int y, wchar_t ch, WORD attr) {
    if (x < 0 || y < 0 || x >= width || y >= height) return;
    size_t idx = (size_t)y * width + x;
    chars[idx] = ch;
    attrs[idx] = attr;
}

static void dialog_fill(std::vector<wchar_t>& chars, std::vector<WORD>& attrs,
    int width, int height, int x, int y, int len, wchar_t ch, WORD attr) {
    for (int i = 0; i < len; i++)
        dialog_put(chars, attrs, width, height, x + i, y, ch, attr);
}

static void dialog_text(std::vector<wchar_t>& chars, std::vector<WORD>& attrs,
    int width, int height, int x, int y, const std::wstring& text, WORD attr) {
    for (int i = 0; i < (int)text.size(); i++)
        dialog_put(chars, attrs, width, height, x + i, y, text[i], attr);
}

static void dialog_box(std::vector<wchar_t>& chars, std::vector<WORD>& attrs,
    int width, int height, int left, int top, int box_w, int box_h, WORD border_attr, WORD fill_attr) {
    if (box_w <= 1 || box_h <= 1) return;
    for (int y = top; y < top + box_h; y++)
        dialog_fill(chars, attrs, width, height, left, y, box_w, L' ', fill_attr);

    dialog_put(chars, attrs, width, height, left, top, L'\x250C', border_attr);
    dialog_put(chars, attrs, width, height, left + box_w - 1, top, L'\x2510', border_attr);
    dialog_put(chars, attrs, width, height, left, top + box_h - 1, L'\x2514', border_attr);
    dialog_put(chars, attrs, width, height, left + box_w - 1, top + box_h - 1, L'\x2518', border_attr);
    if (box_w > 2) {
        dialog_fill(chars, attrs, width, height, left + 1, top, box_w - 2, L'\x2500', border_attr);
        dialog_fill(chars, attrs, width, height, left + 1, top + box_h - 1, box_w - 2, L'\x2500', border_attr);
    }
    for (int y = top + 1; y < top + box_h - 1; y++) {
        dialog_put(chars, attrs, width, height, left, y, L'\x2502', border_attr);
        dialog_put(chars, attrs, width, height, left + box_w - 1, y, L'\x2502', border_attr);
    }
}

static std::wstring dialog_input_tail(const std::wstring& value, int width, int cursor, int& start_out) {
    start_out = 0;
    if (width <= 0) return L"";
    if ((int)value.size() <= width) return value;
    start_out = std::max(0, cursor - width + 1);
    if (start_out + width > (int)value.size())
        start_out = (int)value.size() - width;
    return value.substr(start_out, width);
}

static WORD dialog_button_key_style(DIALOG_BUTTON_ROLE role) {
    if (role == DIALOG_BUTTON_CAUTION) return DIALOG_STYLE_CAUTION_KEY;
    if (role == DIALOG_BUTTON_CANCEL) return DIALOG_STYLE_CANCEL_KEY;
    return DIALOG_STYLE_CONFIRM_KEY;
}

static WORD dialog_button_text_style(DIALOG_BUTTON_ROLE role) {
    if (role == DIALOG_BUTTON_CAUTION) return DIALOG_STYLE_CAUTION_TEXT;
    if (role == DIALOG_BUTTON_CANCEL) return DIALOG_STYLE_CANCEL_TEXT;
    return DIALOG_STYLE_CONFIRM_TEXT;
}

static int dialog_button_draw(std::vector<wchar_t>& chars, std::vector<WORD>& attrs,
    int width, int height, int x, int y, const DialogButton& button) {
    if (x >= width) return x;

    WORD key_attr = dialog_button_key_style(button.role);
    WORD text_attr = dialog_button_text_style(button.role);
    std::wstring tail = ui_text_tail(button.key, button.text);

    dialog_text(chars, attrs, width, height, x, y, L"[ ", text_attr);
    x += 2;
    dialog_text(chars, attrs, width, height, x, y, button.key, key_attr);
    x += (int)button.key.size();
    if (!tail.empty()) {
        dialog_text(chars, attrs, width, height, x, y, tail, text_attr);
        x += (int)tail.size();
    }
    dialog_text(chars, attrs, width, height, x, y, L" ]", text_attr);
    x += 2;
    if (x < width) {
        dialog_put(chars, attrs, width, height, x, y, L' ', DIALOG_STYLE_FILL);
        x++;
    }
    return x;
}

static bool dialog_is_style(WORD attr) {
    return attr >= DIALOG_STYLE_NONE && attr <= DIALOG_STYLE_CANCEL_TEXT;
}

static std::string dialog_vt_fg_bg(int fg, int bg) {
    std::string vt;
    if (bg >= 0) {
        vt += "\x1b[48;5;";
        vt += std::to_string(bg);
        vt += 'm';
    } else {
        vt += "\x1b[49m";
    }
    if (fg >= 0) {
        vt += "\x1b[38;5;";
        vt += std::to_string(fg);
        vt += 'm';
    } else {
        vt += "\x1b[39m";
    }
    return vt;
}

static std::string dialog_style_vt(WORD attr, const DialogPalette& palette) {
    switch (attr) {
    case DIALOG_STYLE_BORDER:        return dialog_vt_fg_bg(palette.border_fg, palette.border_bg);
    case DIALOG_STYLE_FILL:          return dialog_vt_fg_bg(palette.fill_fg, palette.fill_bg);
    case DIALOG_STYLE_TITLE:         return dialog_vt_fg_bg(palette.title_fg, palette.title_bg);
    case DIALOG_STYLE_SUMMARY:       return dialog_vt_fg_bg(palette.summary_fg, palette.summary_bg);
    case DIALOG_STYLE_DETAIL:        return dialog_vt_fg_bg(palette.detail_fg, palette.detail_bg);
    case DIALOG_STYLE_LABEL:         return dialog_vt_fg_bg(palette.label_fg, palette.label_bg);
    case DIALOG_STYLE_INPUT:         return dialog_vt_fg_bg(palette.input_fg, palette.input_bg);
    case DIALOG_STYLE_INPUT_SELECTED:return dialog_vt_fg_bg(palette.input_selected_fg, palette.input_selected_bg);
    case DIALOG_STYLE_INPUT_CURSOR:  return dialog_vt_fg_bg(palette.input_cursor_fg, palette.input_cursor_bg);
    case DIALOG_STYLE_PROGRESS_TEXT: return dialog_vt_fg_bg(palette.progress_text_fg, palette.progress_text_bg);
    case DIALOG_STYLE_PROGRESS_BAR:  return dialog_vt_fg_bg(palette.progress_bar_fg, palette.progress_bar_bg);
    case DIALOG_STYLE_PROGRESS_FILL: return dialog_vt_fg_bg(palette.progress_fill_fg, palette.progress_fill_bg);
    case DIALOG_STYLE_CONFIRM_KEY:   return dialog_vt_fg_bg(palette.confirm_key_fg, palette.confirm_key_bg);
    case DIALOG_STYLE_CONFIRM_TEXT:  return dialog_vt_fg_bg(palette.confirm_text_fg, palette.confirm_text_bg);
    case DIALOG_STYLE_CAUTION_KEY:   return dialog_vt_fg_bg(palette.caution_key_fg, palette.caution_key_bg);
    case DIALOG_STYLE_CAUTION_TEXT:  return dialog_vt_fg_bg(palette.caution_text_fg, palette.caution_text_bg);
    case DIALOG_STYLE_CANCEL_KEY:    return dialog_vt_fg_bg(palette.cancel_key_fg, palette.cancel_key_bg);
    case DIALOG_STYLE_CANCEL_TEXT:   return dialog_vt_fg_bg(palette.cancel_text_fg, palette.cancel_text_bg);
    default:                         return "";
    }
}

static bool dialog_overlay_read_size(int& width, int& height) {
    CONSOLE_SCREEN_BUFFER_INFO csbi = {};
    if (!GetConsoleScreenBufferInfo(out_h, &csbi)) return false;
    width = csbi.srWindow.Right - csbi.srWindow.Left + 1;
    height = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    return width > 0 && height > 0;
}

static void dialog_overlay_append_cursor_move(std::string& outbuf, int x, int y) {
    outbuf += "\x1b[";
    outbuf += std::to_string(y + 1);
    outbuf += ';';
    outbuf += std::to_string(x + 1);
    outbuf += 'H';
}

static void dialog_draw(std::vector<wchar_t>& chars, std::vector<WORD>& attrs, int width, int height, const DialogState& dialog) {
    if (!dialog.visible) return;

    int dialog_w = std::min(width, std::max(36, std::min(width - 4, 64)));
    int dialog_h = 7;
    if (dialog.layout == DIALOG_LAYOUT_INPUT) dialog_h = 8;
    if (dialog_w <= 2 || dialog_h <= 2) return;

    int left = std::max(0, (width - dialog_w) / 2);
    int top  = std::max(0, (height - dialog_h) / 2);

    // Clear a moat around the dialog so underlying tool chrome stays visually separated.
    int clear_left = std::max(0, left - 2);
    int clear_top = std::max(0, top - 2);
    int clear_right = std::min(width - 1, left + dialog_w + 1);
    int clear_bottom = std::min(height - 1, top + dialog_h + 1);
    for (int y = clear_top; y <= clear_bottom; y++)
        dialog_fill(chars, attrs, width, height, clear_left, y, clear_right - clear_left + 1, L' ', DIALOG_STYLE_FILL);

    dialog_box(chars, attrs, width, height, left, top, dialog_w, dialog_h, DIALOG_STYLE_BORDER, DIALOG_STYLE_FILL);

    dialog_text(chars, attrs, width, height, left + 2, top, dialog_fit(dialog.title, dialog_w - 4), DIALOG_STYLE_TITLE);
    dialog_text(chars, attrs, width, height, left + 2, top + 2, dialog_fit(dialog.summary, dialog_w - 4), DIALOG_STYLE_SUMMARY);
    if (!dialog.detail.empty()) {
        dialog_text(chars, attrs, width, height, left + 2, top + 3, dialog_fit(dialog.detail, dialog_w - 4), DIALOG_STYLE_DETAIL);
    }

    int footer_x = left + 2;
    int footer_y = top + dialog_h - 2;
    if (dialog.layout == DIALOG_LAYOUT_INPUT) {
        dialog_text(chars, attrs, width, height, left + 2, top + 3, dialog_fit(dialog.input_label, dialog_w - 4), DIALOG_STYLE_LABEL);

        int input_x = left + 2;
        int input_y = top + 4;
        int input_w = std::max(1, dialog_w - 4);
        dialog_fill(chars, attrs, width, height, input_x, input_y, input_w, L' ', DIALOG_STYLE_INPUT);

        int start = 0;
        std::wstring visible = dialog_input_tail(dialog.input_value, input_w, dialog.input_cursor, start);
        WORD input_attr = dialog.input_selected ? DIALOG_STYLE_INPUT_SELECTED : DIALOG_STYLE_INPUT;
        dialog_text(chars, attrs, width, height, input_x, input_y, visible, input_attr);

        if (!dialog.input_selected) {
            int cursor_x = input_x + std::max(0, dialog.input_cursor - start);
            if (cursor_x >= input_x + input_w) cursor_x = input_x + input_w - 1;
            wchar_t cursor_ch = L' ';
            if (dialog.input_cursor >= start && dialog.input_cursor < start + (int)visible.size())
                cursor_ch = visible[dialog.input_cursor - start];
            dialog_put(chars, attrs, width, height, cursor_x, input_y, cursor_ch, DIALOG_STYLE_INPUT_CURSOR);
        }
    } else if (dialog.layout == DIALOG_LAYOUT_PROGRESS) {
        int bar_x = left + 2;
        int bar_y = top + 4;
        int bar_w = std::max(8, dialog_w - 4);
        uint64_t total = std::max<uint64_t>(1, dialog.progress_total);
        uint64_t current = std::min(dialog.progress_current, total);
        int filled = (int)((bar_w * current) / total);
        dialog_fill(chars, attrs, width, height, bar_x, bar_y, bar_w, L' ', DIALOG_STYLE_PROGRESS_BAR);
        if (filled > 0)
            dialog_fill(chars, attrs, width, height, bar_x, bar_y, filled, L' ', DIALOG_STYLE_PROGRESS_FILL);

        std::wstring percent = std::to_wstring((current * 100) / total) + L"%";
        int percent_x = bar_x + std::max(0, (bar_w - (int)percent.size()) / 2);
        dialog_text(chars, attrs, width, height, percent_x, bar_y, percent, DIALOG_STYLE_PROGRESS_TEXT);
        dialog_text(chars, attrs, width, height, left + 2, footer_y, dialog_fit(dialog.progress_footer.empty() ? L"Working..." : dialog.progress_footer, dialog_w - 4), DIALOG_STYLE_PROGRESS_TEXT);
        return;
    }

    for (const DialogButton& button : dialog.buttons)
        footer_x = dialog_button_draw(chars, attrs, width, height, footer_x, footer_y, button);
}

static void dialog_overlay_draw(const DialogState& dialog) {
    if (!dialog.visible) return;

    int width = 0, height = 0;
    if (!dialog_overlay_read_size(width, height)) return;

    std::vector<wchar_t> chars((size_t)width * height, L' ');
    std::vector<WORD> attrs((size_t)width * height, 0);
    dialog_draw(chars, attrs, width, height, dialog);

    std::string frame;
    for (int y = 0; y < height; y++) {
        int first = -1;
        int last = -1;
        size_t row_off = (size_t)y * width;
        for (int x = 0; x < width; x++) {
            if (!dialog_is_style(attrs[row_off + x])) continue;
            if (first < 0) first = x;
            last = x;
        }
        if (first < 0) continue;

        dialog_overlay_append_cursor_move(frame, first, y);
        WORD style = (WORD)-1;
        std::wstring run;
        for (int x = first; x <= last; x++) {
            size_t idx = row_off + x;
            WORD next = attrs[idx];
            if (next != style) {
                if (!run.empty()) {
                    frame += to_utf8(run);
                    run.clear();
                }
                frame += dialog_style_vt(next, dialog.palette);
                style = next;
            }
            run.push_back(chars[idx]);
        }
        if (!run.empty())
            frame += to_utf8(run);
    }
    frame += RESET;
    out(frame);
}

static bool dialog_button_matches(const DialogButton& button, WORD vk, wchar_t ch, bool ctrl, bool shift) {
    if (button.ch) {
        if (!ch) return false;
        if (!!button.ctrl != ctrl || !!button.shift != shift) return false;
        return towlower(button.ch) == towlower(ch);
    }
    if (!button.vk) return false;
    return button.vk == vk && !!button.ctrl == ctrl && !!button.shift == shift;
}

// Shared dialog key handling stops at UI intent: it reports submit/redraw plus the pressed button id.
// The caller remains responsible for interpreting that id and applying tool-specific behavior.
static DialogEvent dialog_handle_key(DialogState& dialog, WORD vk, wchar_t ch, bool ctrl, bool shift) {
    DialogEvent event;
    if (!dialog.visible) return event;

    for (const DialogButton& button : dialog.buttons) {
        if (!dialog_button_matches(button, vk, ch, ctrl, shift))
            continue;
        event.kind = DIALOG_EVENT_SUBMIT;
        event.button_id = button.id;
        event.input_value = dialog.input_value;
        return event;
    }

    if (dialog.layout != DIALOG_LAYOUT_INPUT)
        return event;

    if (dialog.input_selected) {
        if (!ctrl && vk == VK_LEFT) {
            dialog.input_selected = false;
            dialog.input_cursor = 0;
            event.kind = DIALOG_EVENT_REDRAW;
            return event;
        }
        if (!ctrl && vk == VK_RIGHT) {
            dialog.input_selected = false;
            dialog.input_cursor = (int)dialog.input_value.size();
            event.kind = DIALOG_EVENT_REDRAW;
            return event;
        }
        if (!ctrl && vk == VK_HOME) {
            dialog.input_selected = false;
            dialog.input_cursor = 0;
            event.kind = DIALOG_EVENT_REDRAW;
            return event;
        }
        if (!ctrl && vk == VK_END) {
            dialog.input_selected = false;
            dialog.input_cursor = (int)dialog.input_value.size();
            event.kind = DIALOG_EVENT_REDRAW;
            return event;
        }
        if (!ctrl && (vk == VK_BACK || vk == VK_DELETE || (ch >= 32 && ch != 127))) {
            dialog.input_value.clear();
            dialog.input_cursor = 0;
            dialog.input_selected = false;
            if (ch >= 32 && ch != 127) {
                dialog.input_value.push_back(ch);
                dialog.input_cursor = 1;
            }
            event.kind = DIALOG_EVENT_REDRAW;
            return event;
        }
    }

    if (!ctrl && vk == VK_LEFT) {
        dialog.input_selected = false;
        dialog.input_cursor = std::max(0, dialog.input_cursor - 1);
        event.kind = DIALOG_EVENT_REDRAW;
        return event;
    }
    if (!ctrl && vk == VK_RIGHT) {
        dialog.input_selected = false;
        dialog.input_cursor = std::min((int)dialog.input_value.size(), dialog.input_cursor + 1);
        event.kind = DIALOG_EVENT_REDRAW;
        return event;
    }
    if (!ctrl && vk == VK_HOME) {
        dialog.input_selected = false;
        dialog.input_cursor = 0;
        event.kind = DIALOG_EVENT_REDRAW;
        return event;
    }
    if (!ctrl && vk == VK_END) {
        dialog.input_selected = false;
        dialog.input_cursor = (int)dialog.input_value.size();
        event.kind = DIALOG_EVENT_REDRAW;
        return event;
    }
    if (!ctrl && vk == VK_BACK) {
        dialog.input_selected = false;
        if (dialog.input_cursor > 0) {
            dialog.input_value.erase(dialog.input_cursor - 1, 1);
            dialog.input_cursor--;
            event.kind = DIALOG_EVENT_REDRAW;
        }
        return event;
    }
    if (!ctrl && vk == VK_DELETE) {
        dialog.input_selected = false;
        if (dialog.input_cursor < (int)dialog.input_value.size()) {
            dialog.input_value.erase(dialog.input_cursor, 1);
            event.kind = DIALOG_EVENT_REDRAW;
        }
        return event;
    }
    if (!ctrl && ch >= 32 && ch != 127) {
        dialog.input_selected = false;
        dialog.input_value.insert(dialog.input_cursor, 1, ch);
        dialog.input_cursor++;
        event.kind = DIALOG_EVENT_REDRAW;
        return event;
    }

    return event;
}
