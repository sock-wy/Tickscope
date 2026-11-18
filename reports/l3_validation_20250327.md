# L3 order book validation — ES, 2025-03-27 08:30–09:00 CT

Code: `2_ie421_processing/book_py/l3book.py` (Python reference implementation).
Input: `data.nosync/raw/20250327/ch310_133000/134000/135000.slim.zst` (13:30–14:00 UTC, the RTH open).
Instrument checked: SecurityID 4916 (ESM5, front month). Details: `l3_20250327T1330/validation.json`.

## Method
- **L3 book**: every resting order (OrderID → side, price, qty, priority), built from the order entries
  of template 46 (orders in the top 10 levels, price/side via `ReferenceID`) and template 47 (deeper orders).
- **Reference**: CME's own MBP-10 book, rebuilt from the price-level entries of template 46.
- At the end of every matching event (MatchEventIndicator bit 7) each known MBP level is compared with
  the L3 aggregate at the same price: quantity **and** order count must both be equal.

## Results
| Metric | Value |
|---|---|
| Sequence gaps | 0 |
| Events compared | 1,479,613 |
| Price levels compared | 12,945,176 |
| Exact match (qty and order count) | **88.1%** overall |
| Best level (L1) exact match | **99.1%** |
| L3 shows *more* quantity than CME | **0 times** |
| Order events applied | 771k new, 161k change, 768k delete |
| Change/delete of an order never seen | 1,741 / 4,546 (orders resting before 08:30) |
| Runtime | 34 s for 30 min of channel 310 |

Match by depth: L1 99.1 · L2 96.5 · L3 93.8 · L4 91.0 · L5 88.5 · L6 86.2 · L7 84.1 · L8 82.1 · L9 80.3 · L10 78.7 (%).

## Reading the numbers
- **Every mismatch is missing quantity, never extra quantity.** The capture has no snapshot, so orders
  resting before 08:30 are invisible until they are changed or cancelled. When price moves into a range the
  book has not visited since 08:30, those levels hold unseen orders and the L3 book shows less than CME.
- **13:44–13:53 UTC: 100.0% at every level** (≈4.7M level comparisons). Once the visible range consists only of
  orders placed during the capture, the L3 book reproduces CME's MBP-10 exactly. This is the evidence the
  order-book logic is correct.
- The rate falls again at 13:54 when ES moves to new prices, then recovers.

## Implications for the project
1. The L3 logic is validated; the C++ engine can use this Python output as its reference (same events,
   same book state, compare per event).
2. For latency benchmarks, start measurement after a warm-up period, or seed the book by replaying the
   prior session, so the book is complete.
3. Template 48 (trades) is not needed to maintain the book: fills arrive as 46/47 change/delete events.
