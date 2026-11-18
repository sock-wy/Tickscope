// Cheapest timestamp source per platform, converted to nanoseconds after the run.
//
//   macOS    mach_absolute_time   Apple Silicon: 24 MHz timer = 41.67 ns per tick (what steady_clock uses)
//   x86-64   rdtsc                ~0.3 ns resolution (invariant TSC), calibrated against steady_clock
//   other    steady_clock
//
// Reading cntvct_el0 directly on macOS is not reliable (the first Mac run produced
// non-monotonic values), so macOS always goes through mach_absolute_time.
//
// Per-packet percentiles cannot be finer than the resolution. Means over many packets are
// still accurate: each reading is off by less than one tick, with no systematic bias.
#pragma once
#include <chrono>
#include <cstdint>
#include <thread>
#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif

namespace tickscope {

inline uint64_t ticks() {
#if defined(__APPLE__)
    return mach_absolute_time();
#elif defined(__x86_64__)
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t(hi) << 32) | lo;
#else
    return uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

struct ClockInfo {
    double ns_per_tick;   // conversion factor
    double resolution_ns; // smallest non-zero step
    double overhead_ns;   // cost of one ticks() call, measured back to back
    const char* source;
    uint64_t backwards = 0; // reads lower than the previous one (must be 0)
};

inline ClockInfo calibrate_clock() {
    ClockInfo c{};
#if defined(__APPLE__)
    c.source = "mach_absolute_time";
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    c.ns_per_tick = double(tb.numer) / double(tb.denom);
#elif defined(__x86_64__)
    c.source = "rdtsc";
    const auto w0 = std::chrono::steady_clock::now();
    const uint64_t t0 = ticks();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const uint64_t t1 = ticks();
    const auto w1 = std::chrono::steady_clock::now();
    c.ns_per_tick = double(std::chrono::duration_cast<std::chrono::nanoseconds>(w1 - w0).count()) / double(t1 - t0);
#else
    c.source = "steady_clock";
    c.ns_per_tick = 1.0 * std::chrono::steady_clock::period::num * 1e9 / std::chrono::steady_clock::period::den;
#endif
    // overhead, smallest step, and a monotonicity check over 1M back-to-back reads
    constexpr int N = 1'000'000;
    uint64_t prev = ticks(), min_step = UINT64_MAX;
    const uint64_t a = prev;
    for (int i = 0; i < N; ++i) {
        const uint64_t t = ticks();
        if (t < prev) ++c.backwards;
        else if (t > prev && t - prev < min_step) min_step = t - prev;
        prev = t;
    }
    c.overhead_ns = double(prev - a) * c.ns_per_tick / N;
    c.resolution_ns = double(min_step == UINT64_MAX ? 1 : min_step) * c.ns_per_tick;
    return c;
}

}  // namespace tickscope
