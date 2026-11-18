// CME MDP 3.0 (SBE schema 1, version 13) decoding for the order-book messages.
//
// Only the two templates the order book needs are decoded:
//   46 MDIncrementalRefreshBook       price-level entries (MBP-10) + the order events behind them
//   47 MDIncrementalRefreshOrderBook  order events that do not change the top 10 levels
// Everything else is skipped by its MsgSize.
//
// Decoding writes plain structs into an Events buffer; the book never sees raw bytes.
// All reads are little-endian unaligned loads via memcpy (compiles to a single mov).
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

namespace tickscope {

template <class T>
inline T rd(const uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

constexpr int64_t PRICE_NULL = INT64_MAX;
constexpr int32_t INT32_NULL = INT32_MAX;
constexpr uint64_t UINT64_NULL = UINT64_MAX;
constexpr uint8_t UINT8_NULL = 255;
constexpr uint8_t END_OF_EVENT = 0x80;   // MatchEventIndicator bit 7

enum Action : uint8_t { NEW = 0, CHANGE = 1, DELETE = 2, DELETE_THRU = 3, DELETE_FROM = 4, OVERLAY = 5 };
enum Side : uint8_t { BID = 0, ASK = 1 };

// One order event (from 46's order group or from 47).
struct OrderEvent {
    uint64_t order_id;
    uint64_t priority;      // UINT64_NULL if absent
    int64_t price;          // mantissa, 1e-9 x DisplayFactor
    int32_t qty;            // display qty, 0 if null
    int32_t security_id;
    uint8_t action;         // NEW / CHANGE / DELETE
    uint8_t side;           // BID / ASK
};

// One price-level entry of template 46 (CME's own MBP-10 view; used for validation).
struct LevelEvent {
    int64_t price;
    int32_t qty;
    int32_t orders;
    int32_t security_id;
    uint8_t level;          // 1..10
    uint8_t action;
    uint8_t side;
};

// Everything one packet produced, in feed order. `end_of_event` marks positions
// (index into `marks`) where a matching event ended, so validation can run there.
struct Events {
    struct Mark {
        uint32_t n_orders, n_levels;  // counts at the end of the message
        bool end_of_event;
    };
    std::vector<OrderEvent> orders;
    std::vector<LevelEvent> levels;
    std::vector<Mark> marks;          // one per decoded 46/47 message
    uint32_t seq = 0;
    uint64_t sending_time = 0;
    uint32_t skipped_refs = 0;        // 46 order entries whose ReferenceID points nowhere

    void clear() {
        orders.clear();
        levels.clear();
        marks.clear();
        skipped_refs = 0;
    }
};

// MDEntryType '0' bid, '1' offer. Implied ('E','F') and book reset ('J') are not orders.
inline bool book_side(uint8_t entry_type, uint8_t& side) {
    if (entry_type == '0') { side = BID; return true; }
    if (entry_type == '1') { side = ASK; return true; }
    return false;
}

// Stage "parse": packet header and message framing only. Each 46/47 message is recorded
// as (template, block length, body, end); everything else is skipped by MsgSize.
struct Frame {
    uint16_t tid, block;
    const uint8_t* body;
    const uint8_t* end;
};

struct Frames {
    uint32_t seq = 0;
    uint64_t sending_time = 0;
    uint32_t n = 0;
    bool ok = true;
    Frame f[64];
};

inline void frame_packet(const uint8_t* p, uint32_t len, Frames& fr) {
    fr.n = 0;
    fr.ok = len >= 12;
    if (!fr.ok) return;
    fr.seq = rd<uint32_t>(p);
    fr.sending_time = rd<uint64_t>(p + 4);
    uint32_t off = 12;
    while (off + 10 <= len) {
        const uint16_t size = rd<uint16_t>(p + off);
        if (size < 10 || off + size > len) { fr.ok = false; return; }
        const uint16_t tid = rd<uint16_t>(p + off + 4);
        if ((tid == 46 || tid == 47) && fr.n < 64)
            fr.f[fr.n++] = {tid, rd<uint16_t>(p + off + 2), p + off + 10, p + off + size};
        off += size;
    }
    fr.ok = off == len;
}

// Stage "decode": message bodies -> OrderEvent / LevelEvent. Returns false on a bad group.
inline bool decode_frames(const Frames& fr, Events& ev) {
    ev.seq = fr.seq;
    ev.sending_time = fr.sending_time;
    for (uint32_t k = 0; k < fr.n; ++k) {
        const Frame& m = fr.f[k];
        const uint8_t* body = m.body;
        const uint8_t* end = m.end;
        const uint8_t match = body[8];
        const uint8_t* g = body + m.block;
        if (m.tid == 46) {
            // NoMDEntries: groupSize = blockLength u16 + numInGroup u8
            const uint16_t eblk = rd<uint16_t>(g);
            const uint8_t ne = g[2];
            const uint8_t* e0 = g + 3;
            if (e0 + size_t(eblk) * ne > end) return false;
            // NoOrderIDEntries: groupSize8Byte = blockLength u16, numInGroup at offset 7
            const uint8_t* og = e0 + size_t(eblk) * ne;
            const uint16_t oblk = rd<uint16_t>(og);
            const uint8_t no = og[7];
            const uint8_t* o0 = og + 8;
            if (o0 + size_t(oblk) * no > end) return false;

            // Order entries first (they reference the MD entries for price/side/security).
            for (uint8_t i = 0; i < no; ++i) {
                const uint8_t* o = o0 + size_t(oblk) * i;
                const uint8_t ref = o[20];
                if (ref == 0 || ref == UINT8_NULL || ref > ne) { ++ev.skipped_refs; continue; }
                const uint8_t* e = e0 + size_t(eblk) * (ref - 1);
                uint8_t side;
                if (!book_side(e[26], side)) continue;
                const int32_t q = rd<int32_t>(o + 16);
                ev.orders.push_back({rd<uint64_t>(o), rd<uint64_t>(o + 8), rd<int64_t>(e),
                                     q == INT32_NULL ? 0 : q, rd<int32_t>(e + 12), o[21], side});
            }
            for (uint8_t i = 0; i < ne; ++i) {
                const uint8_t* e = e0 + size_t(eblk) * i;
                uint8_t side;
                if (!book_side(e[26], side)) continue;
                ev.levels.push_back({rd<int64_t>(e), rd<int32_t>(e + 8), rd<int32_t>(e + 20),
                                     rd<int32_t>(e + 12), e[24], e[25], side});
            }
        } else {
            const uint16_t eblk = rd<uint16_t>(g);
            const uint8_t ne = g[2];
            const uint8_t* e0 = g + 3;
            if (e0 + size_t(eblk) * ne > end) return false;
            for (uint8_t i = 0; i < ne; ++i) {
                const uint8_t* e = e0 + size_t(eblk) * i;
                uint8_t side;
                if (!book_side(e[33], side)) continue;
                const int32_t q = rd<int32_t>(e + 24);
                ev.orders.push_back({rd<uint64_t>(e), rd<uint64_t>(e + 8), rd<int64_t>(e + 16),
                                     q == INT32_NULL ? 0 : q, rd<int32_t>(e + 28), e[32], side});
            }
        }
        ev.marks.push_back({uint32_t(ev.orders.size()), uint32_t(ev.levels.size()),
                            (match & END_OF_EVENT) != 0});
    }
    return true;
}

// Both stages in one call.
inline bool decode_packet(const uint8_t* p, uint32_t n, Events& ev) {
    Frames fr;
    frame_packet(p, n, fr);
    return decode_frames(fr, ev) && fr.ok;
}

}  // namespace tickscope
