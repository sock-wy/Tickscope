// TickScope replay: feeds captured CME packets through the full pipeline and times
// seven stages per packet, from "packet available" to "order and snapshot out".
//
//   replay [--book baseline|flat|pool|hash] [--speed X] [--validate SID] [--out results.json] file.slim ...
//
//   --book      order-book implementation (default baseline):
//               baseline  std::map levels, std::unordered_map orders, std::list queues
//               flat      + price levels in a flat array            (optimization 1)
//               pool      + order pool, intrusive queues            (optimization 2)
//               hash      + open-addressing OrderID index           (optimization 3)
//   --speed     0 = back to back, as fast as possible (default); X > 0 = recorded pace sped up
//               X times (1 = real time). Paced runs measure stage 1 (waiting behind earlier packets).
//   --max-seconds  replay only the first N seconds of each stream
//   --merge     treat each folder as one day and interleave the days by time since their own
//               start (IDs are remapped per day so the books stay separate): a synthetic load of
//               several real sessions at once
//   --validate  also rebuild CME's MBP-10 and compare it with the L3 book at the end of
//               every matching event (results must equal book_py/l3book.py)
//
// Input: uncompressed .slim (tools/prep_slim.py): records <QIHH> = capture ts_ns, dst ip,
// dst port, payload length, then the UDP payload. All files are loaded into memory first.
//
// Stages (timestamps T0..T7):
//   1 queue     T0 packet due (paced) or loop start -> T1 processing starts
//   2 parse     packet header, sequence check, message framing
//   3 decode    message bodies -> OrderEvent / LevelEvent
//   4 apply     order events into the L3 book
//   5 tob       best bid / ask of every security the packet touched
//   6 strategy  decision on the front-month top of book (stub: imbalance rule)
//   7 out       encode the order (if any) and publish the top-of-book snapshot (seqlock)
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <numeric>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "tickscope/book_baseline.hpp"
#include "tickscope/book_flat.hpp"
#include "tickscope/book_pool.hpp"
#include "tickscope/clock.hpp"
#include "tickscope/mbp10.hpp"
#include "tickscope/mdp3.hpp"

using namespace tickscope;

struct Packet {
    uint64_t capture_ns;
    uint64_t t;           // replay time: ns since the start of its own stream
    const uint8_t* data;
    uint32_t len;
    uint8_t stream;       // 0 unless --merge combines several days
};

// Streams of different days share SecurityIDs and OrderIDs, so merged streams get their own
// ID ranges. Stream 0 keeps the original IDs.
inline void remap(Events& ev, uint8_t stream) {
    if (!stream) return;
    const int32_t sid_off = int32_t(stream) << 28;
    const uint64_t oid_tag = uint64_t(stream) << 56;
    for (auto& o : ev.orders) { o.security_id += sid_off; o.order_id ^= oid_tag; }
    for (auto& l : ev.levels) l.security_id += sid_off;
}

static std::vector<uint8_t> read_file(const char* path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) { std::fprintf(stderr, "cannot open %s\n", path); std::exit(1); }
    std::vector<uint8_t> buf(size_t(f.tellg()));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(buf.data()), std::streamsize(buf.size()));
    return buf;
}

static void index_packets(const std::vector<uint8_t>& buf, uint8_t stream, std::vector<Packet>& out) {
    size_t off = 0;
    while (off + 16 <= buf.size()) {
        const uint8_t* h = buf.data() + off;
        const uint16_t len = rd<uint16_t>(h + 14);
        if (off + 16 + len > buf.size()) break;
        out.push_back({rd<uint64_t>(h), 0, h + 16, len, stream});
        off += 16 + len;
    }
}

// Tick size per security = gcd of every price seen (stand-in for template 54 definitions).
static std::unordered_map<int32_t, int64_t> derive_ticks(const std::vector<Packet>& packets) {
    std::unordered_map<int32_t, int64_t> g;
    Events ev;
    for (const auto& pk : packets) {
        ev.clear();
        decode_packet(pk.data, pk.len, ev);
        remap(ev, pk.stream);
        for (const auto& o : ev.orders)
            if (o.price != PRICE_NULL) g[o.security_id] = std::gcd(g[o.security_id], o.price < 0 ? -o.price : o.price);
        for (const auto& l : ev.levels)
            if (l.price != PRICE_NULL) g[l.security_id] = std::gcd(g[l.security_id], l.price < 0 ? -l.price : l.price);
    }
    for (auto& [sid, t] : g)
        if (t == 0) t = 1'000'000'000;
    return g;
}

// ---- stages 6 and 7: strategy stub, order encoding, snapshot publishing ----------------

struct OrderMsg {              // fixed-size outbound order (iLink-like layout, not sent anywhere)
    uint64_t client_id;
    int64_t price;
    int32_t security_id;
    int32_t qty;
    uint8_t side;
    uint8_t pad[7];
};

struct Snapshot {              // what the live viewer will read from shared memory
    uint64_t seq;              // seqlock: odd while writing
    uint64_t sending_time;
    int64_t bid_px, bid_qty, ask_px, ask_qty;
};

struct Outbox {
    std::vector<OrderMsg> ring = std::vector<OrderMsg>(4096);
    uint64_t sent = 0;
    Snapshot snap{};
};

// Imbalance rule on the front month: join the bid when it is 4x the ask (and vice versa).
// The rule only exists to put a realistic decision on the critical path.
struct Strategy {
    int32_t sid;
    uint64_t next_id = 1;
    bool decide(int64_t bpx, int64_t bq, int64_t apx, int64_t aq, OrderMsg& out) {
        if (bq > 4 * aq) { out = {next_id++, bpx, sid, 1, BID, {}}; return true; }
        if (aq > 4 * bq) { out = {next_id++, apx, sid, 1, ASK, {}}; return true; }
        return false;
    }
};

// ---- statistics --------------------------------------------------------------------------

struct Pct {
    double p50, p90, p99, p999, max, mean;
};

static Pct percentiles(std::vector<uint32_t> v, double ns_per_tick) {
    if (v.empty()) return {0, 0, 0, 0, 0, 0};
    std::sort(v.begin(), v.end());
    auto at = [&](double q) { return double(v[std::min(v.size() - 1, size_t(q * double(v.size())))]) * ns_per_tick; };
    double sum = 0;
    for (auto x : v) sum += x;
    return {at(0.5), at(0.9), at(0.99), at(0.999), double(v.back()) * ns_per_tick, sum / double(v.size()) * ns_per_tick};
}

struct Options {
    std::string book = "baseline";
    double speed = 0;
    double max_seconds = 0;     // replay only the first N seconds of each stream (0 = all)
    bool merge = false;         // files in different folders = different days, interleaved
    int32_t validate_sid = -1;
    int32_t strategy_sid = 4916;
    std::string out_path;
    std::vector<std::string> files;
};

template <class Book>
static int run(const Options& opt, const std::vector<Packet>& packets) {
    const bool validate = opt.validate_sid >= 0;
    const ClockInfo clk = calibrate_clock();
    if (clk.backwards || clk.overhead_ns > 1000) {
        std::fprintf(stderr, "clock %s unusable: %llu backward steps, %.0f ns per read\n", clk.source,
                     (unsigned long long)clk.backwards, clk.overhead_ns);
        return 3;
    }

    Book book;
    if constexpr (requires(Book b) { b.set_tick(0, 1); }) {
        for (auto& [sid, t] : derive_ticks(packets)) book.set_tick(sid, t);
    }
    MBP10 mbp;
    Events ev;
    ev.orders.reserve(256);
    ev.levels.reserve(256);
    ev.marks.reserve(64);
    Frames fr;
    Outbox outbox;
    Strategy strat{opt.strategy_sid};

    constexpr int NS = 7;
    static const char* names[NS] = {"1_queue", "2_parse", "3_decode", "4_apply", "5_top_of_book", "6_strategy", "7_out"};
    std::vector<uint32_t> stage[NS];
    std::vector<uint32_t> total;
    for (auto& v : stage) v.reserve(packets.size());
    total.reserve(packets.size());

    uint64_t events_compared = 0, levels_compared = 0, levels_matched = 0;
    uint64_t by_level[10] = {}, by_level_match[10] = {};
    uint64_t framing_errors = 0, seq_gaps = 0, skipped_refs = 0, orders_out = 0;
    uint32_t last_seq[256] = {};
    std::set<uint64_t> touched;
    std::vector<int32_t> tob_sids;
    tob_sids.reserve(64);

    const double tick_per_ns = 1.0 / clk.ns_per_tick;
    // offered load: average rate and busiest millisecond, before speed-up
    const uint64_t span_ns = packets.empty() ? 1 : std::max<uint64_t>(packets.back().t, 1);
    uint64_t peak_ms = 0;
    for (size_t i = 0, j = 0; i < packets.size(); ++i) {
        while (packets[i].t - packets[j].t >= 1'000'000) ++j;
        peak_ms = std::max<uint64_t>(peak_ms, i - j + 1);
    }
    int n_streams = 0;
    for (const auto& pk : packets) n_streams = std::max(n_streams, int(pk.stream) + 1);
    const uint64_t start = ticks();

    for (const Packet& pk : packets) {
        uint64_t t0;
        if (opt.speed > 0) {        // wait until this packet is due at the chosen pace
            t0 = start + uint64_t(double(pk.t) / opt.speed * tick_per_ns);
            while (ticks() < t0) {}
        } else {
            t0 = ticks();
        }
        const uint64_t t1 = ticks();

        frame_packet(pk.data, pk.len, fr);
        if (!fr.ok) ++framing_errors;
        if (last_seq[pk.stream] && fr.seq != last_seq[pk.stream] + 1) ++seq_gaps;
        last_seq[pk.stream] = fr.seq;
        const uint64_t t2 = ticks();

        ev.clear();
        if (!decode_frames(fr, ev)) ++framing_errors;
        remap(ev, pk.stream);
        const uint64_t t3 = ticks();

        tob_sids.clear();
        uint32_t oi = 0, li = 0;
        for (const auto& m : ev.marks) {
            for (; oi < m.n_orders; ++oi) {
                book.apply(ev.orders[oi]);
                tob_sids.push_back(ev.orders[oi].security_id);
            }
            if (validate) {
                for (; li < m.n_levels; ++li) {
                    const LevelEvent& le = ev.levels[li];
                    mbp.apply(le);
                    touched.insert(MBP10::key(le.security_id, le.side));
                }
                if (m.end_of_event && !touched.empty()) {
                    ++events_compared;
                    for (uint64_t k : touched) {
                        const int32_t sid = int32_t(k >> 1);
                        const uint8_t side = uint8_t(k & 1);
                        if (sid != opt.validate_sid) continue;
                        const auto& slots = mbp.side(sid, side);
                        for (int j = 0; j < 10; ++j) {
                            if (!slots[j].used) continue;
                            ++levels_compared;
                            ++by_level[j];
                            const auto* l = book.level(sid, side, slots[j].price);
                            if (l && l->qty == slots[j].qty && l->count == slots[j].orders) {
                                ++levels_matched;
                                ++by_level_match[j];
                            }
                        }
                    }
                    touched.clear();
                }
            }
        }
        const uint64_t t4 = ticks();

        // stage 5: top of book for touched securities; keep the strategy security's quote
        std::sort(tob_sids.begin(), tob_sids.end());
        tob_sids.erase(std::unique(tob_sids.begin(), tob_sids.end()), tob_sids.end());
        bool have_quote = false;
        int64_t bpx = 0, bq = 0, apx = 0, aq = 0;
        for (int32_t sid : tob_sids) {
            int64_t p1, q1, p2, q2;
            const bool hb = book.best(sid, BID, p1, q1);
            const bool ha = book.best(sid, ASK, p2, q2);
            if (sid == strat.sid && hb && ha) { have_quote = true; bpx = p1; bq = q1; apx = p2; aq = q2; }
        }
        const uint64_t t5 = ticks();

        OrderMsg om;
        const bool fire = have_quote && strat.decide(bpx, bq, apx, aq, om);
        const uint64_t t6 = ticks();

        if (fire) {
            outbox.ring[outbox.sent++ & 4095] = om;
            ++orders_out;
        }
        if (have_quote) {
            Snapshot& s = outbox.snap;
            __atomic_store_n(&s.seq, s.seq + 1, __ATOMIC_RELEASE);
            s.sending_time = fr.sending_time;
            s.bid_px = bpx; s.bid_qty = bq; s.ask_px = apx; s.ask_qty = aq;
            __atomic_store_n(&s.seq, s.seq + 1, __ATOMIC_RELEASE);
        }
        const uint64_t t7 = ticks();

        skipped_refs += ev.skipped_refs;
        const uint64_t ts[8] = {t0, t1, t2, t3, t4, t5, t6, t7};
        for (int k = 0; k < NS; ++k) stage[k].push_back(uint32_t(ts[k + 1] - ts[k]));
        total.push_back(uint32_t(t7 - t0));
    }
    const double wall_s = double(ticks() - start) * clk.ns_per_tick * 1e-9;

    // ---- report ----
    const auto& st = book.stats();
    std::string js;
    char b[640];
    std::snprintf(b, sizeof b,
                  "{\n \"engine\": \"%s\",\n \"mode\": \"%s\",\n \"speed\": %g,\n \"packets\": %zu,\n"
                  " \"wall_seconds\": %.3f,\n \"packets_per_second\": %.0f,\n"
                  " \"clock\": {\"source\": \"%s\", \"resolution_ns\": %.2f, \"overhead_ns\": %.2f, \"backwards\": %" PRIu64 "},\n"
                  " \"framing_errors\": %" PRIu64 ",\n \"sequence_gaps\": %" PRIu64 ",\n"
                  " \"orders_out\": %" PRIu64 ",\n",
                  opt.book.c_str(), validate ? "validate" : "timing", opt.speed, packets.size(), wall_s,
                  double(packets.size()) / wall_s, clk.source, clk.resolution_ns, clk.overhead_ns, clk.backwards,
                  framing_errors, seq_gaps, orders_out);
    js += b;
    const double speed_x = opt.speed > 0 ? opt.speed : 0;
    std::snprintf(b, sizeof b,
                  " \"load\": {\"streams\": %d, \"replayed_seconds\": %.1f, \"recorded_pps\": %.0f, "
                  "\"recorded_peak_per_ms\": %" PRIu64 ", \"offered_pps\": %.0f, \"offered_peak_pps\": %.0f},\n",
                  n_streams, double(span_ns) * 1e-9, double(packets.size()) / (double(span_ns) * 1e-9), peak_ms,
                  speed_x ? double(packets.size()) / (double(span_ns) * 1e-9) * speed_x : double(packets.size()) / wall_s,
                  speed_x ? double(peak_ms) * 1000.0 * speed_x : double(packets.size()) / wall_s);
    js += b;
    std::snprintf(b, sizeof b,
                  " \"order_events\": {\"new\": %" PRIu64 ", \"change\": %" PRIu64 ", \"delete\": %" PRIu64
                  ", \"new_existing\": %" PRIu64 ", \"change_unknown\": %" PRIu64 ", \"delete_unknown\": %" PRIu64
                  "},\n \"orders_resting_at_end\": %zu,\n \"skipped_order_refs\": %" PRIu64 ",\n",
                  st.n_new, st.n_change, st.n_delete, st.new_existing, st.change_unknown, st.delete_unknown,
                  book.resting_orders(), skipped_refs);
    js += b;
    auto line = [&](const char* name, const std::vector<uint32_t>& v, bool last) {
        Pct p = percentiles(v, clk.ns_per_tick);
        std::snprintf(b, sizeof b,
                      "  \"%s\": {\"mean\": %.1f, \"p50\": %.0f, \"p90\": %.0f, \"p99\": %.0f, \"p99.9\": %.0f, "
                      "\"max\": %.0f}%s\n",
                      name, p.mean, p.p50, p.p90, p.p99, p.p999, p.max, last ? "" : ",");
        js += b;
    };
    js += " \"latency_ns_per_packet\": {\n";
    for (int k = 0; k < NS; ++k) line(names[k], stage[k], false);
    line("total", total, true);
    js += " }";
    if (validate) {
        std::snprintf(b, sizeof b,
                      ",\n \"validation\": {\"security\": %d, \"events_compared\": %" PRIu64
                      ", \"levels_compared\": %" PRIu64 ", \"levels_matched\": %" PRIu64 ", \"match_pct\": %.3f, "
                      "\"match_pct_by_level\": [",
                      opt.validate_sid, events_compared, levels_compared, levels_matched,
                      100.0 * double(levels_matched) / double(std::max<uint64_t>(levels_compared, 1)));
        js += b;
        for (int j = 0; j < 10; ++j) {
            std::snprintf(b, sizeof b, "%s%.3f", j ? ", " : "",
                          100.0 * double(by_level_match[j]) / double(std::max<uint64_t>(by_level[j], 1)));
            js += b;
        }
        js += "]}";
    }
    js += "\n}\n";
    std::fputs(js.c_str(), stdout);
    if (!opt.out_path.empty()) std::ofstream(opt.out_path) << js;
    return 0;
}

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--validate" && i + 1 < argc) opt.validate_sid = std::atoi(argv[++i]);
        else if (a == "--out" && i + 1 < argc) opt.out_path = argv[++i];
        else if (a == "--book" && i + 1 < argc) opt.book = argv[++i];
        else if (a == "--speed" && i + 1 < argc) opt.speed = std::atof(argv[++i]);
        else if (a == "--max-seconds" && i + 1 < argc) opt.max_seconds = std::atof(argv[++i]);
        else if (a == "--merge") opt.merge = true;
        else opt.files.push_back(a);
    }
    if (opt.files.empty()) {
        std::fprintf(stderr, "usage: replay [--book baseline|flat|pool|hash] [--speed X] [--max-seconds N] [--merge] [--validate SID] [--out f.json] file.slim ...\n");
        return 2;
    }
    if (opt.merge && opt.validate_sid >= 0) {
        std::fprintf(stderr, "--validate cannot be combined with --merge\n");
        return 2;
    }
    // stream = folder (one day) when merging, otherwise everything is one stream
    std::vector<std::string> dirs;
    std::vector<std::vector<uint8_t>> buffers;
    std::vector<Packet> packets;
    buffers.reserve(opt.files.size());
    for (auto& f : opt.files) {
        uint8_t stream = 0;
        if (opt.merge) {
            const std::string dir = f.substr(0, f.find_last_of('/') == std::string::npos ? 0 : f.find_last_of('/'));
            auto it = std::find(dirs.begin(), dirs.end(), dir);
            if (it == dirs.end()) { dirs.push_back(dir); it = dirs.end() - 1; }
            stream = uint8_t(it - dirs.begin());
        }
        buffers.push_back(read_file(f.c_str()));
        index_packets(buffers.back(), stream, packets);
    }
    // align every stream to its own start, cut to --max-seconds, interleave by replay time
    std::vector<uint64_t> first(256, UINT64_MAX);
    for (const auto& pk : packets) first[pk.stream] = std::min(first[pk.stream], pk.capture_ns);
    for (auto& pk : packets) pk.t = pk.capture_ns - first[pk.stream];
    if (opt.max_seconds > 0) {
        const uint64_t lim = uint64_t(opt.max_seconds * 1e9);
        packets.erase(std::remove_if(packets.begin(), packets.end(), [&](const Packet& p) { return p.t >= lim; }),
                      packets.end());
    }
    std::stable_sort(packets.begin(), packets.end(), [](const Packet& a, const Packet& b) { return a.t < b.t; });
    if (opt.book == "baseline") return run<BaselineBook>(opt, packets);
    if (opt.book == "flat") return run<FlatBook>(opt, packets);
    if (opt.book == "pool") return run<PoolBook>(opt, packets);
    if (opt.book == "hash") return run<HashBook>(opt, packets);
    std::fprintf(stderr, "unknown --book %s\n", opt.book.c_str());
    return 2;
}
