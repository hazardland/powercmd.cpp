// MODULE: persist
// Purpose : on-disk persistence — working-dir memory, command history, aliases (load/save/compact)
// Exports : prev_dir last_session_dir | zcmd_dir() history_path() prev_dir_path() aliases_path()
//           load_prev_dir() save_prev_dir() | aliases | load_aliases() write_alias() expand_alias()
//           load_history() append_history() compact_history()
// Depends : common.h, input.h (struct input used by load_history)

static std::string prev_dir;
static std::string last_session_dir;

// Returns %USERPROFILE%\.zcmd\ and creates it if it doesn't exist.
std::string zcmd_dir() {
    wchar_t buf[MAX_PATH];
    GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
    std::string dir = to_utf8(buf) + "\\.zcmd";
    CreateDirectoryW(to_wide(dir).c_str(), NULL);
    return dir + "\\";
}

std::string history_path()  { return zcmd_dir() + "history";  }
std::string prev_dir_path() { return zcmd_dir() + "prev_dir"; }

void load_prev_dir() {
    std::ifstream f(prev_dir_path());
    std::string line;
    if (std::getline(f, line) && !line.empty()) last_session_dir = line;
}

void save_prev_dir() {
    std::string cur = cwd();
    if (cur.empty()) return;
    std::ofstream f(prev_dir_path());
    f << cur << "\n";
}

// ---- aliases ----

static std::unordered_map<std::string, std::string> aliases;

std::string aliases_path() { return zcmd_dir() + "aliases"; }

void load_aliases() {
    std::ifstream f(aliases_path());
    std::string line;
    while (std::getline(f, line)) {
        size_t eq = line.find('=');
        if (eq != std::string::npos && eq > 0)
            aliases[line.substr(0, eq)] = line.substr(eq + 1);
    }
}

// Read-modify-write: reads file, applies change, rewrites.
// Safe with parallel sessions — file is always authoritative.
void write_alias(const std::string& name, const std::string& val) {
    std::unordered_map<std::string, std::string> file_aliases;
    { std::ifstream f(aliases_path()); std::string line;
      while (std::getline(f, line)) {
          size_t eq = line.find('=');
          if (eq != std::string::npos && eq > 0)
              file_aliases[line.substr(0, eq)] = line.substr(eq + 1);
      }
    }
    if (val.empty()) file_aliases.erase(name);
    else             file_aliases[name] = val;
    aliases = file_aliases;
    std::ofstream f(aliases_path());
    for (auto& kv : file_aliases) f << kv.first << "=" << kv.second << "\n";
}

// Expands the first word of line if it matches an alias; appends remaining args.
// Returns empty string if no alias matched.
std::string expand_alias(const std::string& line) {
    size_t sp = line.find(' ');
    std::string name = sp == std::string::npos ? line : line.substr(0, sp);
    std::string namel = name;
    std::transform(namel.begin(), namel.end(), namel.begin(), ::tolower);
    auto it = aliases.find(namel);
    if (it == aliases.end()) return "";
    std::string expanded = it->second;
    if (sp != std::string::npos) expanded += line.substr(sp);
    return expanded;
}

// Reads .history into e.hist and deduplicates, keeping only the last occurrence of each command.
void load_history(input& e) {
    std::ifstream f(history_path());
    std::string line;
    std::vector<std::wstring> raw;
    while (std::getline(f, line)) {
        if (!line.empty()) {
            for (char& c : line) if (c == '\x1f') c = '\n';
            raw.push_back(to_wide(line));
        }
    }
    std::unordered_set<std::wstring> seen;
    for (int i = (int)raw.size() - 1; i >= 0; i--) {
        if (seen.insert(raw[i]).second)
            e.hist.push_back(raw[i]);
    }
    std::reverse(e.hist.begin(), e.hist.end());
}

// Appends a single command to the history file immediately (fish-style: no loss on crash).
// Multiline commands: \n is encoded as \x1F (Unit Separator) so each entry stays one disk line.
void append_history(const std::wstring& cmd) {
    std::string s = to_utf8(cmd);
    for (char& c : s) if (c == '\n') c = '\x1f';
    std::ofstream f(history_path(), std::ios::app);
    f << s << "\n";
}

// Re-reads the history file, deduplicates keeping last occurrence, rewrites.
void compact_history() {
    std::string path = history_path();
    std::vector<std::wstring> raw;
    { std::ifstream f(path); std::string line;
      while (std::getline(f, line)) if (!line.empty()) {
          for (char& c : line) if (c == '\x1f') c = '\n';
          raw.push_back(to_wide(line));
      }
    }
    std::vector<std::wstring> deduped;
    std::unordered_set<std::wstring> seen;
    for (int i = (int)raw.size() - 1; i >= 0; i--)
        if (seen.insert(raw[i]).second) deduped.push_back(raw[i]);
    std::reverse(deduped.begin(), deduped.end());
    std::ofstream f(path);
    for (auto& entry : deduped) {
        std::string s = to_utf8(entry);
        for (char& c : s) if (c == '\n') c = '\x1f';
        f << s << "\n";
    }
}
