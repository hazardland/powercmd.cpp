// MODULE: common
// Purpose : system includes, color macros, version macro, global console handles, UTF-8/wide converters, out/err writers, path utils, clipboard
// Exports : GRAY BLUE RED YELLOW GREEN RESET VERSION | out_h err_h in_h orig_in_mode | to_utf8() to_wide() out() err() | normalize_path() clipboard_get() clipboard_set()
// Depends : (none — must be first include)

#include <windows.h>
#include <string>
#include <algorithm>
#include <vector>
#include <cstdio>
#include <cwctype>
#include <fstream>
#include <sstream>
#include <unordered_set>
#include <unordered_map>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstring>
#include <cmath>

#ifndef VERSION_MINOR
#define VERSION_MINOR 0
#endif
#define STR_(x) #x
#define STR(x) STR_(x)
#define VERSION "0.0." STR(VERSION_MINOR)

#define GRAY   "\x1b[38;2;142;150;166m"
#define BLUE   "\x1b[38;5;75m"
#define RED    "\x1b[38;5;203m"
#define YELLOW "\x1b[38;5;229m"
#define BRIGHT_YELLOW "\x1b[38;5;226m"
#define MAGENTA "\x1b[1;35m"
#define GREEN  "\x1b[38;2;76;208;101m"
#define SILVER "\x1b[38;5;250m"
#define ARCHIVE_RED "\x1b[38;5;210m"
#define RESET  "\x1b[0m"

// Console handles initialised once in main and used by all I/O functions.
static HANDLE out_h;
static HANDLE err_h;
static HANDLE in_h;
// Saved input mode; restored before spawning child processes so they receive normal input handling.
static DWORD  orig_in_mode;

enum ENTRY_COLOR_KIND {
    ENTRY_COLOR_FILE = 0,
    ENTRY_COLOR_DIR,
    ENTRY_COLOR_EXE,
    ENTRY_COLOR_ARCHIVE,
    ENTRY_COLOR_IMAGE,
    ENTRY_COLOR_MEDIA,
    ENTRY_COLOR_HIDDEN,
};

// Convert wide string to UTF-8; used when writing wstring data to the console or history file.
std::string to_utf8(const std::wstring& ws) {
    if (ws.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, NULL, 0, NULL, NULL);
    std::string s(n - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, &s[0], n, NULL, NULL);
    return s;
}

// Convert UTF-8 string to wide; used before any Win32 API call that requires a wchar_t path or command.
std::wstring to_wide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, NULL, 0);
    std::wstring ws(n - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &ws[0], n);
    return ws;
}

// Write UTF-8 bytes directly to stdout handle; bypasses C runtime buffering so ANSI sequences render immediately.
void out(const std::string& s) {
    DWORD w;
    WriteFile(out_h, s.c_str(), (DWORD)s.size(), &w, NULL);
}

// Write to stderr handle; used for error messages that should not mix with stdout output.
void err(const std::string& s) {
    DWORD w;
    WriteFile(err_h, s.c_str(), (DWORD)s.size(), &w, NULL);
}

static ENTRY_COLOR_KIND entry_color_kind(const std::wstring& name, bool is_dir, bool is_hidden) {
    if (is_hidden) return ENTRY_COLOR_HIDDEN;
    if (is_dir) return ENTRY_COLOR_DIR;

    size_t dot = name.rfind(L'.');
    if (dot == std::wstring::npos) return ENTRY_COLOR_FILE;

    std::wstring ext = name.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);

    if (ext == L".exe" || ext == L".bat" || ext == L".cmd" || ext == L".ps1" || ext == L".msi")
        return ENTRY_COLOR_EXE;

    if (ext == L".zip" || ext == L".tar" || ext == L".gz" || ext == L".tgz" ||
        ext == L".bz2" || ext == L".xz" || ext == L".7z" || ext == L".rar" ||
        ext == L".z" || ext == L".lz" || ext == L".lzma" || ext == L".zst" ||
        ext == L".deb" || ext == L".rpm" || ext == L".cab" || ext == L".iso")
        return ENTRY_COLOR_ARCHIVE;

    if (ext == L".jpg" || ext == L".jpeg" || ext == L".png" || ext == L".gif" ||
        ext == L".bmp" || ext == L".tif" || ext == L".tiff" || ext == L".svg" ||
        ext == L".webp" || ext == L".ico" || ext == L".raw" || ext == L".heic")
        return ENTRY_COLOR_IMAGE;

    if (ext == L".mp3" || ext == L".wav" || ext == L".ogg" || ext == L".flac" ||
        ext == L".aac" || ext == L".m4a" || ext == L".wma" ||
        ext == L".mp4" || ext == L".mkv" || ext == L".avi" || ext == L".mov" ||
        ext == L".wmv" || ext == L".flv" || ext == L".webm" || ext == L".m4v")
        return ENTRY_COLOR_MEDIA;

    return ENTRY_COLOR_FILE;
}

static bool ui_key_matches_text_prefix(const std::wstring& key, const std::wstring& text) {
    return key.size() == 1 && !text.empty() && towlower(key[0]) == towlower(text[0]);
}

static std::wstring ui_text_tail(const std::wstring& key, const std::wstring& text) {
    if (ui_key_matches_text_prefix(key, text))
        return text.substr(1);
    return text;
}

// Strip surrounding quotes and normalize forward slashes to backslashes.
std::string normalize_path(const std::string& path) {
    std::string p = path;
    if (p.size() >= 2 && p.front() == '"' && p.back() == '"')
        p = p.substr(1, p.size() - 2);
    std::replace(p.begin(), p.end(), '/', '\\');
    return p;
}

// Read CF_UNICODETEXT from the clipboard; returns empty string if unavailable.
std::wstring clipboard_get() {
    std::wstring result;
    if (!OpenClipboard(NULL)) return result;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        wchar_t* txt = static_cast<wchar_t*>(GlobalLock(h));
        if (txt) { result = txt; GlobalUnlock(h); }
    }
    CloseClipboard();
    return result;
}

// Write a wide string to the clipboard as CF_UNICODETEXT.
void clipboard_set(const std::wstring& text) {
    if (!OpenClipboard(NULL)) return;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (hg) {
        wchar_t* dst = static_cast<wchar_t*>(GlobalLock(hg));
        if (dst) { memcpy(dst, text.c_str(), bytes); GlobalUnlock(hg); }
        SetClipboardData(CF_UNICODETEXT, hg);
    }
    CloseClipboard();
}
