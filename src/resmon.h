// MODULE: resmon
// Purpose : fullscreen resource monitor for CPU, memory, power, and network
// Exports : resmon_cmd()
// Depends : common.h, terminal.h, info.h

#include <iphlpapi.h>
#include <dxgi1_4.h>
#include <powerbase.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <winternl.h>

static const char* RESMON_CYAN = "\x1b[38;5;51m";

struct resmon_net_t {
    std::string name;
    ULONG64 rx_bytes_per_sec = 0;
    ULONG64 tx_bytes_per_sec = 0;
    ULONG64 link_bits_per_sec = 0;
};

struct resmon_gpu_t {
    std::string name;
    uint64_t luid_key = 0;
    double usage_pct = 0.0;
    ULONGLONG used_bytes = 0;
    ULONGLONG total_bytes = 0;
    ULONGLONG shared_bytes = 0;
    ULONGLONG dedicated_bytes = 0;
};

struct resmon_snapshot_t {
    double cpu = 0.0;
    DWORD logical_cpus = 0;
    std::vector<double> core_cpu;
    ULONGLONG uptime_ms = 0;
    DWORD memory_load = 0;
    ULONGLONG memory_total = 0;
    ULONGLONG memory_avail = 0;
    bool has_battery = false;
    bool charging = false;
    bool saver_on = false;
    BYTE ac_line = 255;
    BYTE battery_percent = 255;
    std::vector<resmon_gpu_t> gpus;
    std::vector<resmon_net_t> nets;
};

struct resmon_state_t {
    bool cpu_ready = false;
    FILETIME prev_idle = {};
    FILETIME prev_kernel = {};
    FILETIME prev_user = {};
    bool core_ready = false;
    std::vector<SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION> prev_core_perf;
    bool gpu_query_init = false;
    bool gpu_query_live = false;
    PDH_HQUERY gpu_query = nullptr;
    PDH_HCOUNTER gpu_counter = nullptr;
    bool gpu_mem_query_init = false;
    PDH_HQUERY gpu_mem_query = nullptr;
    PDH_HCOUNTER gpu_mem_shared_counter = nullptr;
    PDH_HCOUNTER gpu_mem_dedicated_counter = nullptr;
    PDH_HCOUNTER gpu_mem_total_counter = nullptr;
    ULONGLONG last_gpu_tick = 0;
    std::unordered_map<uint64_t, double> last_gpu_loads;
    ULONGLONG prev_net_tick = 0;
    std::unordered_map<ULONG, std::pair<ULONG64, ULONG64>> prev_net_bytes;
    std::vector<double> cpu_history;
    std::vector<std::vector<double>> core_history;
    std::vector<double> memory_history;
    std::vector<double> battery_history;
    std::unordered_map<uint64_t, std::vector<double>> gpu_history;
    std::unordered_map<std::string, std::vector<double>> net_history;
};

typedef NTSTATUS (NTAPI *resmon_nt_query_system_information_t)(
    SYSTEM_INFORMATION_CLASS, PVOID, ULONG, PULONG);

static uint64_t resmon_ft64(FILETIME ft) {
    return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

static uint64_t resmon_luid_key(LUID luid) {
    return ((uint64_t)(uint32_t)luid.HighPart << 32) | luid.LowPart;
}

static bool resmon_parse_hex_u32(const std::wstring& text, uint32_t& value) {
    if (text.empty()) return false;
    wchar_t* end = nullptr;
    unsigned long parsed = wcstoul(text.c_str(), &end, 16);
    if (!end || *end != 0) return false;
    value = (uint32_t)parsed;
    return true;
}

static bool resmon_parse_gpu_luid(const std::wstring& instance, uint64_t& key_out) {
    size_t pos = instance.find(L"luid_0x");
    if (pos == std::wstring::npos) return false;
    pos += 7;
    size_t mid = instance.find(L"_0x", pos);
    if (mid == std::wstring::npos) return false;
    size_t end = instance.find(L'_', mid + 3);
    std::wstring high_text = instance.substr(pos, mid - pos);
    std::wstring low_text = instance.substr(mid + 3, end == std::wstring::npos ? std::wstring::npos : end - (mid + 3));
    uint32_t high = 0;
    uint32_t low = 0;
    if (!resmon_parse_hex_u32(high_text, high)) return false;
    if (!resmon_parse_hex_u32(low_text, low)) return false;
    key_out = ((uint64_t)high << 32) | low;
    return true;
}

static void resmon_ensure_gpu_query(resmon_state_t& state) {
    if (state.gpu_query_init) return;
    state.gpu_query_init = true;
    if (PdhOpenQueryW(nullptr, 0, &state.gpu_query) != ERROR_SUCCESS)
        return;
    if (PdhAddEnglishCounterW(state.gpu_query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &state.gpu_counter) != ERROR_SUCCESS) {
        PdhCloseQuery(state.gpu_query);
        state.gpu_query = nullptr;
        state.gpu_counter = nullptr;
        return;
    }
    if (PdhCollectQueryData(state.gpu_query) == ERROR_SUCCESS)
        state.gpu_query_live = true;
}

static void resmon_ensure_gpu_mem_query(resmon_state_t& state) {
    if (state.gpu_mem_query_init) return;
    state.gpu_mem_query_init = true;
    if (PdhOpenQueryW(nullptr, 0, &state.gpu_mem_query) != ERROR_SUCCESS)
        return;
    if (PdhAddEnglishCounterW(state.gpu_mem_query, L"\\GPU Adapter Memory(*)\\Shared Usage", 0, &state.gpu_mem_shared_counter) != ERROR_SUCCESS ||
        PdhAddEnglishCounterW(state.gpu_mem_query, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &state.gpu_mem_dedicated_counter) != ERROR_SUCCESS ||
        PdhAddEnglishCounterW(state.gpu_mem_query, L"\\GPU Adapter Memory(*)\\Total Committed", 0, &state.gpu_mem_total_counter) != ERROR_SUCCESS) {
        PdhCloseQuery(state.gpu_mem_query);
        state.gpu_mem_query = nullptr;
        state.gpu_mem_shared_counter = nullptr;
        state.gpu_mem_dedicated_counter = nullptr;
        state.gpu_mem_total_counter = nullptr;
        return;
    }
    PdhCollectQueryData(state.gpu_mem_query);
}

static std::unordered_map<uint64_t, ULONGLONG> resmon_sample_gpu_mem_counter(PDH_HCOUNTER counter) {
    std::unordered_map<uint64_t, ULONGLONG> values;
    if (!counter) return values;
    DWORD buf_size = 0;
    DWORD item_count = 0;
    PDH_STATUS status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_LARGE, &buf_size, &item_count, nullptr);
    if (status != PDH_MORE_DATA || buf_size == 0 || item_count == 0)
        return values;

    std::vector<uint8_t> buf(buf_size);
    PDH_FMT_COUNTERVALUE_ITEM_W* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
    if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_LARGE, &buf_size, &item_count, items) != ERROR_SUCCESS)
        return values;

    for (DWORD i = 0; i < item_count; i++) {
        if (!items[i].szName) continue;
        if (items[i].FmtValue.CStatus != ERROR_SUCCESS) continue;
        uint64_t key = 0;
        if (!resmon_parse_gpu_luid(items[i].szName, key)) continue;
        values[key] = (ULONGLONG)items[i].FmtValue.largeValue;
    }
    return values;
}

static void resmon_sample_gpu_memory(resmon_state_t& state,
    std::unordered_map<uint64_t, ULONGLONG>& shared_out,
    std::unordered_map<uint64_t, ULONGLONG>& dedicated_out,
    std::unordered_map<uint64_t, ULONGLONG>& total_out) {
    resmon_ensure_gpu_mem_query(state);
    if (!state.gpu_mem_query) return;
    if (PdhCollectQueryData(state.gpu_mem_query) != ERROR_SUCCESS) return;
    shared_out = resmon_sample_gpu_mem_counter(state.gpu_mem_shared_counter);
    dedicated_out = resmon_sample_gpu_mem_counter(state.gpu_mem_dedicated_counter);
    total_out = resmon_sample_gpu_mem_counter(state.gpu_mem_total_counter);
}

static std::unordered_map<uint64_t, double> resmon_sample_gpu_loads(resmon_state_t& state) {
    ULONGLONG now = GetTickCount64();
    resmon_ensure_gpu_query(state);
    if (!state.gpu_query || !state.gpu_counter) return state.last_gpu_loads;
    if (state.last_gpu_tick != 0 && now - state.last_gpu_tick < 1000)
        return state.last_gpu_loads;
    if (PdhCollectQueryData(state.gpu_query) != ERROR_SUCCESS) return state.last_gpu_loads;

    DWORD buf_size = 0;
    DWORD item_count = 0;
    PDH_STATUS status = PdhGetFormattedCounterArrayW(
        state.gpu_counter,
        PDH_FMT_DOUBLE,
        &buf_size,
        &item_count,
        nullptr);
    if (status != PDH_MORE_DATA || buf_size == 0 || item_count == 0)
        return state.last_gpu_loads;

    std::vector<uint8_t> buf(buf_size);
    PDH_FMT_COUNTERVALUE_ITEM_W* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
    if (PdhGetFormattedCounterArrayW(
            state.gpu_counter,
            PDH_FMT_DOUBLE,
            &buf_size,
            &item_count,
            items) != ERROR_SUCCESS)
        return state.last_gpu_loads;

    std::unordered_map<uint64_t, double> loads;

    for (DWORD i = 0; i < item_count; i++) {
        if (!items[i].szName) continue;
        if (items[i].FmtValue.CStatus != ERROR_SUCCESS) continue;
        uint64_t key = 0;
        if (!resmon_parse_gpu_luid(items[i].szName, key)) continue;
        double value = items[i].FmtValue.doubleValue;
        if (value < 0.0) value = 0.0;
        if (value > 100.0) value = 100.0;
        auto it = loads.find(key);
        if (it == loads.end() || value > it->second)
            loads[key] = value;
    }
    state.last_gpu_loads = loads;
    state.last_gpu_tick = now;
    return loads;
}

static std::string resmon_trim_label(const std::string& s, size_t max_len) {
    if (s.size() <= max_len) return s;
    if (max_len <= 3) return s.substr(0, max_len);
    return s.substr(0, max_len - 3) + "...";
}

static std::string resmon_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return (char)tolower(c);
    });
    return s;
}

static std::string resmon_pad_right(const std::string& s, int width) {
    if ((int)s.size() >= width) return s;
    return s + std::string(width - (int)s.size(), ' ');
}

static std::string resmon_fmt_bytes(double bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit = 0;
    while (bytes >= 1024.0 && unit < 4) {
        bytes /= 1024.0;
        unit++;
    }
    char buf[32];
    if (unit == 0) snprintf(buf, sizeof(buf), "%.0f %s", bytes, units[unit]);
    else snprintf(buf, sizeof(buf), "%.1f %s", bytes, units[unit]);
    return buf;
}

static std::string resmon_fmt_rate(ULONG64 bytes_per_sec) {
    return resmon_fmt_bytes((double)bytes_per_sec) + "/s";
}

static std::string resmon_fmt_link(ULONG64 bits_per_sec) {
    double value = (double)bits_per_sec;
    const char* unit = "bps";
    if (value >= 1000.0 * 1000 * 1000) {
        value /= 1000.0 * 1000 * 1000;
        unit = "Gbps";
    } else if (value >= 1000.0 * 1000) {
        value /= 1000.0 * 1000;
        unit = "Mbps";
    } else if (value >= 1000.0) {
        value /= 1000.0;
        unit = "Kbps";
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f %s", value, unit);
    return buf;
}

static std::string resmon_fmt_uptime(ULONGLONG uptime_ms) {
    ULONGLONG total_sec = uptime_ms / 1000;
    ULONGLONG days = total_sec / 86400;
    ULONGLONG hours = (total_sec / 3600) % 24;
    ULONGLONG mins = (total_sec / 60) % 60;
    ULONGLONG secs = total_sec % 60;
    char buf[64];
    if (days > 0)
        snprintf(buf, sizeof(buf), "%llud %02llu:%02llu:%02llu", days, hours, mins, secs);
    else
        snprintf(buf, sizeof(buf), "%02llu:%02llu:%02llu", hours, mins, secs);
    return buf;
}

static void resmon_history_push(std::vector<double>& history, double value, size_t max_len = 256) {
    if (value < 0.0) value = 0.0;
    if (value > 100.0) value = 100.0;
    history.push_back(value);
    if (history.size() > max_len)
        history.erase(history.begin(), history.begin() + (history.size() - max_len));
}

static const char* resmon_pct_color(double pct) {
    if (pct <= 25.0) return BLUE;
    if (pct >= 75.0) return RED;
    return BRIGHT_YELLOW;
}

static std::string resmon_graph(const std::vector<double>& history, int width, const char* color = nullptr) {
    static const char* glyphs[] = {" ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
    if (width < 1) width = 1;

    std::string out;
    int start = (int)history.size() > width ? (int)history.size() - width : 0;
    int pad = width - ((int)history.size() - start);
    for (int i = 0; i < pad; i++)
        out += std::string(GRAY) + " " + RESET;

    for (int i = start; i < (int)history.size(); i++) {
        double pct = history[i];
        int level = (int)std::round((pct / 100.0) * 8.0);
        if (level < 0) level = 0;
        if (level > 8) level = 8;
        out += color ? color : resmon_pct_color(pct);
        out += glyphs[level];
        out += RESET;
    }
    return out;
}

static std::string resmon_bar(double pct, int width, const char* color = nullptr) {
    static const char* glyphs[] = {" ", "▏", "▎", "▍", "▌", "▋", "▊", "▉", "█"};
    if (width < 2) width = 2;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (!color)
        color = (pct <= 25.0) ? BLUE : (pct >= 75.0) ? RED : BRIGHT_YELLOW;
    int fill = (int)std::round((pct / 100.0) * width * 8.0);
    if (fill < 0) fill = 0;
    if (fill > width * 8) fill = width * 8;
    std::string out;
    for (int i = 0; i < width; i++) {
        int cell_fill = fill - i * 8;
        if (cell_fill < 0) cell_fill = 0;
        if (cell_fill > 8) cell_fill = 8;
        out += color;
        out += glyphs[cell_fill];
        out += RESET;
    }
    return out;
}

static int resmon_metric_graph_width(int cols, int suffix_visible) {
    int width = cols - 2 - 8 - 1 - 2 - suffix_visible;
    if (width < 1) width = 1;
    return width;
}

static std::string resmon_metric_line(const std::string& label, const std::vector<double>& history,
    int cols, const std::string& suffix, const char* suffix_color, const char* graph_color = nullptr) {
    int graph_width = resmon_metric_graph_width(cols, (int)suffix.size());
    return "  " + std::string(GRAY) + resmon_pad_right(label, 8) + " " +
        resmon_graph(history, graph_width, graph_color) + "  " + suffix_color + suffix + RESET;
}

static std::string resmon_power_text(const resmon_snapshot_t& snap);

static std::string resmon_battery_line(const resmon_snapshot_t& snap, const std::vector<double>& history, int cols) {
    if (!snap.has_battery || snap.battery_percent > 100)
        return resmon_metric_line("Battery", history, cols, "n/a", GRAY, GREEN);
    char pct_buf[16];
    snprintf(pct_buf, sizeof(pct_buf), "%u%%", (unsigned)snap.battery_percent);
    return resmon_metric_line("Battery", history, cols, pct_buf, YELLOW, GREEN);
}

static double resmon_network_pct(const resmon_net_t& net) {
    if (net.link_bits_per_sec == 0) return 0.0;
    double bits_per_sec = (double)(net.rx_bytes_per_sec + net.tx_bytes_per_sec) * 8.0;
    double pct = bits_per_sec / (double)net.link_bits_per_sec * 100.0;
    if (pct < 0.0) pct = 0.0;
    if (pct > 100.0) pct = 100.0;
    return pct;
}

static std::string resmon_network_line(const resmon_net_t& net, const std::vector<double>& history, int cols, int idx) {
    char label[16];
    char pct_buf[16];
    snprintf(label, sizeof(label), "Net %02d", idx);
    snprintf(pct_buf, sizeof(pct_buf), "%.1f%%", resmon_network_pct(net));
    return resmon_metric_line(label, history, cols, pct_buf, RESMON_CYAN);
}

static std::string resmon_core_line(int idx, const std::vector<double>& history, double pct, int cols) {
    char label[64];
    char pct_buf[32];
    snprintf(label, sizeof(label), "Core %02d", idx);
    snprintf(pct_buf, sizeof(pct_buf), "%5.1f%%", pct);
    return resmon_metric_line(label, history, cols, pct_buf, BLUE);
}

static std::string resmon_detail_line(const std::string& label, const std::string& text, int cols, const char* color = SILVER) {
    int label_width = 8;
    int text_width = cols - 2 - label_width - 1;
    if (text_width < 1) text_width = 1;
    return "  " + std::string(GRAY) + resmon_pad_right(label, label_width) + " " +
        color + resmon_trim_label(text, (size_t)text_width) + RESET;
}

static void resmon_update_history(resmon_state_t& state, const resmon_snapshot_t& snap) {
    resmon_history_push(state.cpu_history, snap.cpu);
    resmon_history_push(state.memory_history, (double)snap.memory_load);

    if (snap.has_battery && snap.battery_percent <= 100)
        resmon_history_push(state.battery_history, (double)snap.battery_percent);

    if (state.core_history.size() < snap.core_cpu.size())
        state.core_history.resize(snap.core_cpu.size());
    for (size_t i = 0; i < snap.core_cpu.size(); i++)
        resmon_history_push(state.core_history[i], snap.core_cpu[i]);

    for (const auto& gpu : snap.gpus)
        resmon_history_push(state.gpu_history[gpu.luid_key], gpu.usage_pct);

    for (const auto& net : snap.nets)
        resmon_history_push(state.net_history[net.name], resmon_network_pct(net));
}

static std::string resmon_power_text(const resmon_snapshot_t& snap) {
    if (!snap.has_battery) return "No battery";
    std::string ac = "AC ";
    if (snap.ac_line == 1) ac += "online";
    else if (snap.ac_line == 0) ac += "offline";
    else ac += "unknown";

    std::string battery = "Battery ";
    if (snap.battery_percent == 255) battery += "unknown";
    else battery += std::to_string((int)snap.battery_percent) + "%";

    std::string extra;
    if (snap.charging) extra += " charging";
    if (snap.saver_on) extra += " saver";
    return ac + "  " + battery + extra;
}

static std::string resmon_gpu_suffix(const resmon_gpu_t& gpu) {
    char pct_buf[16];
    snprintf(pct_buf, sizeof(pct_buf), "%.1f%%", gpu.usage_pct);
    return std::string(pct_buf) + "  " + resmon_fmt_bytes((double)gpu.used_bytes) + " / " + resmon_fmt_bytes((double)gpu.total_bytes);
}

static void resmon_sample(resmon_state_t& state, resmon_snapshot_t& snap) {
    std::unordered_map<uint64_t, double> gpu_loads = resmon_sample_gpu_loads(state);
    std::unordered_map<uint64_t, ULONGLONG> gpu_shared;
    std::unordered_map<uint64_t, ULONGLONG> gpu_dedicated;
    std::unordered_map<uint64_t, ULONGLONG> gpu_total;
    resmon_sample_gpu_memory(state, gpu_shared, gpu_dedicated, gpu_total);

    FILETIME idle = {}, kernel = {}, user = {};
    if (GetSystemTimes(&idle, &kernel, &user)) {
        if (state.cpu_ready) {
            uint64_t idle_delta = resmon_ft64(idle) - resmon_ft64(state.prev_idle);
            uint64_t kernel_delta = resmon_ft64(kernel) - resmon_ft64(state.prev_kernel);
            uint64_t user_delta = resmon_ft64(user) - resmon_ft64(state.prev_user);
            uint64_t total_delta = kernel_delta + user_delta;
            if (total_delta > 0) snap.cpu = (double)(total_delta - idle_delta) / (double)total_delta * 100.0;
        }
        state.prev_idle = idle;
        state.prev_kernel = kernel;
        state.prev_user = user;
        state.cpu_ready = true;
    }

    SYSTEM_INFO si = {};
    GetSystemInfo(&si);
    snap.logical_cpus = si.dwNumberOfProcessors;
    snap.uptime_ms = GetTickCount64();

    static resmon_nt_query_system_information_t nt_query_system_information =
        (resmon_nt_query_system_information_t)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQuerySystemInformation");
    if (nt_query_system_information && snap.logical_cpus > 0) {
        std::vector<SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION> core_perf(snap.logical_cpus);
        ULONG out_len = 0;
        if (nt_query_system_information(SystemProcessorPerformanceInformation,
                core_perf.data(),
                (ULONG)(core_perf.size() * sizeof(core_perf[0])),
                &out_len) >= 0) {
            snap.core_cpu.resize(core_perf.size(), 0.0);
            if (state.core_ready && state.prev_core_perf.size() == core_perf.size()) {
                for (size_t i = 0; i < core_perf.size(); i++) {
                    uint64_t idle_delta = (uint64_t)(core_perf[i].IdleTime.QuadPart - state.prev_core_perf[i].IdleTime.QuadPart);
                    uint64_t kernel_delta = (uint64_t)(core_perf[i].KernelTime.QuadPart - state.prev_core_perf[i].KernelTime.QuadPart);
                    uint64_t user_delta = (uint64_t)(core_perf[i].UserTime.QuadPart - state.prev_core_perf[i].UserTime.QuadPart);
                    uint64_t total_delta = kernel_delta + user_delta;
                    if (total_delta > 0)
                        snap.core_cpu[i] = (double)(total_delta - idle_delta) / (double)total_delta * 100.0;
                }
            }
            state.prev_core_perf = core_perf;
            state.core_ready = true;
        }
    }

    MEMORYSTATUSEX mem = {};
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem)) {
        snap.memory_load = mem.dwMemoryLoad;
        snap.memory_total = mem.ullTotalPhys;
        snap.memory_avail = mem.ullAvailPhys;
    }

    SYSTEM_POWER_STATUS power = {};
    if (GetSystemPowerStatus(&power)) {
        snap.has_battery = (power.BatteryFlag != 128);
        snap.charging = (power.BatteryFlag & 8) != 0;
        snap.saver_on = power.SystemStatusFlag != 0;
        snap.ac_line = power.ACLineStatus;
        snap.battery_percent = power.BatteryLifePercent;
    }

    IDXGIFactory1* factory = nullptr;
    if (CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory) == S_OK && factory) {
        for (UINT i = 0;; i++) {
            IDXGIAdapter1* adapter = nullptr;
            if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND)
                break;
            if (!adapter) break;

            DXGI_ADAPTER_DESC1 desc = {};
            if (adapter->GetDesc1(&desc) == S_OK && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                IDXGIAdapter3* adapter3 = nullptr;
                DXGI_QUERY_VIDEO_MEMORY_INFO local_mem = {};
                DXGI_QUERY_VIDEO_MEMORY_INFO nonlocal_mem = {};
                bool have_local = false;
                bool have_nonlocal = false;

                if (adapter->QueryInterface(__uuidof(IDXGIAdapter3), (void**)&adapter3) == S_OK && adapter3) {
                    have_local = adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local_mem) == S_OK;
                    have_nonlocal = adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonlocal_mem) == S_OK;
                }

                ULONGLONG used = 0;
                ULONGLONG total = 0;
                resmon_gpu_t gpu;
                gpu.name = resmon_trim_label(to_utf8(desc.Description), 22);
                gpu.luid_key = resmon_luid_key(desc.AdapterLuid);
                auto shared_it = gpu_shared.find(gpu.luid_key);
                auto dedicated_it = gpu_dedicated.find(gpu.luid_key);
                auto total_it = gpu_total.find(gpu.luid_key);
                gpu.shared_bytes = shared_it != gpu_shared.end() ? shared_it->second : 0;
                gpu.dedicated_bytes = dedicated_it != gpu_dedicated.end() ? dedicated_it->second : 0;
                gpu.used_bytes = gpu.shared_bytes + gpu.dedicated_bytes;
                if (total_it != gpu_total.end() && total_it->second > 0)
                    gpu.total_bytes = total_it->second;
                else if (desc.DedicatedVideoMemory > 0 || desc.SharedSystemMemory > 0)
                    gpu.total_bytes = desc.DedicatedVideoMemory + desc.SharedSystemMemory;
                else if (have_local && local_mem.Budget > 0)
                    gpu.total_bytes = local_mem.Budget;
                else if (have_nonlocal && nonlocal_mem.Budget > 0)
                    gpu.total_bytes = nonlocal_mem.Budget;
                else
                    gpu.total_bytes = 0;
                auto load_it = gpu_loads.find(gpu.luid_key);
                gpu.usage_pct = load_it != gpu_loads.end() ? load_it->second : 0.0;
                snap.gpus.push_back(gpu);

                if (adapter3) adapter3->Release();
            }
            adapter->Release();
        }
        factory->Release();
    }

    ULONG table_size = 0;
    ULONG adapter_size = 0;
    ULONGLONG now = GetTickCount64();
    double dt = state.prev_net_tick ? (double)(now - state.prev_net_tick) / 1000.0 : 0.0;
    std::unordered_map<ULONG, std::string> adapter_names;
    GetAdaptersInfo(nullptr, &adapter_size);
    if (adapter_size > 0) {
        std::vector<uint8_t> adapter_buf(adapter_size);
        IP_ADAPTER_INFO* adapters = reinterpret_cast<IP_ADAPTER_INFO*>(adapter_buf.data());
        if (GetAdaptersInfo(adapters, &adapter_size) == NO_ERROR) {
            for (IP_ADAPTER_INFO* a = adapters; a; a = a->Next) {
            std::string ip = a->IpAddressList.IpAddress.String;
            if (ip == "0.0.0.0" || ip.substr(0, 4) == "127.") continue;
            std::string desc = a->Description;
            std::string desc_lower = resmon_lower(desc);
            if (desc_lower.find("hyper-v") != std::string::npos) continue;
            adapter_names[a->Index] = desc;
        }
    }
    }

    GetIfTable(nullptr, &table_size, FALSE);
    std::vector<uint8_t> table_buf(table_size);
    MIB_IFTABLE* table = reinterpret_cast<MIB_IFTABLE*>(table_buf.data());
    if (GetIfTable(table, &table_size, FALSE) == NO_ERROR) {
        std::unordered_map<ULONG, std::pair<ULONG64, ULONG64>> current_bytes;
        std::unordered_map<std::string, resmon_net_t> nets_by_name;
        for (DWORD i = 0; i < table->dwNumEntries; i++) {
            const MIB_IFROW& row = table->table[i];
            if (row.dwOperStatus != IF_OPER_STATUS_OPERATIONAL) continue;
            if (row.dwType == IF_TYPE_SOFTWARE_LOOPBACK || row.dwType == IF_TYPE_TUNNEL) continue;
            auto visible = adapter_names.find(row.dwIndex);
            if (visible == adapter_names.end()) continue;

            ULONG64 rx = 0;
            ULONG64 tx = 0;
            auto prev = state.prev_net_bytes.find(row.dwIndex);
            if (dt > 0.0 && prev != state.prev_net_bytes.end()) {
                if ((ULONG64)row.dwInOctets >= prev->second.first)
                    rx = (ULONG64)(((ULONG64)row.dwInOctets - prev->second.first) / dt);
                if ((ULONG64)row.dwOutOctets >= prev->second.second)
                    tx = (ULONG64)(((ULONG64)row.dwOutOctets - prev->second.second) / dt);
            }

            current_bytes[row.dwIndex] = {(ULONG64)row.dwInOctets, (ULONG64)row.dwOutOctets};
            resmon_net_t& net = nets_by_name[visible->second];
            net.name = visible->second;
            net.rx_bytes_per_sec += rx;
            net.tx_bytes_per_sec += tx;
            if (row.dwSpeed > net.link_bits_per_sec)
                net.link_bits_per_sec = row.dwSpeed;
        }
        state.prev_net_bytes = current_bytes;
        state.prev_net_tick = now;
        for (auto& it : nets_by_name)
            snap.nets.push_back(it.second);
    }

    std::sort(snap.nets.begin(), snap.nets.end(), [](const resmon_net_t& a, const resmon_net_t& b) {
        ULONG64 a_total = a.rx_bytes_per_sec + a.tx_bytes_per_sec;
        ULONG64 b_total = b.rx_bytes_per_sec + b.tx_bytes_per_sec;
        if (a_total != b_total) return a_total > b_total;
        return a.name < b.name;
    });

    std::sort(snap.gpus.begin(), snap.gpus.end(), [](const resmon_gpu_t& a, const resmon_gpu_t& b) {
        return a.name < b.name;
    });
}

static void resmon_draw_line(std::string& frame, const std::string& line) {
    frame += line;
    frame += "\x1b[K\r\n";
}

static void resmon_cmd() {
    resmon_state_t state;
    resmon_snapshot_t snap;
    resmon_sample(state, snap);
    resmon_update_history(state, snap);

    out("\x1b[?25l\x1b[2J");
    int prev_cols = 0;
    int prev_rows = 0;
    ULONGLONG last_sample = 0;

    while (true) {
        DWORD events = 0;
        GetNumberOfConsoleInputEvents(in_h, &events);
        while (events-- > 0) {
            INPUT_RECORD ir = {};
            DWORD rd = 0;
            ReadConsoleInputW(in_h, &ir, 1, &rd);
            if (ir.EventType != KEY_EVENT || !ir.Event.KeyEvent.bKeyDown) continue;
            WORD vk = ir.Event.KeyEvent.wVirtualKeyCode;
            wchar_t ch = ir.Event.KeyEvent.uChar.UnicodeChar;
            if (vk == VK_ESCAPE || ch == L'q' || ch == L'Q')
                goto done;
        }

        ULONGLONG now = GetTickCount64();
        int cols = term_width();
        int rows = term_height();
        if (now - last_sample >= 250 || cols != prev_cols || rows != prev_rows) {
            snap = {};
            resmon_sample(state, snap);
            resmon_update_history(state, snap);
            last_sample = now;

            std::string frame;
            if (cols != prev_cols || rows != prev_rows) frame += "\x1b[2J";
            frame += "\x1b[H";

            std::string title = std::string(BLUE) + "res" + BRIGHT_YELLOW "mon" RESET;
            int used_rows = 0;
            resmon_draw_line(frame, "  " + title + "  " + GRAY + cur_time() + RESET + "  " + GRAY + "Esc/q exit" + RESET);
            used_rows++;

            std::string sep = GRAY;
            for (int i = 0; i < cols; i++) sep += "\xe2\x94\x80";
            sep += RESET;
            resmon_draw_line(frame, sep);
            used_rows++;

            std::vector<std::string> overview_head_lines;
            std::vector<std::string> overview_tail_lines;
            std::vector<std::string> core_lines;
            std::vector<std::string> detail_lines;

            char cpu_pct[16];
            snprintf(cpu_pct, sizeof(cpu_pct), "%.1f%%", snap.cpu);
            overview_head_lines.push_back(resmon_metric_line("CPU", state.cpu_history, cols, cpu_pct, GREEN));

            char mem_pct[16];
            snprintf(mem_pct, sizeof(mem_pct), "%lu%%", (unsigned long)snap.memory_load);
            for (size_t i = 0; i < snap.gpus.size(); i++) {
                std::string label = "GPU " + std::to_string((int)i);
                char gpu_pct[16];
                snprintf(gpu_pct, sizeof(gpu_pct), "%.1f%%", snap.gpus[i].usage_pct);
                overview_head_lines.push_back(resmon_metric_line(label, state.gpu_history[snap.gpus[i].luid_key], cols, gpu_pct, YELLOW, YELLOW));
                detail_lines.push_back(resmon_detail_line(label, snap.gpus[i].name + "  " + resmon_gpu_suffix(snap.gpus[i]), cols));
            }
            detail_lines.push_back(resmon_detail_line("Battery", resmon_power_text(snap), cols, YELLOW));

            if (snap.nets.empty()) {
                detail_lines.push_back(resmon_detail_line("Network", "No active adapters", cols, GRAY));
            } else {
                for (int i = 0; i < (int)snap.nets.size(); i++) {
                    overview_tail_lines.push_back(resmon_network_line(snap.nets[i], state.net_history[snap.nets[i].name], cols, i));
                    std::string detail = snap.nets[i].name + "  RX " + resmon_fmt_rate(snap.nets[i].rx_bytes_per_sec) +
                        "  TX " + resmon_fmt_rate(snap.nets[i].tx_bytes_per_sec);
                    if (snap.nets[i].link_bits_per_sec > 0)
                        detail += "  Link " + resmon_fmt_link(snap.nets[i].link_bits_per_sec);
                    detail_lines.push_back(resmon_detail_line("Net " + std::to_string(i), detail, cols, RESMON_CYAN));
                }
            }

            overview_tail_lines.push_back(resmon_metric_line("Memory", state.memory_history, cols, mem_pct, MAGENTA));
            overview_tail_lines.push_back(resmon_battery_line(snap, state.battery_history, cols));
            detail_lines.push_back(resmon_detail_line("Uptime", resmon_fmt_uptime(snap.uptime_ms), cols, GREEN));

            for (size_t i = 0; i < snap.core_cpu.size(); i++)
                core_lines.push_back(resmon_core_line((int)i, state.core_history[i], snap.core_cpu[i], cols));

            int detail_rows = std::min((int)detail_lines.size(), std::max(2, rows / 3));
            int overview_rows = rows - used_rows - 1 - detail_rows;
            while (overview_rows < 1 && detail_rows > 0) {
                detail_rows--;
                overview_rows = rows - used_rows - 1 - detail_rows;
            }
            if (overview_rows < 1) overview_rows = 1;

            int tail_rows = std::min((int)overview_tail_lines.size(), overview_rows);
            int head_rows = overview_rows - tail_rows;

            int shown_overview = 0;
            for (const auto& line : overview_head_lines) {
                if (shown_overview >= head_rows) break;
                resmon_draw_line(frame, line);
                shown_overview++;
                used_rows++;
            }

            int remaining_overview = head_rows - shown_overview;
            int shown_cores = std::min((int)core_lines.size(), remaining_overview);
            for (int i = 0; i < shown_cores; i++) {
                resmon_draw_line(frame, core_lines[i]);
                used_rows++;
            }
            shown_overview += shown_cores;
            if (shown_cores < (int)core_lines.size() && shown_overview < head_rows) {
                int hidden = (int)core_lines.size() - shown_cores;
                resmon_draw_line(frame, "  " + std::string(GRAY) + "... " + std::to_string(hidden) + " more cores" + RESET);
                used_rows++;
                shown_overview++;
            }

            while (shown_overview < head_rows) {
                resmon_draw_line(frame, "");
                used_rows++;
                shown_overview++;
            }

            for (int i = 0; i < tail_rows; i++) {
                resmon_draw_line(frame, overview_tail_lines[i]);
                used_rows++;
            }

            resmon_draw_line(frame, sep);
            used_rows++;

            for (int i = 0; i < detail_rows; i++) {
                resmon_draw_line(frame, detail_lines[i]);
                used_rows++;
            }

            while (used_rows < rows)
                resmon_draw_line(frame, "");

            out(frame);
            prev_cols = cols;
            prev_rows = rows;
        }

        Sleep(50);
    }

done:
    if (state.gpu_query) {
        PdhCloseQuery(state.gpu_query);
        state.gpu_query = nullptr;
    }
    if (state.gpu_mem_query) {
        PdhCloseQuery(state.gpu_mem_query);
        state.gpu_mem_query = nullptr;
    }
    out("\x1b[0m\x1b[2J\x1b[H\x1b[?25h");
}
