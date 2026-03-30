// MODULE: mp3
// Purpose : lightweight MP3 playback using minimp3 for decode and waveOut for output
// Exports : mp3_cmd() mp3_shutdown()
// Depends : common.h

#include <mmsystem.h>

#define MINIMP3_IMPLEMENTATION
#include "../minimp3.h"

enum mp3_state_kind {
    mp3_stopped = 0,
    mp3_loading,
    mp3_playing,
    mp3_paused,
};

struct mp3_player_t {
    std::mutex              lock;
    std::thread             worker;
    std::atomic<bool>       stop_flag = false;
    std::string             path;
    std::string             last_error;
    std::vector<char>       pcm_bytes;
    HWAVEOUT                device = NULL;
    WAVEHDR                 header = {};
    bool                    header_prepared = false;
    int                     hz = 0;
    int                     channels = 0;
    int                     volume = 100;
    int                     track_peak = 0;
    double                  track_rms = 0.0;
    double                  total_sec = 0.0;
    mp3_state_kind          state = mp3_stopped;
    ULONGLONG               play_started_tick = 0;
    ULONGLONG               pause_started_tick = 0;
    ULONGLONG               paused_ms = 0;
    // folder playback state
    std::vector<std::string>    folder_tracks;   // all .mp3 found recursively
    std::vector<std::string>    folder_history;  // play history, oldest→newest; last = current
    std::unordered_set<int>     folder_played;   // indices into folder_tracks already started
};

static mp3_player_t g_mp3;

static int mp3_ui();

static std::string mp3_trim(std::string s) {
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    while (!s.empty() && s.back()  == ' ') s.pop_back();
    return s;
}

static std::string mp3_state_text(mp3_state_kind state) {
    if (state == mp3_loading) return "loading";
    if (state == mp3_playing) return "playing";
    if (state == mp3_paused)  return "paused";
    return "stopped";
}

static std::string mp3_name_only(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

static std::string mp3_display_path(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}

static std::string mp3_mm_text(MMRESULT code) {
    char buf[256] = {};
    if (waveOutGetErrorTextA(code, buf, sizeof(buf)) == MMSYSERR_NOERROR)
        return buf;
    return "audio device error";
}

static void mp3_scan_folder(const std::string& dir, std::vector<std::string>& out_tracks) {
    std::wstring wdir = to_wide(dir) + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wdir.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        std::string full = dir + "\\" + to_utf8(name);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            mp3_scan_folder(full, out_tracks);
        } else {
            if (name.size() > 4) {
                std::wstring ext = name.substr(name.size() - 4);
                std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
                if (ext == L".mp3") out_tracks.push_back(full);
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void mp3_folder_clear() {
    g_mp3.folder_tracks.clear();
    g_mp3.folder_history.clear();
    g_mp3.folder_played.clear();
}

static int mp3_elapsed_ms_locked() {
    if (g_mp3.state == mp3_loading || g_mp3.play_started_tick == 0) return 0;

    ULONGLONG now = GetTickCount64();
    ULONGLONG elapsed = 0;
    if (g_mp3.state == mp3_paused && g_mp3.pause_started_tick != 0)
        elapsed = g_mp3.pause_started_tick - g_mp3.play_started_tick - g_mp3.paused_ms;
    else
        elapsed = now - g_mp3.play_started_tick - g_mp3.paused_ms;

    ULONGLONG total_ms = (ULONGLONG)(g_mp3.total_sec * 1000.0);
    if (elapsed > total_ms) elapsed = total_ms;
    return (int)elapsed;
}

static int mp3_elapsed_ms() {
    std::lock_guard<std::mutex> guard(g_mp3.lock);
    return mp3_elapsed_ms_locked();
}

static double mp3_elapsed_sec() {
    return mp3_elapsed_ms() / 1000.0;
}

struct mp3_snapshot_t {
    std::string path;
    std::string last_error;
    std::string bars;
    int elapsed_ms = 0;
    int volume = 100;
    double total_sec = 0.0;
    mp3_state_kind state = mp3_stopped;
    int folder_pos = 0;   // 1-based index of current track in folder (0 = not in folder mode)
    int folder_total = 0;
};

static std::string mp3_make_bars_locked(int elapsed_ms, int bar_count);

static mp3_snapshot_t mp3_snapshot() {
    std::lock_guard<std::mutex> guard(g_mp3.lock);
    mp3_snapshot_t snap;
    snap.path = g_mp3.path;
    snap.last_error = g_mp3.last_error;
    snap.elapsed_ms = mp3_elapsed_ms_locked();
    snap.bars = mp3_make_bars_locked(snap.elapsed_ms, 12);
    snap.volume = g_mp3.volume;
    snap.total_sec = g_mp3.total_sec;
    snap.state = g_mp3.state;
    snap.folder_total = (int)g_mp3.folder_tracks.size();
    snap.folder_pos   = snap.folder_total > 0 ? (int)g_mp3.folder_history.size() : 0;
    return snap;
}

static bool mp3_decode_file(const std::string& path, std::vector<int16_t>& pcm, int& hz, int& channels, double& total_sec, std::string& error_text) {
    std::ifstream f(to_wide(path).c_str(), std::ios::binary);
    if (!f) {
        error_text = "mp3: cannot open file: " + path;
        return false;
    }

    std::vector<unsigned char> data((std::istreambuf_iterator<char>(f)), {});
    if (data.empty()) {
        error_text = "mp3: file is empty: " + path;
        return false;
    }

    mp3dec_t dec = {};
    mp3dec_init(&dec);
    std::vector<mp3d_sample_t> frame(MINIMP3_MAX_SAMPLES_PER_FRAME);

    bool got_stream = false;
    for (int pos = 0; pos < (int)data.size();) {
        if (g_mp3.stop_flag) return false;

        mp3dec_frame_info_t info = {};
        int samples = mp3dec_decode_frame(&dec, data.data() + pos, (int)data.size() - pos, frame.data(), &info);
        if (info.frame_bytes <= 0) {
            pos++;
            continue;
        }
        pos += info.frame_bytes;
        if (samples <= 0) continue;

        if (!got_stream) {
            hz = info.hz;
            channels = info.channels;
            got_stream = true;
            if (hz <= 0 || (channels != 1 && channels != 2)) {
                error_text = "mp3: unsupported stream format";
                return false;
            }
        } else if (info.hz != hz || info.channels != channels) {
            error_text = "mp3: variable stream format is not supported yet";
            return false;
        }

        pcm.insert(pcm.end(), frame.begin(), frame.begin() + samples * channels);
    }

    if (!got_stream || pcm.empty()) {
        error_text = "mp3: no audio frames decoded";
        return false;
    }

    total_sec = (double)pcm.size() / (double)(channels * hz);
    return true;
}

static void mp3_worker_main(std::string path) {
    std::vector<int16_t> pcm;
    int hz = 0;
    int channels = 0;
    double total_sec = 0.0;
    std::string error_text;

    {
        std::lock_guard<std::mutex> guard(g_mp3.lock);
        g_mp3.path = path;
        g_mp3.last_error.clear();
        g_mp3.state = mp3_loading;
    }

    bool ok = mp3_decode_file(path, pcm, hz, channels, total_sec, error_text);
    int track_peak = 0;
    double track_rms = 0.0;
    if (ok) {
        long long sum_sq = 0;
        for (int16_t s : pcm) {
            int v = s < 0 ? -s : s;
            if (v > track_peak) track_peak = v;
            sum_sq += (long long)s * s;
        }
        if (!pcm.empty())
            track_rms = sqrt((double)sum_sq / (double)pcm.size());
    }
    if (!ok) {
        std::lock_guard<std::mutex> guard(g_mp3.lock);
        if (!g_mp3.stop_flag && !error_text.empty()) g_mp3.last_error = error_text;
        g_mp3.state = mp3_stopped;
        g_mp3.path.clear();
        return;
    }

    WAVEFORMATEX fmt = {};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = (WORD)channels;
    fmt.nSamplesPerSec = hz;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = (WORD)(fmt.nChannels * (fmt.wBitsPerSample / 8));
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;

    HWAVEOUT device = NULL;
    MMRESULT mm = waveOutOpen(&device, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL);
    if (mm != MMSYSERR_NOERROR) {
        std::lock_guard<std::mutex> guard(g_mp3.lock);
        g_mp3.last_error = "mp3: " + mp3_mm_text(mm);
        g_mp3.state = mp3_stopped;
        g_mp3.path.clear();
        return;
    }

    {
        std::lock_guard<std::mutex> guard(g_mp3.lock);
        if (g_mp3.stop_flag) {
            waveOutClose(device);
            g_mp3.state = mp3_stopped;
            g_mp3.path.clear();
            return;
        }
        g_mp3.device = device;
        g_mp3.hz = hz;
        g_mp3.channels = channels;
        g_mp3.total_sec = total_sec;
        g_mp3.track_peak = track_peak;
        g_mp3.track_rms  = track_rms;
        g_mp3.play_started_tick = 0;
        g_mp3.pause_started_tick = 0;
        g_mp3.paused_ms = 0;
        g_mp3.pcm_bytes.resize(pcm.size() * sizeof(int16_t));
        memcpy(g_mp3.pcm_bytes.data(), pcm.data(), g_mp3.pcm_bytes.size());
        g_mp3.header = {};
        g_mp3.header.lpData = g_mp3.pcm_bytes.empty() ? NULL : &g_mp3.pcm_bytes[0];
        g_mp3.header.dwBufferLength = (DWORD)g_mp3.pcm_bytes.size();
    }

    DWORD left = (DWORD)((g_mp3.volume * 0xFFFFu) / 100u);
    DWORD volume = (left << 16) | left;
    waveOutSetVolume(device, volume);

    {
        std::lock_guard<std::mutex> guard(g_mp3.lock);
        mm = waveOutPrepareHeader(device, &g_mp3.header, sizeof(g_mp3.header));
        if (mm == MMSYSERR_NOERROR) g_mp3.header_prepared = true;
    }
    if (mm != MMSYSERR_NOERROR) {
        std::lock_guard<std::mutex> guard(g_mp3.lock);
        g_mp3.last_error = "mp3: " + mp3_mm_text(mm);
        g_mp3.state = mp3_stopped;
        g_mp3.path.clear();
        waveOutClose(device);
        g_mp3.device = NULL;
        g_mp3.pcm_bytes.clear();
        return;
    }

    {
        std::lock_guard<std::mutex> guard(g_mp3.lock);
        mm = waveOutWrite(device, &g_mp3.header, sizeof(g_mp3.header));
        if (mm == MMSYSERR_NOERROR) {
            g_mp3.state = mp3_playing;
            g_mp3.play_started_tick = GetTickCount64();
            g_mp3.pause_started_tick = 0;
            g_mp3.paused_ms = 0;
        }
    }
    if (mm != MMSYSERR_NOERROR) {
        std::lock_guard<std::mutex> guard(g_mp3.lock);
        g_mp3.last_error = "mp3: " + mp3_mm_text(mm);
        g_mp3.state = mp3_stopped;
        waveOutUnprepareHeader(device, &g_mp3.header, sizeof(g_mp3.header));
        g_mp3.header_prepared = false;
        waveOutClose(device);
        g_mp3.device = NULL;
        g_mp3.path.clear();
        g_mp3.pcm_bytes.clear();
        return;
    }

    while (!g_mp3.stop_flag) {
        bool done = false;
        {
            std::lock_guard<std::mutex> guard(g_mp3.lock);
            done = (g_mp3.header.dwFlags & WHDR_DONE) != 0;
        }
        if (done) break;
        Sleep(50);
    }

    bool stopped = g_mp3.stop_flag;
    {
        std::lock_guard<std::mutex> guard(g_mp3.lock);
        if (g_mp3.device) waveOutReset(g_mp3.device);
        if (g_mp3.device && g_mp3.header_prepared)
            waveOutUnprepareHeader(g_mp3.device, &g_mp3.header, sizeof(g_mp3.header));
        if (g_mp3.device) waveOutClose(g_mp3.device);
        g_mp3.device = NULL;
        g_mp3.header = {};
        g_mp3.header_prepared = false;
        g_mp3.pcm_bytes.clear();
        g_mp3.hz = 0;
        g_mp3.channels = 0;
        g_mp3.total_sec = 0.0;
        g_mp3.track_peak = 0;
        g_mp3.track_rms  = 0.0;
        g_mp3.play_started_tick = 0;
        g_mp3.pause_started_tick = 0;
        g_mp3.paused_ms = 0;
        g_mp3.state = mp3_stopped;
        g_mp3.path.clear();
        if (!stopped) g_mp3.last_error.clear();
    }
}

static void mp3_stop_playback() {
    g_mp3.stop_flag = true;

    HWAVEOUT device = NULL;
    {
        std::lock_guard<std::mutex> guard(g_mp3.lock);
        device = g_mp3.device;
    }
    if (device) waveOutReset(device);

    if (g_mp3.worker.joinable()) g_mp3.worker.join();

    std::lock_guard<std::mutex> guard(g_mp3.lock);
    g_mp3.stop_flag = false;
    g_mp3.state = mp3_stopped;
    g_mp3.path.clear();
    g_mp3.total_sec = 0.0;
    g_mp3.hz = 0;
    g_mp3.channels = 0;
    g_mp3.track_peak = 0;
    g_mp3.track_rms  = 0.0;
    g_mp3.play_started_tick = 0;
    g_mp3.pause_started_tick = 0;
    g_mp3.paused_ms = 0;
    g_mp3.device = NULL;
    g_mp3.header = {};
    g_mp3.header_prepared = false;
    g_mp3.pcm_bytes.clear();
}

// Start playing a single file. Caller must already have stopped playback.
// Does NOT clear folder state.
static void mp3_start_track(const std::string& path) {
    {
        std::lock_guard<std::mutex> guard(g_mp3.lock);
        g_mp3.last_error.clear();
        g_mp3.path = path;
        g_mp3.state = mp3_loading;
    }
    g_mp3.stop_flag = false;
    g_mp3.worker = std::thread(mp3_worker_main, path);
}

// Pick a random unplayed track; returns index or -1 if all played.
static int mp3_folder_pick_next() {
    int total = (int)g_mp3.folder_tracks.size();
    if (total == 0) return -1;
    if ((int)g_mp3.folder_played.size() >= total) return -1;
    std::vector<int> pool;
    for (int i = 0; i < total; i++)
        if (!g_mp3.folder_played.count(i)) pool.push_back(i);
    if (pool.empty()) return -1;
    return pool[rand() % pool.size()];
}

static int mp3_play_file(const std::string& arg) {
    std::string path = normalize_path(mp3_trim(arg));
    if (path.empty()) {
        err("mp3: missing file path\r\n");
        return 1;
    }

    DWORD attr = GetFileAttributesW(to_wide(path).c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        err("mp3: file not found\r\n");
        return 1;
    }

    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        // Folder mode: scan, shuffle, play first random track
        std::vector<std::string> tracks;
        mp3_scan_folder(path, tracks);
        if (tracks.empty()) {
            err("mp3: no .mp3 files found in folder\r\n");
            return 1;
        }
        mp3_stop_playback();
        {
            std::lock_guard<std::mutex> guard(g_mp3.lock);
            mp3_folder_clear();
            g_mp3.folder_tracks = tracks;
        }
        int idx = mp3_folder_pick_next();
        {
            std::lock_guard<std::mutex> guard(g_mp3.lock);
            g_mp3.folder_played.insert(idx);
            g_mp3.folder_history.push_back(g_mp3.folder_tracks[idx]);
        }
        mp3_start_track(g_mp3.folder_tracks[idx]);
        return mp3_ui();
    }

    // Single file mode — stop and clear folder state
    mp3_stop_playback();
    {
        std::lock_guard<std::mutex> guard(g_mp3.lock);
        mp3_folder_clear();
    }
    mp3_start_track(path);
    return mp3_ui();
}

static int mp3_show_status() {
    mp3_snapshot_t snap = mp3_snapshot();

    if (!snap.last_error.empty()) out(snap.last_error + "\r\n");

    char time_buf[64] = {};
    double elapsed = mp3_elapsed_sec();
    int elapsed_s = (int)(elapsed + 0.5);
    int total_s = (int)(snap.total_sec + 0.5);
    snprintf(time_buf, sizeof(time_buf), "%02d:%02d / %02d:%02d",
        elapsed_s / 60, elapsed_s % 60, total_s / 60, total_s % 60);

    out("State : " + mp3_state_text(snap.state) + "\r\n");
    out("Volume: " + std::to_string(snap.volume) + "%\r\n");
    if (!snap.path.empty()) out("File  : " + mp3_display_path(snap.path) + "\r\n");
    if (snap.state != mp3_stopped) out(std::string("Time  : ") + time_buf + "\r\n");
    return snap.state == mp3_stopped && snap.last_error.empty() ? 1 : 0;
}

// mp3_make_bars_locked — waveform bar visualizer
//
// Called inside g_mp3.lock every UI refresh (~60ms). Returns a string like
// [▁▃▆█▇▄▂▁▃▅▇█] that is printed in-place on the status line.
//
// How it works — scrolling history:
//   Every step_ms of playback a new bar is computed and appended on the right;
//   the oldest bar drops off the left. This means the display always scrolls
//   forward in time, so it never "freezes" even during a sustained note.
//
// How each bar's height is measured:
//   Instead of averaging over the full step window (which made every bar look
//   the same for bass), we sample a narrow ~2ms window at the step's midpoint.
//   At 80 Hz bass the waveform cycle is ~12ms, so adjacent 15ms steps land on
//   different phases of the cycle — one bar catches the crest (loud), the next
//   catches near the zero crossing (quiet), giving real height variation.
//
// Amplitude → height mapping (smoothstep S-curve):
//   norm  = peak_in_window / track_peak          (0..1 relative to loudest sample)
//   level = norm² × (3 − 2×norm) × 7            (smoothstep, 0..7)
//   Smoothstep has an S-shape: it pushes low values toward ▁ and high values
//   toward █, making quiet sections visually dark and loud sections visually full.
//
// Reset logic:
//   play_started_tick changes when a new track starts or the user seeks, so the
//   history is cleared automatically on those events.
//
// --- Tuning knobs ---
//
//   Time resolution (zoom in / zoom out):
//     Change `step_ms` below AND the WaitForSingleObject timeout in mp3_ui_key_pressed()
//     to the same value — they must match for smooth 1-bar-per-frame scrolling.
//     Smaller  → zoomed in,  more detail, shorter time window (15ms → ~180ms total)
//     Larger   → zoomed out, smoother,    longer  time window (60ms → ~720ms total)
//     Good range: 10ms (very detailed, ~120fps) … 60ms (wide view, ~16fps)
//
//   Contrast (exaggeration of highs and lows):
//     The mapping is:  level = norm² × (3 − 2×norm) × 7   (smoothstep S-curve)
//     To increase contrast, steepen the curve — replace with a sharper S:
//       level = norm³ × (6×norm² − 15×norm + 10) × 7      (smootherstep, steeper)
//     To decrease contrast (flatter, more uniform bars), use a power curve:
//       level = sqrt(norm) × 7                             (gentler slope)
//     Or scale the output to exaggerate: level = norm² × (3 − 2×norm) × 9 − 1
//     clamped to [0, 7] — this clips the very top and bottom for more drama.
//
//   Reference level (what counts as "loud"):
//     `ref_peak = track_peak` — the absolute loudest sample in the file.
//     Lower it (e.g. track_peak * 0.6) to make average passages look louder.
//     Use track_rms * N (N ≈ 3–5) to normalize relative to typical loudness.
static std::string mp3_make_bars_locked(int elapsed_ms, int bar_count) {
    static const char* glyphs[] = {"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};

    // Scrolling history: each slot = peak level of a step_ms window
    static double history[12] = {};
    static int    history_size = 0;
    static int    last_step    = -1;
    static ULONGLONG last_play_tick = 0;

    // Reset on track change or seek (play_started_tick changes)
    if (g_mp3.play_started_tick != last_play_tick) {
        memset(history, 0, sizeof(history));
        history_size = 0;
        last_step    = -1;
        last_play_tick = g_mp3.play_started_tick;
    }

    if (g_mp3.pcm_bytes.empty() || g_mp3.hz <= 0 || (g_mp3.channels != 1 && g_mp3.channels != 2)) {
        return std::string(GRAY) + "[--------]" + RESET;
    }

    const int16_t* samples = (const int16_t*)g_mp3.pcm_bytes.data();
    int sample_count = (int)(g_mp3.pcm_bytes.size() / sizeof(int16_t));
    int frame_count = sample_count / g_mp3.channels;
    int ref_peak = g_mp3.track_peak > 0 ? g_mp3.track_peak : 32767;

    // Each step: sample a narrow ~2ms window at the step midpoint.
    const int step_ms  = 8;
    const int half_win = g_mp3.hz / 500;   // ~2ms total window, 1ms each side
    int cur_step = elapsed_ms / step_ms;
    while (last_step < cur_step) {
        last_step++;
        long long mid = ((long long)last_step * step_ms + step_ms / 2) * g_mp3.hz / 1000;
        int start_frame = (int)(mid - half_win);
        int end_frame   = (int)(mid + half_win);
        if (start_frame < 0) start_frame = 0;
        if (end_frame > frame_count) end_frame = frame_count;

        int peak = 0;
        for (int fr = start_frame; fr < end_frame; fr++) {
            long long mixed = 0;
            for (int ch = 0; ch < g_mp3.channels; ch++)
                mixed += samples[fr * g_mp3.channels + ch];
            mixed /= g_mp3.channels;
            int v = (int)(mixed < 0 ? -mixed : mixed);
            if (v > peak) peak = v;
        }
        double norm = (double)peak / ref_peak;
        if (norm > 1.0) norm = 1.0;
        double level = norm * norm * (3.0 - 2.0 * norm) * 7.0;

        // Shift left, append new value
        if (history_size < bar_count) {
            history[history_size++] = level;
        } else {
            memmove(history, history + 1, (bar_count - 1) * sizeof(double));
            history[bar_count - 1] = level;
        }
    }

    // OLD renderer — restored
    std::string bars = "[";
    for (int i = 0; i < bar_count; i++) {
        int level = i < history_size ? (int)(history[i] + 0.5) : 0;
        level = std::max(0, std::min(level, 7));
        const char* color = (level <= 1) ? BLUE : (level >= 6) ? RED : BRIGHT_YELLOW;
        bars += color;
        bars += glyphs[level];
        bars += RESET;
    }
    bars += "]";
    return bars;

    // NEW renderer (half-block, 16-level, color zones) — postponed, needs work
    // auto zone_color = [](int sub) -> const char* {
    //     if (sub <= 4)  return BLUE;
    //     if (sub <= 9)  return YELLOW;
    //     return RED;
    // };
    // std::string bars2 = "[";
    // for (int i = 0; i < bar_count; i++) {
    //     int level = i < history_size ? (int)(history[i] + 0.5) : 0;
    //     level = std::max(0, std::min(level, 15));
    //     if (level == 0) {
    //         bars2 += ' ';
    //     } else if (level == 1) {
    //         bars2 += zone_color(0);
    //         bars2 += "▄";
    //         bars2 += RESET;
    //     } else {
    //         const char* bot = zone_color(level - 1);
    //         const char* top = zone_color(level);
    //         if (bot == top) {
    //             bars2 += bot;
    //             bars2 += "█";
    //             bars2 += RESET;
    //         } else {
    //             bars2 += "\x1b[0m";
    //             bars2 += bot;
    //             if (top == BLUE)        bars2 += "\x1b[48;5;75m";
    //             else if (top == YELLOW) bars2 += "\x1b[48;5;229m";
    //             else                    bars2 += "\x1b[48;5;203m";
    //             bars2 += "▄";
    //             bars2 += RESET;
    //         }
    //     }
    // }
    // bars2 += "]";
    // return bars2;
}

static void mp3_apply_volume_locked(int volume) {
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    g_mp3.volume = volume;
    if (g_mp3.device) {
        DWORD left = (DWORD)((volume * 0xFFFFu) / 100u);
        DWORD wave_volume = (left << 16) | left;
        waveOutSetVolume(g_mp3.device, wave_volume);
    }
}

static void mp3_seek_locked(int target_ms) {
    if (!g_mp3.device) return;
    if (g_mp3.state != mp3_playing && g_mp3.state != mp3_paused) return;
    if (g_mp3.pcm_bytes.empty() || g_mp3.hz <= 0 || g_mp3.channels <= 0) return;

    int total_ms = (int)(g_mp3.total_sec * 1000.0);
    if (target_ms < 0) target_ms = 0;
    if (target_ms > total_ms) target_ms = total_ms;

    int bytes_per_frame = g_mp3.channels * (int)sizeof(int16_t);
    long long byte_offset = ((long long)target_ms * g_mp3.hz / 1000) * bytes_per_frame;
    long long max_offset = (long long)g_mp3.pcm_bytes.size() - bytes_per_frame;
    if (byte_offset > max_offset) byte_offset = max_offset;
    if (byte_offset < 0) byte_offset = 0;

    bool was_paused = (g_mp3.state == mp3_paused);
    waveOutReset(g_mp3.device);
    if (g_mp3.header_prepared) {
        waveOutUnprepareHeader(g_mp3.device, &g_mp3.header, sizeof(g_mp3.header));
        g_mp3.header_prepared = false;
    }

    g_mp3.header = {};
    g_mp3.header.lpData = &g_mp3.pcm_bytes[(size_t)byte_offset];
    g_mp3.header.dwBufferLength = (DWORD)(g_mp3.pcm_bytes.size() - (size_t)byte_offset);

    if (waveOutPrepareHeader(g_mp3.device, &g_mp3.header, sizeof(g_mp3.header)) != MMSYSERR_NOERROR) return;
    g_mp3.header_prepared = true;
    if (waveOutWrite(g_mp3.device, &g_mp3.header, sizeof(g_mp3.header)) != MMSYSERR_NOERROR) return;

    ULONGLONG now = GetTickCount64();
    g_mp3.play_started_tick = now - (ULONGLONG)target_ms;
    g_mp3.pause_started_tick = 0;
    g_mp3.paused_ms = 0;
    g_mp3.state = mp3_playing;

    if (was_paused) {
        waveOutPause(g_mp3.device);
        g_mp3.state = mp3_paused;
        g_mp3.pause_started_tick = now;
    }
}

static bool mp3_toggle_pause() {
    std::lock_guard<std::mutex> guard(g_mp3.lock);
    if (!g_mp3.device) return false;

    if (g_mp3.state == mp3_paused) {
        MMRESULT mm = waveOutRestart(g_mp3.device);
        if (mm != MMSYSERR_NOERROR) return false;
        if (g_mp3.pause_started_tick != 0) {
            ULONGLONG now = GetTickCount64();
            if (now >= g_mp3.pause_started_tick)
                g_mp3.paused_ms += now - g_mp3.pause_started_tick;
        }
        g_mp3.pause_started_tick = 0;
        g_mp3.state = mp3_playing;
        return true;
    }

    if (g_mp3.state == mp3_playing) {
        MMRESULT mm = waveOutPause(g_mp3.device);
        if (mm != MMSYSERR_NOERROR) return false;
        if (g_mp3.pause_started_tick == 0)
            g_mp3.pause_started_tick = GetTickCount64();
        g_mp3.state = mp3_paused;
        return true;
    }

    return false;
}

static std::string mp3_ui_line() {
    mp3_snapshot_t snap = mp3_snapshot();

    std::string bars = snap.bars;
    std::string state_icon = "▶";
    const char* state_color = GRAY;
    if (snap.state == mp3_paused) {
        state_icon = "▌▌";
        state_color = RED;
    } else if (snap.state == mp3_stopped) {
        state_icon = "■";
        state_color = RED;
    } else if (snap.state == mp3_loading) {
        state_icon = "⋯";
        state_color = YELLOW;
    }

    int elapsed_s = snap.elapsed_ms / 1000;
    int total_s = (int)(snap.total_sec + 0.5);
    char time_buf[64] = {};
    snprintf(time_buf, sizeof(time_buf), "%02d:%02d/%02d:%02d",
        elapsed_s / 60, elapsed_s % 60, total_s / 60, total_s % 60);

    char vol_buf[16] = {};
    snprintf(vol_buf, sizeof(vol_buf), "[%d%%]", snap.volume);
    std::string vol_text = vol_buf;
    std::string file_text = mp3_name_only(snap.path);
    std::string counter_text;
    if (snap.folder_total > 0) {
        char cbuf[16];
        snprintf(cbuf, sizeof(cbuf), "[%d/%d]", snap.folder_pos, snap.folder_total);
        counter_text = cbuf;
    }
    int width = term_width();
    int fixed_visible = 14 + 1 + (int)strlen(time_buf) + 1 + (int)vol_text.size() + 1 + (int)state_icon.size() + (counter_text.empty() ? 0 : 1 + (int)counter_text.size());
    int remain = width - fixed_visible - 1;
    if (remain < 0) remain = 0;
    if (!file_text.empty()) {
        remain -= 1;
        if (remain < 0) remain = 0;
        if ((int)file_text.size() > remain) {
            if (remain <= 3) file_text = "";
            else file_text = file_text.substr(0, remain - 3) + "...";
        }
    }

    std::string line = bars;
    line += " ";
    line += MAGENTA;
    line += time_buf;
    line += RESET;
    line += " ";
    line += MAGENTA;
    line += vol_text;
    line += RESET;
    line += " ";
    line += state_color;
    line += state_icon;
    line += RESET;
    if (!counter_text.empty()) {
        line += " ";
        line += GRAY;
        line += counter_text;
        line += RESET;
    }
    if (!file_text.empty()) {
        line += " ";
        line += GRAY;
        line += file_text;
        line += RESET;
    }
    return line;
}

static bool mp3_ui_key_pressed() {
    DWORD wait = WaitForSingleObject(in_h, 8);
    if (wait != WAIT_OBJECT_0) return false;

    DWORD count = 0;
    GetNumberOfConsoleInputEvents(in_h, &count);
    if (!count) return false;

    std::vector<INPUT_RECORD> records(count);
    DWORD read = 0;
    if (!ReadConsoleInputW(in_h, records.data(), count, &read)) return false;

    bool exit_ui = false;
    bool do_next = false;
    bool do_prev = false;
    for (DWORD i = 0; i < read; i++) {
        const INPUT_RECORD& rec = records[i];
        if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown) continue;
        WORD vk    = rec.Event.KeyEvent.wVirtualKeyCode;
        DWORD ctrl = rec.Event.KeyEvent.dwControlKeyState;
        bool  held_ctrl = (ctrl & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
        if (vk == VK_ESCAPE) {
            exit_ui = true;
        } else if (vk == VK_UP) {
            std::lock_guard<std::mutex> guard(g_mp3.lock);
            mp3_apply_volume_locked(g_mp3.volume + 5);
        } else if (vk == VK_DOWN) {
            std::lock_guard<std::mutex> guard(g_mp3.lock);
            mp3_apply_volume_locked(g_mp3.volume - 5);
        } else if (vk == VK_SPACE) {
            mp3_toggle_pause();
        } else if (vk == VK_LEFT && held_ctrl) {
            do_prev = true;
        } else if (vk == VK_RIGHT && held_ctrl) {
            do_next = true;
        } else if (vk == VK_LEFT) {
            std::lock_guard<std::mutex> guard(g_mp3.lock);
            mp3_seek_locked(mp3_elapsed_ms_locked() - 10000);
        } else if (vk == VK_RIGHT) {
            std::lock_guard<std::mutex> guard(g_mp3.lock);
            mp3_seek_locked(mp3_elapsed_ms_locked() + 10000);
        }
    }
    if (do_next) {
        std::string next_path;
        {
            std::lock_guard<std::mutex> guard(g_mp3.lock);
            if (!g_mp3.folder_tracks.empty()) {
                int idx = mp3_folder_pick_next();
                if (idx >= 0) {
                    g_mp3.folder_played.insert(idx);
                    g_mp3.folder_history.push_back(g_mp3.folder_tracks[idx]);
                    next_path = g_mp3.folder_tracks[idx];
                }
            }
        }
        if (!next_path.empty()) { mp3_stop_playback(); mp3_start_track(next_path); }
    }
    if (do_prev) {
        std::string prev_path;
        {
            std::lock_guard<std::mutex> guard(g_mp3.lock);
            // history tail is current, one before that is prev
            if (g_mp3.folder_history.size() >= 2) {
                g_mp3.folder_history.pop_back(); // drop current
                prev_path = g_mp3.folder_history.back();
            }
        }
        if (!prev_path.empty()) { mp3_stop_playback(); mp3_start_track(prev_path); }
    }
    return exit_ui;
}

static int mp3_ui() {
    mp3_snapshot_t snap = mp3_snapshot();
    if (snap.state == mp3_stopped && snap.path.empty()) {
        err("mp3: nothing is playing\r\n");
        return 1;
    }

    while (true) {
        snap = mp3_snapshot();
        // Track finished naturally — try auto-advance in folder mode
        if (snap.state == mp3_stopped && snap.path.empty()) {
            std::string next_path;
            {
                std::lock_guard<std::mutex> guard(g_mp3.lock);
                if (!g_mp3.folder_tracks.empty()) {
                    int idx = mp3_folder_pick_next();
                    if (idx >= 0) {
                        g_mp3.folder_played.insert(idx);
                        g_mp3.folder_history.push_back(g_mp3.folder_tracks[idx]);
                        next_path = g_mp3.folder_tracks[idx];
                    } else {
                        // All tracks played — clear and stop
                        mp3_folder_clear();
                    }
                }
            }
            if (!next_path.empty()) {
                mp3_stop_playback();
                mp3_start_track(next_path);
                continue;
            }
            break;
        }
        out("\r\x1b[2K" + mp3_ui_line());
        if (mp3_ui_key_pressed()) break;
    }
    out("\r\x1b[2K");
    return 0;
}

static int mp3_cmd(const std::string& line) {
    std::string args = line.size() > 3 ? mp3_trim(line.substr(3)) : "";
    std::string lower = args;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    if (args.empty()) {
        out(
            GREEN "mp3" RESET " <file>        Play one MP3 file\r\n"
            GREEN "mp3" RESET " <folder>      Play all MP3s in folder recursively (shuffled)\r\n"
            GREEN "mp3 next" RESET "          Skip to next random track in folder\r\n"
            GREEN "mp3 prev" RESET "          Go back to previous track\r\n"
            GREEN "mp3 pause" RESET "         Pause playback\r\n"
            GREEN "mp3 resume" RESET "        Resume playback\r\n"
            GREEN "mp3 stop" RESET "          Stop playback and clear queue\r\n"
            GREEN "mp3 vol" RESET " <0-100>   Set playback volume\r\n"
            GREEN "mp3 status" RESET "        Show playback status\r\n"
            GREEN "mp3 ui" RESET "            Show now-playing line  Space:pause  ←→:seek  Ctrl+←→:prev/next  ↑↓:vol  Esc:exit\r\n"
        );
        return 0;
    }

    if (lower == "status") return mp3_show_status();

    if (lower == "next") {
        std::string next_path;
        {
            std::lock_guard<std::mutex> guard(g_mp3.lock);
            if (g_mp3.folder_tracks.empty()) {
                err("mp3: not in folder mode\r\n");
                return 1;
            }
            int idx = mp3_folder_pick_next();
            if (idx < 0) {
                out("mp3: all tracks played\r\n");
                mp3_stop_playback();
                return 0;
            }
            g_mp3.folder_played.insert(idx);
            g_mp3.folder_history.push_back(g_mp3.folder_tracks[idx]);
            next_path = g_mp3.folder_tracks[idx];
        }
        mp3_stop_playback();
        mp3_start_track(next_path);
        return 0;
    }

    if (lower == "prev") {
        std::string prev_path;
        {
            std::lock_guard<std::mutex> guard(g_mp3.lock);
            if (g_mp3.folder_tracks.empty()) {
                err("mp3: not in folder mode\r\n");
                return 1;
            }
            if (g_mp3.folder_history.size() < 2) {
                err("mp3: no previous track\r\n");
                return 1;
            }
            g_mp3.folder_history.pop_back();
            prev_path = g_mp3.folder_history.back();
        }
        mp3_stop_playback();
        mp3_start_track(prev_path);
        return 0;
    }

    if (lower == "stop") {
        mp3_stop_playback();
        mp3_folder_clear();
        out("mp3: stopped\r\n");
        return 0;
    }

    if (lower == "pause") {
        std::lock_guard<std::mutex> guard(g_mp3.lock);
        if (!g_mp3.device) {
            err("mp3: nothing is playing\r\n");
            return 1;
        }
        MMRESULT mm = waveOutPause(g_mp3.device);
        if (mm != MMSYSERR_NOERROR) {
            err("mp3: pause failed\r\n");
            return 1;
        }
        if (g_mp3.state == mp3_playing && g_mp3.pause_started_tick == 0)
            g_mp3.pause_started_tick = GetTickCount64();
        g_mp3.state = mp3_paused;
        return 0;
    }

    if (lower == "resume") {
        std::lock_guard<std::mutex> guard(g_mp3.lock);
        if (!g_mp3.device) {
            err("mp3: nothing is playing\r\n");
            return 1;
        }
        MMRESULT mm = waveOutRestart(g_mp3.device);
        if (mm != MMSYSERR_NOERROR) {
            err("mp3: resume failed\r\n");
            return 1;
        }
        if (g_mp3.pause_started_tick != 0) {
            ULONGLONG now = GetTickCount64();
            if (now >= g_mp3.pause_started_tick)
                g_mp3.paused_ms += now - g_mp3.pause_started_tick;
        }
        g_mp3.pause_started_tick = 0;
        g_mp3.state = mp3_playing;
        return 0;
    }

    if (lower == "ui") return mp3_ui();

    if (lower.size() >= 3 && lower.substr(0, 3) == "vol") {
        std::string rest = mp3_trim(args.substr(3));
        if (rest.empty()) {
            out("Volume: " + std::to_string(g_mp3.volume) + "%\r\n");
            return 0;
        }
        for (char c : rest) {
            if (c < '0' || c > '9') {
                err("mp3: volume must be a number from 0 to 100\r\n");
                return 1;
            }
        }
        int volume = atoi(rest.c_str());
        if (volume < 0) volume = 0;
        if (volume > 100) volume = 100;
        std::lock_guard<std::mutex> guard(g_mp3.lock);
        mp3_apply_volume_locked(volume);
        out("Volume: " + std::to_string(volume) + "%\r\n");
        return 0;
    }

    return mp3_play_file(args);
}

static void mp3_shutdown() {
    mp3_stop_playback();
}
