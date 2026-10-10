#pragma once

#include "memory_pool.hpp"
#include "types.hpp"

#include <cstdint>
#include <limits>

#if defined(__GNUC__) || defined(__clang__)
#define NM_ALWAYS_INLINE inline __attribute__((always_inline))
#define NM_LIKELY(x)     __builtin_expect(!!(x), 1)
#define NM_UNLIKELY(x)   __builtin_expect(!!(x), 0)
#define NM_PREFETCH(p)   __builtin_prefetch((p), 0, 3)
#else
#define NM_ALWAYS_INLINE inline
#define NM_LIKELY(x)     (x)
#define NM_UNLIKELY(x)   (x)
#define NM_PREFETCH(p)   ((void)0)
#endif

namespace nanomatch {

[[nodiscard]] constexpr uint32_t next_pow2_u32(uint32_t n) noexcept {
    if (n <= 2) {
        return 2;
    }
    --n;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    return n + 1;
}

// Single-threaded matcher with lock-free ingress/egress. All storage is claimed
// in the constructor; process() / try_push() never call new or malloc.
template <uint32_t MaxOrders   = (1u << 20),
          uint32_t MaxLevels   = (1u << 14),
          uint32_t MaxAccounts = (1u << 16),
          uint32_t RingSize    = (1u << 16)>
class alignas(kCacheLine) OrderBook {
    static_assert(MaxOrders >= 2, "need at least two order slots");
    static_assert(MaxLevels >= 2, "need at least two price levels");
    static_assert(MaxAccounts >= 1);
    static_assert(RingSize >= 2 && (RingSize & (RingSize - 1)) == 0);

    static constexpr uint32_t kIdCap    = next_pow2_u32(MaxOrders * 2u);
    static constexpr uint32_t kPriceCap = next_pow2_u32(MaxLevels * 2u);

public:
    using Inbound  = MpscRing<Command, RingSize>;
    using TradeOut = SpscRing<Trade, RingSize>;
    using ExecOut  = SpscRing<ExecutionReport, RingSize>;

    OrderBook() {
        accounts_ = alloc_aligned_array<Account>(MaxAccounts);
        for (uint32_t i = 0; i < MaxAccounts; ++i) {
            ::new (static_cast<void*>(accounts_ + i)) Account{};
            accounts_[i].id = i;
        }
    }

    ~OrderBook() {
        if (accounts_) {
            for (uint32_t i = 0; i < MaxAccounts; ++i) {
                accounts_[i].~Account();
            }
        }
        free_aligned_array(accounts_, MaxAccounts);
        accounts_ = nullptr;
    }

    OrderBook(const OrderBook&)            = delete;
    OrderBook& operator=(const OrderBook&) = delete;

    // --- lock-free ingress (any producer thread) --------------------------------
    [[nodiscard]] bool submit(const Command& cmd) noexcept { return inbound_.try_push(cmd); }

    // Matching thread: drain the inbound ring.
    uint32_t drain() noexcept {
        uint32_t n = 0;
        Command  cmd{};
        while (inbound_.try_pop(cmd)) {
            process(cmd);
            ++n;
        }
        return n;
    }

    [[nodiscard]] bool try_pop_trade(Trade& t) noexcept { return trades_out_.try_pop(t); }
    [[nodiscard]] bool try_pop_exec(ExecutionReport& r) noexcept { return execs_out_.try_pop(r); }

    struct Top {
        bool     valid{false};
        int64_t  price{0};
        uint64_t qty{0};
        uint32_t count{0};
    };

    // --- hot path: price-time matching, zero heap -------------------------------
    NM_ALWAYS_INLINE RejectReason process(const Command& cmd) noexcept {
        ++cmds_;
        if (NM_UNLIKELY(cmd.type == CommandType::Cancel)) {
            return cancel(cmd.order_id);
        }
        return accept_order(cmd);
    }

    [[nodiscard]] Top snapshot_bid() const noexcept { return top(bid_head_); }
    [[nodiscard]] Top snapshot_ask() const noexcept { return top(ask_head_); }

    uint32_t depth(Side side, DepthLevel* out, uint32_t max) const noexcept {
        uint32_t n   = 0;
        uint32_t idx = side == Side::Buy ? bid_head_ : ask_head_;
        while (idx != kInvalidIndex && n < max) {
            const PriceLevel& lvl = levels_[idx];
            out[n].price          = lvl.price;
            out[n].qty            = lvl.total_qty;
            out[n].count          = lvl.count;
            ++n;
            idx = lvl.next;
        }
        return n;
    }

    [[nodiscard]] const Order* find_order(uint64_t id) const noexcept {
        const uint32_t idx = ids_.find(id);
        if (idx == kInvalidIndex) {
            return nullptr;
        }
        return &orders_[idx];
    }

    [[nodiscard]] const Account& account(uint32_t user) const noexcept { return accounts_[user]; }
    [[nodiscard]] Account&       account(uint32_t user) noexcept { return accounts_[user]; }

    [[nodiscard]] uint64_t cmds_processed() const noexcept { return cmds_; }
    [[nodiscard]] uint64_t trade_count() const noexcept { return trades_; }
    [[nodiscard]] uint64_t reject_count() const noexcept { return rejects_; }
    [[nodiscard]] uint32_t resting() const noexcept { return resting_; }
    [[nodiscard]] uint32_t dropped_reports() const noexcept { return dropped_; }

    [[nodiscard]] static constexpr uint32_t max_orders() noexcept { return MaxOrders; }
    [[nodiscard]] static constexpr uint32_t max_levels() noexcept { return MaxLevels; }
    [[nodiscard]] static constexpr uint32_t max_accounts() noexcept { return MaxAccounts; }

private:
    NM_ALWAYS_INLINE RejectReason accept_order(const Command& cmd) noexcept {
        if (NM_UNLIKELY(cmd.qty == 0 || cmd.order_id == kInvalidId)) {
            return reject(cmd, RejectReason::InvalidArg);
        }
        if (NM_UNLIKELY(cmd.user_id >= MaxAccounts)) {
            return reject(cmd, RejectReason::AccountInvalid);
        }
        if (NM_UNLIKELY(cmd.type == CommandType::Limit && cmd.price < 0)) {
            return reject(cmd, RejectReason::InvalidArg);
        }
        if (NM_UNLIKELY(ids_.find(cmd.order_id) != kInvalidIndex)) {
            return reject(cmd, RejectReason::DuplicateId);
        }

        const uint32_t idx = orders_.acquire();
        if (NM_UNLIKELY(idx == kInvalidIndex)) {
            return reject(cmd, RejectReason::BookFull);
        }

        Order& o     = orders_[idx];
        o            = Order{};
        o.id         = cmd.order_id;
        o.quantity   = cmd.qty;
        o.remaining  = cmd.qty;
        o.user_id    = cmd.user_id;
        o.side       = cmd.side;
        o.status     = OrderStatus::Live;
        o.level_idx  = kInvalidIndex;
        o.next       = kInvalidIndex;
        o.prev       = kInvalidIndex;

        if (cmd.type == CommandType::Market) {
            o.type  = OrderType::Market;
            o.price = cmd.side == Side::Buy ? std::numeric_limits<int64_t>::max()
                                            : std::numeric_limits<int64_t>::min();
        } else {
            o.type  = OrderType::Limit;
            o.price = cmd.price;
        }

        if (NM_UNLIKELY(!ids_.insert(cmd.order_id, idx))) {
            orders_.release(idx);
            return reject(cmd, RejectReason::DuplicateId);
        }

        const uint64_t trades_before = trades_;
        match(idx);

        if (o.remaining == 0) {
            o.status = OrderStatus::Filled;
            retire(idx);
            return RejectReason::None;
        }

        if (o.type == OrderType::Market) {
            emit_exec(o, ExecType::Cancel, 0, o.remaining, 0, RejectReason::NoLiquidity);
            if (trades_before == trades_) {
                ++rejects_;
            }
            o.status = OrderStatus::Canceled;
            retire(idx);
            return trades_before == trades_ ? RejectReason::NoLiquidity : RejectReason::None;
        }

        if (NM_UNLIKELY(!rest(idx))) {
            emit_exec(o, ExecType::Reject, 0, o.remaining, o.price, RejectReason::LevelFull);
            o.status = OrderStatus::Canceled;
            retire(idx);
            ++rejects_;
            return RejectReason::LevelFull;
        }

        if (trades_before == trades_) {
            emit_exec(o, ExecType::Ack, 0, o.remaining, o.price, RejectReason::None);
        }
        return RejectReason::None;
    }

    NM_ALWAYS_INLINE RejectReason cancel(uint64_t id) noexcept {
        if (NM_UNLIKELY(id == kInvalidId)) {
            ++rejects_;
            return RejectReason::InvalidArg;
        }
        const uint32_t idx = ids_.find(id);
        if (NM_UNLIKELY(idx == kInvalidIndex)) {
            ++rejects_;
            ExecutionReport r{};
            r.order_id = id;
            r.exec     = ExecType::Reject;
            r.reason   = RejectReason::NotFound;
            push_exec(r);
            return RejectReason::NotFound;
        }
        Order& o = orders_[idx];
        if (NM_UNLIKELY(o.status != OrderStatus::Live || o.level_idx == kInvalidIndex)) {
            ++rejects_;
            return RejectReason::NotLive;
        }

        const uint32_t lvl_idx = o.level_idx;
        const uint32_t user    = o.user_id;
        const uint32_t leftover = o.remaining;
        const int64_t  px      = o.price;
        PriceLevel&    lvl     = levels_[lvl_idx];
        lvl.total_qty -= leftover;
        unlink_order(idx);
        if (lvl.head == kInvalidIndex) {
            unlink_level(lvl_idx);
        }

        emit_exec(o, ExecType::Cancel, 0, leftover, px, RejectReason::None);
        --accounts_[user].live_orders;
        --resting_;
        o.status = OrderStatus::Canceled;
        retire(idx);
        return RejectReason::None;
    }

    NM_ALWAYS_INLINE void match(uint32_t taker_idx) noexcept {
        Order& taker = orders_[taker_idx];
        while (taker.remaining > 0) {
            const uint32_t best =
                taker.side == Side::Buy ? ask_head_ : bid_head_;
            if (NM_UNLIKELY(best == kInvalidIndex)) {
                break;
            }
            PriceLevel& lvl = levels_[best];
            if (taker.type == OrderType::Limit) {
                if (taker.side == Side::Buy && lvl.price > taker.price) {
                    break;
                }
                if (taker.side == Side::Sell && lvl.price < taker.price) {
                    break;
                }
            }
            match_level(taker_idx, best);
        }
    }

    NM_ALWAYS_INLINE void match_level(uint32_t taker_idx, uint32_t level_idx) noexcept {
        Order&      taker = orders_[taker_idx];
        PriceLevel& lvl   = levels_[level_idx];

        uint32_t maker_idx = lvl.head;
        while (taker.remaining > 0 && maker_idx != kInvalidIndex) {
            Order&         maker = orders_[maker_idx];
            const uint32_t nxt   = maker.next;
            if (nxt != kInvalidIndex) {
                NM_PREFETCH(&orders_[nxt]);
            }

            const uint32_t qty = taker.remaining < maker.remaining ? taker.remaining
                                                                   : maker.remaining;
            const int64_t px   = lvl.price;

            emit_trade(maker, taker, qty, px);
            apply_fill(maker, taker, qty, px);

            taker.remaining -= qty;
            maker.remaining -= qty;
            lvl.total_qty -= qty;

            if (maker.remaining == 0) {
                unlink_order(maker_idx);
                emit_exec(maker, ExecType::Fill, qty, 0, px, RejectReason::None);
                --accounts_[maker.user_id].live_orders;
                --resting_;
                maker.status = OrderStatus::Filled;
                retire(maker_idx);
            } else {
                emit_exec(maker, ExecType::PartialFill, qty, maker.remaining, px,
                          RejectReason::None);
            }

            emit_exec(taker,
                      taker.remaining == 0 ? ExecType::Fill : ExecType::PartialFill, qty,
                      taker.remaining, px, RejectReason::None);

            maker_idx = nxt;
        }

        if (lvl.head == kInvalidIndex) {
            unlink_level(level_idx);
        }
    }

    NM_ALWAYS_INLINE bool rest(uint32_t idx) noexcept {
        Order&         o       = orders_[idx];
        uint32_t       lvl_idx = price_at_.find(o.price);
        if (lvl_idx != kInvalidIndex) {
            PriceLevel& lvl = levels_[lvl_idx];
            if (NM_UNLIKELY(lvl.side != o.side)) {
                return false;
            }
            append(lvl_idx, idx);
            ++accounts_[o.user_id].live_orders;
            ++resting_;
            return true;
        }

        lvl_idx = levels_.acquire();
        if (NM_UNLIKELY(lvl_idx == kInvalidIndex)) {
            return false;
        }
        PriceLevel& lvl = levels_[lvl_idx];
        lvl             = PriceLevel{};
        lvl.price       = o.price;
        lvl.side        = o.side;
        lvl.head        = kInvalidIndex;
        lvl.tail        = kInvalidIndex;
        if (NM_UNLIKELY(!insert_level_sorted(lvl_idx))) {
            levels_.release(lvl_idx);
            return false;
        }
        if (NM_UNLIKELY(!price_at_.insert(o.price, lvl_idx))) {
            unlink_level(lvl_idx);
            return false;
        }
        append(lvl_idx, idx);
        ++accounts_[o.user_id].live_orders;
        ++resting_;
        return true;
    }

    NM_ALWAYS_INLINE bool insert_level_sorted(uint32_t idx) noexcept {
        PriceLevel& lvl  = levels_[idx];
        uint32_t*   head = lvl.side == Side::Buy ? &bid_head_ : &ask_head_;
        uint32_t    cur  = *head;
        uint32_t    prev = kInvalidIndex;

        if (lvl.side == Side::Buy) {
            while (cur != kInvalidIndex && levels_[cur].price > lvl.price) {
                prev = cur;
                cur  = levels_[cur].next;
            }
        } else {
            while (cur != kInvalidIndex && levels_[cur].price < lvl.price) {
                prev = cur;
                cur  = levels_[cur].next;
            }
        }

        lvl.prev = prev;
        lvl.next = cur;
        if (prev != kInvalidIndex) {
            levels_[prev].next = idx;
        } else {
            *head = idx;
        }
        if (cur != kInvalidIndex) {
            levels_[cur].prev = idx;
        }
        return true;
    }

    NM_ALWAYS_INLINE void append(uint32_t lvl_idx, uint32_t ord_idx) noexcept {
        PriceLevel& lvl = levels_[lvl_idx];
        Order&      o   = orders_[ord_idx];
        o.level_idx     = lvl_idx;
        o.prev          = lvl.tail;
        o.next          = kInvalidIndex;
        if (lvl.tail != kInvalidIndex) {
            orders_[lvl.tail].next = ord_idx;
        } else {
            lvl.head = ord_idx;
        }
        lvl.tail = ord_idx;
        ++lvl.count;
        lvl.total_qty += o.remaining;
        o.status = OrderStatus::Live;
    }

    NM_ALWAYS_INLINE void unlink_order(uint32_t idx) noexcept {
        Order&      o   = orders_[idx];
        const uint32_t lvl_idx = o.level_idx;
        if (lvl_idx == kInvalidIndex) {
            return;
        }
        PriceLevel& lvl = levels_[lvl_idx];
        if (o.prev != kInvalidIndex) {
            orders_[o.prev].next = o.next;
        } else {
            lvl.head = o.next;
        }
        if (o.next != kInvalidIndex) {
            orders_[o.next].prev = o.prev;
        } else {
            lvl.tail = o.prev;
        }
        if (lvl.count > 0) {
            --lvl.count;
        }
        o.next      = kInvalidIndex;
        o.prev      = kInvalidIndex;
        o.level_idx = kInvalidIndex;
    }

    NM_ALWAYS_INLINE void unlink_level(uint32_t idx) noexcept {
        PriceLevel& lvl  = levels_[idx];
        uint32_t*   head = lvl.side == Side::Buy ? &bid_head_ : &ask_head_;
        if (lvl.prev != kInvalidIndex) {
            levels_[lvl.prev].next = lvl.next;
        } else {
            *head = lvl.next;
        }
        if (lvl.next != kInvalidIndex) {
            levels_[lvl.next].prev = lvl.prev;
        }
        price_at_.erase(lvl.price);
        lvl = PriceLevel{};
        levels_.release(idx);
    }

    NM_ALWAYS_INLINE void retire(uint32_t idx) noexcept {
        const uint64_t id = orders_[idx].id;
        ids_.erase(id);
        orders_[idx] = Order{};
        orders_.release(idx);
    }

    NM_ALWAYS_INLINE void apply_fill(Order& maker, Order& taker, uint32_t qty,
                                     int64_t px) noexcept {
        const uint64_t notion =
            static_cast<uint64_t>(qty) * static_cast<uint64_t>(px < 0 ? 0 : px);
        Account& mt = accounts_[taker.user_id];
        Account& mm = accounts_[maker.user_id];
        if (taker.side == Side::Buy) {
            mt.position += static_cast<int64_t>(qty);
            mt.buy_notional += notion;
            mm.position -= static_cast<int64_t>(qty);
            mm.sell_notional += notion;
        } else {
            mt.position -= static_cast<int64_t>(qty);
            mt.sell_notional += notion;
            mm.position += static_cast<int64_t>(qty);
            mm.buy_notional += notion;
        }
    }

    NM_ALWAYS_INLINE void emit_trade(const Order& maker, const Order& taker, uint32_t qty,
                                     int64_t px) noexcept {
        ++trades_;
        Trade t{};
        t.trade_id    = ++next_trade_id_;
        t.maker_id    = maker.id;
        t.taker_id    = taker.id;
        t.maker_user  = maker.user_id;
        t.taker_user  = taker.user_id;
        t.price       = px;
        t.qty         = qty;
        t.taker_side  = taker.side;
        if (NM_UNLIKELY(!trades_out_.try_push(t))) {
            ++dropped_;
        }
    }

    NM_ALWAYS_INLINE void emit_exec(const Order& o, ExecType exec, uint32_t qty,
                                    uint32_t remaining, int64_t px,
                                    RejectReason why) noexcept {
        ExecutionReport r{};
        r.order_id  = o.id;
        r.trade_id  = exec == ExecType::Fill || exec == ExecType::PartialFill ||
                             exec == ExecType::Trade
                         ? next_trade_id_
                         : kInvalidId;
        r.price     = px;
        r.qty       = qty;
        r.remaining = remaining;
        r.exec      = exec;
        r.reason    = why;
        r.side      = o.side;
        push_exec(r);
    }

    NM_ALWAYS_INLINE void push_exec(const ExecutionReport& r) noexcept {
        if (NM_UNLIKELY(!execs_out_.try_push(r))) {
            ++dropped_;
        }
    }

    NM_ALWAYS_INLINE RejectReason reject(const Command& cmd, RejectReason why) noexcept {
        ++rejects_;
        ExecutionReport r{};
        r.order_id = cmd.order_id;
        r.exec     = ExecType::Reject;
        r.reason   = why;
        r.side     = cmd.side;
        r.price    = cmd.price;
        r.qty      = cmd.qty;
        push_exec(r);
        return why;
    }

    NM_ALWAYS_INLINE Top top(uint32_t head) const noexcept {
        Top t{};
        if (head == kInvalidIndex) {
            return t;
        }
        const PriceLevel& lvl = levels_[head];
        t.valid               = true;
        t.price               = lvl.price;
        t.qty                 = lvl.total_qty;
        t.count               = lvl.count;
        return t;
    }

    SlotPool<Order, MaxOrders>         orders_{};
    SlotPool<PriceLevel, MaxLevels>    levels_{};
    IdHash<kIdCap>                     ids_{};
    PriceHash<kPriceCap>               price_at_{};
    Account*                           accounts_{nullptr};

    uint32_t bid_head_{kInvalidIndex};
    uint32_t ask_head_{kInvalidIndex};
    uint32_t resting_{0};
    uint32_t dropped_{0};
    uint64_t cmds_{0};
    uint64_t trades_{0};
    uint64_t rejects_{0};
    uint64_t next_trade_id_{0};

    Inbound  inbound_{};
    TradeOut trades_out_{};
    ExecOut  execs_out_{};
};

using DefaultBook = OrderBook<>;

}  // namespace nanomatch
