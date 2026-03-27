// MODULE: cat
// Purpose : file printer with syntax highlighting — delegates to highlight.h, image.h, video.h
// Exports : cat()
// Depends : common.h, highlight.h, image.h (cat_image is_image_ext), video.h (cat_video is_video_ext)

// Reads a file and prints it with syntax highlighting. filter is an optional case-insensitive
// substring — only lines containing it are shown (like cat file | grep word).
int cat(const std::string& path, const std::string& filter = "") {
    std::string p = normalize_path(path);
    if (is_image_ext(p)) return cat_image(p);
    if (is_video_ext(p)) return cat_video(p);
    std::ifstream f(p);
    if (!f.is_open()) { out("cat: cannot open '" + path + "'\r\n"); return 1; }
    lang l = detect_lang(p);
    std::string flt = filter;
    std::transform(flt.begin(), flt.end(), flt.begin(), ::tolower);
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back()=='\r') line.pop_back();
        if (!flt.empty()) {
            std::string ll = line;
            std::transform(ll.begin(), ll.end(), ll.begin(), ::tolower);
            if (ll.find(flt) == std::string::npos) continue;
        }
        out(colorize_line(line, l) + "\r\n");
    }
    return 0;
}
