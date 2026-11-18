// CME's own market-by-price top-10 book, rebuilt from template 46 price-level entries.
// Used only to validate the L3 book (it is not on the timed path).
#pragma once
#include <array>
#include <cstdint>
#include <unordered_map>

#include "mdp3.hpp"

namespace tickscope {

class MBP10 {
public:
    struct Slot {
        bool used = false;
        int64_t price = 0;
        int32_t qty = 0;
        int32_t orders = 0;
    };
    using Side = std::array<Slot, 10>;

    void apply(const LevelEvent& e) {
        Side& b = book_[key(e.security_id, e.side)];
        const int i = int(e.level) - 1;
        if (i < 0 || i >= 10) return;
        const Slot s{true, e.price, e.qty, e.orders};
        switch (e.action) {
        case NEW:
            for (int j = 9; j > i; --j) b[j] = b[j - 1];
            b[i] = s;
            break;
        case CHANGE:
        case OVERLAY:
            b[i] = s;
            break;
        case DELETE:
            for (int j = i; j < 9; ++j) b[j] = b[j + 1];
            b[9] = Slot{};
            break;
        case DELETE_THRU:
            b.fill(Slot{});
            break;
        case DELETE_FROM:
            for (int j = i; j < 10; ++j) b[j] = Slot{};
            break;
        default:
            break;
        }
    }

    const Side& side(int32_t sid, uint8_t side) { return book_[key(sid, side)]; }
    static uint64_t key(int32_t sid, uint8_t side) { return (uint64_t(uint32_t(sid)) << 1) | side; }

private:
    std::unordered_map<uint64_t, Side> book_;
};

}  // namespace tickscope
