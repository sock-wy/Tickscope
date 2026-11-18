// Baseline L3 order book: the standard way most implementations are written.
//
//   orders   std::unordered_map<OrderID, Order>          O(1) lookup by id
//   levels   std::map<price, Level> per security & side  sorted price levels
//   queue    std::list<OrderID> per level                time priority, O(1) unlink
//
// Every container allocates on the heap through the default allocator. This is the
// reference that later versions (flat arrays, pools, custom hash) are measured against.
#pragma once
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <unordered_map>

#include "mdp3.hpp"

namespace tickscope {

struct BookStats {
    uint64_t n_new = 0, n_change = 0, n_delete = 0;
    uint64_t new_existing = 0, change_unknown = 0, delete_unknown = 0;
};

class BaselineBook {
public:
    struct Level {
        int64_t qty = 0;
        int32_t count = 0;
        std::list<uint64_t> queue;            // order ids in arrival order
    };
    using BidMap = std::map<int64_t, Level, std::greater<int64_t>>;
    using AskMap = std::map<int64_t, Level>;

    struct Security {
        BidMap bids;
        AskMap asks;
    };

    struct Order {
        int32_t security_id;
        uint8_t side;
        int64_t price;
        int32_t qty;
        uint64_t priority;
        std::list<uint64_t>::iterator pos;    // position in its level's queue
    };

    void apply(const OrderEvent& e) {
        auto it = orders_.find(e.order_id);
        switch (e.action) {
        case NEW:
            if (it != orders_.end()) {         // re-add of a live id: treat as replace
                remove_from_level(it->second, e.order_id);
                orders_.erase(it);
                ++stats_.new_existing;
            }
            insert(e);
            ++stats_.n_new;
            break;
        case CHANGE:
            if (it == orders_.end()) {         // order resting before the capture started
                insert(e);
                ++stats_.change_unknown;
                return;
            }
            {
                Order& o = it->second;
                remove_from_level(o, e.order_id);
                o.side = e.side;               // security stays the order's own
                o.price = e.price;
                o.qty = e.qty;
                if (e.priority != UINT64_NULL) o.priority = e.priority;
                add_to_level(o, e.order_id);
            }
            ++stats_.n_change;
            break;
        case DELETE:
            if (it == orders_.end()) { ++stats_.delete_unknown; return; }
            remove_from_level(it->second, e.order_id);
            orders_.erase(it);
            ++stats_.n_delete;
            break;
        default:
            break;
        }
    }

    // Aggregate at one price; nullptr if the level does not exist.
    const Level* level(int32_t sid, uint8_t side, int64_t px) const {
        auto s = secs_.find(sid);
        if (s == secs_.end()) return nullptr;
        if (side == BID) {
            auto l = s->second.bids.find(px);
            return l == s->second.bids.end() ? nullptr : &l->second;
        }
        auto l = s->second.asks.find(px);
        return l == s->second.asks.end() ? nullptr : &l->second;
    }

    // Best price and its quantity; false if that side is empty.
    bool best(int32_t sid, uint8_t side, int64_t& px, int64_t& qty) const {
        auto s = secs_.find(sid);
        if (s == secs_.end()) return false;
        if (side == BID) {
            if (s->second.bids.empty()) return false;
            px = s->second.bids.begin()->first;
            qty = s->second.bids.begin()->second.qty;
        } else {
            if (s->second.asks.empty()) return false;
            px = s->second.asks.begin()->first;
            qty = s->second.asks.begin()->second.qty;
        }
        return true;
    }

    size_t resting_orders() const { return orders_.size(); }
    const BookStats& stats() const { return stats_; }

private:
    void insert(const OrderEvent& e) {
        Order& o = orders_[e.order_id];
        o = Order{e.security_id, e.side, e.price, e.qty, e.priority, {}};
        add_to_level(o, e.order_id);
    }

    template <class M>
    static void add(M& book, Order& o, uint64_t oid) {
        Level& l = book[o.price];
        l.qty += o.qty;
        l.count += 1;
        o.pos = l.queue.insert(l.queue.end(), oid);
    }

    template <class M>
    static void remove(M& book, Order& o) {
        auto l = book.find(o.price);
        if (l == book.end()) return;
        l->second.qty -= o.qty;
        l->second.count -= 1;
        l->second.queue.erase(o.pos);
        if (l->second.count <= 0) book.erase(l);
    }

    void add_to_level(Order& o, uint64_t oid) {
        Security& s = secs_[o.security_id];
        if (o.side == BID) add(s.bids, o, oid);
        else add(s.asks, o, oid);
    }

    void remove_from_level(Order& o, uint64_t) {
        Security& s = secs_[o.security_id];
        if (o.side == BID) remove(s.bids, o);
        else remove(s.asks, o);
    }

    std::unordered_map<uint64_t, Order> orders_;
    std::unordered_map<int32_t, Security> secs_;
    BookStats stats_;
};

}  // namespace tickscope
