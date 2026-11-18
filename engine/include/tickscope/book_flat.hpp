// Optimization 1: price levels in a flat array indexed by price tick.
//
// Only the price-level container changes relative to BaselineBook:
//   baseline  std::map<price, Level>   red-black tree: O(log n), one heap node per level,
//                                      pointer chasing on every lookup
//   flat      std::vector<Level>       slot = (price - base) / tick: O(1), contiguous memory;
//                                      best price kept as an index and moved by a short scan
// Orders (std::unordered_map) and queues (std::list) are unchanged, so the measured gain is
// due to the level container alone.
//
// The tick of each security must be known up front (set_tick). In production it comes from
// the instrument definitions (template 54); the captures have none, so replay derives it from
// the prices before timing starts.
#pragma once
#include <cstdint>
#include <list>
#include <unordered_map>
#include <vector>

#include "book_baseline.hpp"   // BookStats
#include "mdp3.hpp"

namespace tickscope {

class FlatBook {
public:
    struct Level {
        int64_t qty = 0;
        int32_t count = 0;
        std::list<uint64_t> queue;
    };

    void set_tick(int32_t sid, int64_t tick) {
        Security& s = secs_[sid];
        s.side[BID].tick = s.side[ASK].tick = tick;
    }

    void apply(const OrderEvent& e) {
        auto it = orders_.find(e.order_id);
        switch (e.action) {
        case NEW:
            if (it != orders_.end()) {
                remove_from_level(it->second);
                orders_.erase(it);
                ++stats_.new_existing;
            }
            insert(e);
            ++stats_.n_new;
            break;
        case CHANGE:
            if (it == orders_.end()) {
                insert(e);
                ++stats_.change_unknown;
                return;
            }
            {
                Order& o = it->second;
                remove_from_level(o);
                o.side = e.side;
                o.price = e.price;
                o.qty = e.qty;
                if (e.priority != UINT64_NULL) o.priority = e.priority;
                add_to_level(o, e.order_id);
            }
            ++stats_.n_change;
            break;
        case DELETE:
            if (it == orders_.end()) { ++stats_.delete_unknown; return; }
            remove_from_level(it->second);
            orders_.erase(it);
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

    size_t resting_orders() const { return orders_.size(); }
    const BookStats& stats() const { return stats_; }
    uint64_t off_tick() const { return off_tick_; }

private:
    struct Order {
        int32_t security_id;
        uint8_t side;
        int64_t price;
        int32_t qty;
        uint64_t priority;
        std::list<uint64_t>::iterator pos;
    };

    struct PriceArray {
        bool is_bid = false;
        int64_t tick = 1'000'000'000;   // fallback; set_tick overrides
        int64_t base = 0;               // price of slots[0]
        std::vector<Level> slots;
        int32_t best = -1;              // index of the best non-empty level, -1 if none
        int32_t nonempty = 0;

        int64_t index(int64_t px) const { return (px - base) / tick; }

        // Slot index for px, growing the array if px falls outside it.
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
                for (size_t j = 0; j < slots.size(); ++j) grown[j + size_t(shift)] = std::move(slots[j]);
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
        Order& o = orders_[e.order_id];
        o = Order{e.security_id, e.side, e.price, e.qty, e.priority, {}};
        add_to_level(o, e.order_id);
    }

    void add_to_level(Order& o, uint64_t oid) {
        PriceArray& a = secs_[o.security_id].side[o.side];
        if ((o.price - a.base) % a.tick != 0 && !a.slots.empty()) ++off_tick_;
        const size_t i = a.slot(o.price);
        Level& l = a.slots[i];
        l.qty += o.qty;
        if (l.count++ == 0) a.added(int32_t(i));
        o.pos = l.queue.insert(l.queue.end(), oid);
    }

    void remove_from_level(Order& o) {
        PriceArray& a = secs_[o.security_id].side[o.side];
        const int64_t i = a.index(o.price);
        Level& l = a.slots[size_t(i)];
        l.qty -= o.qty;
        l.queue.erase(o.pos);
        if (--l.count == 0) {
            l.qty = 0;
            a.emptied(int32_t(i));
        }
    }

    std::unordered_map<uint64_t, Order> orders_;
    std::unordered_map<int32_t, Security> secs_;
    BookStats stats_;
    uint64_t off_tick_ = 0;
};

}  // namespace tickscope
