// Optimizations 2 and 3, on top of the flat price array (optimization 1).
//
//   2 "pool"  Orders live in one contiguous pool (std::vector) with a free list, and each
//             price level's queue is an intrusive doubly linked list of pool indices.
//             No heap allocation per order or per queue node; an order is one 48-byte record.
//             The OrderID -> pool index lookup is still std::unordered_map.
//   3 "hash"  Same, with the lookup replaced by an open-addressing hash table (linear
//             probing, backward-shift deletion): one array, no nodes, no pointer chasing.
//
// Both are FlatPoolBook<Index>; only the Index type differs, so 2 vs 3 isolates the lookup.
#pragma once
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "book_baseline.hpp"   // BookStats
#include "mdp3.hpp"

namespace tickscope {

constexpr uint32_t NIL = UINT32_MAX;

// OrderID -> pool index, standard library version.
class StdIndex {
public:
    uint32_t find(uint64_t oid) const {
        auto it = m_.find(oid);
        return it == m_.end() ? NIL : it->second;
    }
    void insert(uint64_t oid, uint32_t idx) { m_[oid] = idx; }
    void erase(uint64_t oid) { m_.erase(oid); }
    size_t size() const { return m_.size(); }

private:
    std::unordered_map<uint64_t, uint32_t> m_;
};

// OrderID -> pool index, open addressing with linear probing.
class OpenIndex {
public:
    OpenIndex() { rehash(1u << 12); }   // grows x2 at 50% load; small start keeps it in cache

    uint32_t find(uint64_t oid) const {
        for (size_t i = slot(oid);; i = (i + 1) & mask_) {
            const Entry& e = t_[i];
            if (e.key == oid) return e.val;
            if (e.key == EMPTY) return NIL;
        }
    }

    void insert(uint64_t oid, uint32_t idx) {
        if ((n_ + 1) * 2 > t_.size()) rehash(t_.size() * 2);   // keep load <= 50%
        for (size_t i = slot(oid);; i = (i + 1) & mask_) {
            Entry& e = t_[i];
            if (e.key == oid) { e.val = idx; return; }
            if (e.key == EMPTY) { e = {oid, idx}; ++n_; return; }
        }
    }

    void erase(uint64_t oid) {
        size_t i = slot(oid);
        while (t_[i].key != oid) {
            if (t_[i].key == EMPTY) return;
            i = (i + 1) & mask_;
        }
        // backward-shift deletion: pull later entries of the same probe run into the hole
        size_t hole = i;
        for (size_t j = (i + 1) & mask_; t_[j].key != EMPTY; j = (j + 1) & mask_) {
            const size_t home = slot(t_[j].key);
            const bool movable = (hole <= j) ? (home <= hole || home > j) : (home <= hole && home > j);
            if (movable) {
                t_[hole] = t_[j];
                hole = j;
            }
        }
        t_[hole].key = EMPTY;
        --n_;
    }

    size_t size() const { return n_; }

private:
    static constexpr uint64_t EMPTY = UINT64_MAX;
    struct Entry {
        uint64_t key = EMPTY;
        uint32_t val = NIL;
    };

    size_t slot(uint64_t k) const { return size_t((k * 0x9E3779B97F4A7C15ull) >> shift_); }

    void rehash(size_t cap) {
        std::vector<Entry> old;
        old.swap(t_);
        t_.assign(cap, Entry{});
        mask_ = cap - 1;
        shift_ = 64 - __builtin_ctzll(cap);
        n_ = 0;
        for (const Entry& e : old)
            if (e.key != EMPTY) insert(e.key, e.val);
    }

    std::vector<Entry> t_;
    size_t mask_ = 0, n_ = 0;
    int shift_ = 0;
};

template <class Index>
class FlatPoolBook {
public:
    struct Level {
        int64_t qty = 0;
        int32_t count = 0;
        uint32_t head = NIL, tail = NIL;      // pool indices, oldest first
    };

    FlatPoolBook() { pool_.reserve(1u << 16); }

    void set_tick(int32_t sid, int64_t tick) {
        Security& s = secs_[sid];
        s.side[BID].tick = s.side[ASK].tick = tick;
    }

    void apply(const OrderEvent& e) {
        const uint32_t idx = index_.find(e.order_id);
        switch (e.action) {
        case NEW:
            if (idx != NIL) {
                unlink(idx);
                release(e.order_id, idx);
                ++stats_.new_existing;
            }
            insert(e);
            ++stats_.n_new;
            break;
        case CHANGE:
            if (idx == NIL) {
                insert(e);
                ++stats_.change_unknown;
                return;
            }
            {
                unlink(idx);
                Order& o = pool_[idx];
                o.side = e.side;
                o.price = e.price;
                o.qty = e.qty;
                if (e.priority != UINT64_NULL) o.priority = e.priority;
                link(idx);
            }
            ++stats_.n_change;
            break;
        case DELETE:
            if (idx == NIL) { ++stats_.delete_unknown; return; }
            unlink(idx);
            release(e.order_id, idx);
            ++stats_.n_delete;
            break;
        default:
            break;
        }
    }

    const Level* level(int32_t sid, uint8_t side, int64_t px) const {
        auto s = secs_.find(sid);
        if (s == secs_.end()) return nullptr;
        const PriceArray& a = s->second.side[side];
        const int64_t i = a.index(px);
        if (i < 0 || i >= int64_t(a.slots.size()) || a.slots[size_t(i)].count == 0) return nullptr;
        return &a.slots[size_t(i)];
    }

    bool best(int32_t sid, uint8_t side, int64_t& px, int64_t& qty) const {
        auto s = secs_.find(sid);
        if (s == secs_.end()) return false;
        const PriceArray& a = s->second.side[side];
        if (a.best < 0) return false;
        px = a.base + int64_t(a.best) * a.tick;
        qty = a.slots[size_t(a.best)].qty;
        return true;
    }

    size_t resting_orders() const { return index_.size(); }
    const BookStats& stats() const { return stats_; }

private:
    struct Order {                 // 48 bytes
        uint64_t order_id;
        uint64_t priority;
        int64_t price;
        int32_t qty;
        int32_t security_id;
        uint32_t prev, next;       // neighbours in the level queue; next doubles as free-list link
        uint8_t side;
    };

    struct PriceArray {
        bool is_bid = false;
        int64_t tick = 1'000'000'000;
        int64_t base = 0;
        std::vector<Level> slots;
        int32_t best = -1;
        int32_t nonempty = 0;

        int64_t index(int64_t px) const { return (px - base) / tick; }

        size_t slot(int64_t px) {
            constexpr int64_t W = 4096;
            if (slots.empty()) {
                base = px - (W / 2) * tick;
                slots.resize(W);
            }
            int64_t i = index(px);
            if (i < 0) {
                const int64_t shift = -i + int64_t(slots.size()) / 2;
                std::vector<Level> grown(slots.size() + size_t(shift));
                std::copy(slots.begin(), slots.end(), grown.begin() + shift);
                slots.swap(grown);
                base -= shift * tick;
                if (best >= 0) best += int32_t(shift);
                i += shift;
            } else if (i >= int64_t(slots.size())) {
                slots.resize(size_t(std::max<int64_t>(i + 1 + int64_t(slots.size()) / 2, 2 * int64_t(slots.size()))));
            }
            return size_t(i);
        }

        void added(int32_t i) {
            ++nonempty;
            if (best < 0 || (is_bid ? i > best : i < best)) best = i;
        }

        void emptied(int32_t i) {
            --nonempty;
            if (i != best) return;
            if (nonempty == 0) { best = -1; return; }
            if (is_bid) { do --best; while (slots[size_t(best)].count == 0); }
            else        { do ++best; while (slots[size_t(best)].count == 0); }
        }
    };

    struct Security {
        PriceArray side[2];
        Security() { side[BID].is_bid = true; }
    };

    void insert(const OrderEvent& e) {
        uint32_t idx;
        if (free_ != NIL) {
            idx = free_;
            free_ = pool_[idx].next;
        } else {
            idx = uint32_t(pool_.size());
            pool_.emplace_back();
        }
        pool_[idx] = Order{e.order_id, e.priority, e.price, e.qty, e.security_id, NIL, NIL, e.side};
        index_.insert(e.order_id, idx);
        link(idx);
    }

    void release(uint64_t oid, uint32_t idx) {
        index_.erase(oid);
        pool_[idx].next = free_;
        free_ = idx;
    }

    // append to the back of its price level
    void link(uint32_t idx) {
        Order& o = pool_[idx];
        PriceArray& a = secs_[o.security_id].side[o.side];
        const size_t i = a.slot(o.price);
        Level& l = a.slots[i];
        o.prev = l.tail;
        o.next = NIL;
        if (l.tail != NIL) pool_[l.tail].next = idx;
        else l.head = idx;
        l.tail = idx;
        l.qty += o.qty;
        if (l.count++ == 0) a.added(int32_t(i));
    }

    void unlink(uint32_t idx) {
        Order& o = pool_[idx];
        PriceArray& a = secs_[o.security_id].side[o.side];
        const int64_t i = a.index(o.price);
        Level& l = a.slots[size_t(i)];
        if (o.prev != NIL) pool_[o.prev].next = o.next;
        else l.head = o.next;
        if (o.next != NIL) pool_[o.next].prev = o.prev;
        else l.tail = o.prev;
        l.qty -= o.qty;
        if (--l.count == 0) {
            l.qty = 0;
            a.emptied(int32_t(i));
        }
    }

    std::vector<Order> pool_;
    uint32_t free_ = NIL;
    Index index_;
    std::unordered_map<int32_t, Security> secs_;
    BookStats stats_;
};

using PoolBook = FlatPoolBook<StdIndex>;
using HashBook = FlatPoolBook<OpenIndex>;

}  // namespace tickscope
