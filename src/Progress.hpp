#ifndef FILECAST_PROGRESS_H
#define FILECAST_PROGRESS_H

// Human-readable formatting and a throttled single-line progress bar. The bar is
// drawn on stderr with a carriage return so stdout stays clean for piping; in
// verbose mode it is silent and the caller logs per packet instead.

#include <cstdint>
#include <cstdio>
#include <chrono>
#include <string>

namespace Progress {

// e.g. 1610612736 -> "1.5 GiB"
inline std::string humanBytes(uint64_t n) {
    const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(n);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), unit == 0 ? "%.0f %s" : "%.1f %s", value, units[unit]);
    return buf;
}

// bytes/second -> "12.3 MiB/s"
inline std::string humanRate(double bytes_per_sec) {
    if (bytes_per_sec < 0) bytes_per_sec = 0;
    return humanBytes(static_cast<uint64_t>(bytes_per_sec)) + "/s";
}

// seconds -> "3.2s", "1m03s" or ">99h" (clamped so a huge ETA can't overflow int)
inline std::string humanDuration(double sec) {
    char buf[32];
    if (sec < 0) sec = 0;
    if (sec < 60.0) {
        snprintf(buf, sizeof(buf), "%.1fs", sec);
    } else if (sec >= 360000.0) {  // >= 100 hours: report a bound, don't cast
        snprintf(buf, sizeof(buf), ">99h");
    } else {
        int total = static_cast<int>(sec + 0.5);
        snprintf(buf, sizeof(buf), "%dm%02ds", total / 60, total % 60);
    }
    return buf;
}

class Reporter {
public:
    Reporter() = default;

    void start(const std::string& label, uint64_t total, bool verbose) {
        verbose_ = verbose;
        label_   = label;
        total_   = total;
        start_   = std::chrono::steady_clock::now();
        last_    = start_;
        active_  = true;
    }

    // Redraw the bar for `done` bytes, at most ~10x/second. No-op in verbose mode.
    void update(uint64_t done) {
        if (verbose_ || !active_) return;
        auto now = std::chrono::steady_clock::now();
        if (done < total_ &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last_).count() < 100) {
            return;
        }
        last_ = now;

        double secs = std::chrono::duration<double>(now - start_).count();
        double rate = secs > 0 ? done / secs : 0;
        int pct = total_ ? static_cast<int>(done * 100 / total_) : 0;
        std::string eta = (rate > 0 && done < total_)
            ? humanDuration((total_ - done) / rate)
            : "--";

        fprintf(stderr, "\r\033[K%s: %d%% %s/%s  %s  ETA %s",
                label_.c_str(), pct, humanBytes(done).c_str(), humanBytes(total_).c_str(),
                humanRate(rate).c_str(), eta.c_str());
        fflush(stderr);
    }

    // Clear the bar. Does not print a summary — the caller prints its own on stdout.
    void finish() {
        if (verbose_ || !active_) return;
        fprintf(stderr, "\r\033[K");
        fflush(stderr);
        active_ = false;
    }

    double elapsed() const {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
    }

private:
    bool verbose_ = false;
    std::string label_;
    uint64_t total_ = 0;
    std::chrono::steady_clock::time_point start_;
    std::chrono::steady_clock::time_point last_;
    bool active_ = false;
};

}  // namespace Progress

#endif  // FILECAST_PROGRESS_H
