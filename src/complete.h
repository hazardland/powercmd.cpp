// MODULE: complete
// Purpose : filesystem tab completion — returns sorted matches for a path prefix, dirs-only mode for cd/ls
// Exports : complete()
// Depends : common.h

// Returns filesystem entries matching prefix* sorted alphabetically; appends "/" to directories.
// dirs_only is set true when completing after "cd" so files are excluded.
std::vector<std::wstring> complete(const std::wstring& prefix, bool dirs_only = false) {
    std::vector<std::wstring> result;
    if (prefix.size() == 2 && iswalpha(prefix[0]) && prefix[1] == L':') {
        result.push_back(prefix + L"/");
        return result;
    }

    std::wstring dir, name;
    size_t sep = prefix.find_last_of(L"\\/");
    if (sep == std::wstring::npos) { dir = L""; name = prefix; }
    else { dir = prefix.substr(0, sep + 1); name = prefix.substr(sep + 1); }

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + name + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return result;
    do {
        std::wstring fname = fd.cFileName;
        if (fname == L"." || fname == L"..") continue;
        bool is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (dirs_only && !is_dir) continue;
        std::wstring full = dir + fname;
        if (is_dir) full += L"/";
        std::replace(full.begin(), full.end(), L'\\', L'/');
        result.push_back(full);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(result.begin(), result.end());
    return result;
}
