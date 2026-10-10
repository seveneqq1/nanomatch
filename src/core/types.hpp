#pragma once

#include <cstdint>
#include <type_traits>

namespace nanomatch {

inline constexpr uint32_t kInvalidIndex = 0xFFFFFFFFu;
inline constexpr uint64_t kInvalidId    = 0;
inline constexpr std::size_t kCacheLine = 64;

enum class Side : uint8_t { Buy = 0, Sell = 1 };

enum class OrderType : uint8_t { Limit = 0, Market = 1 };

enum class CommandType : uint8_t { Limit = 0, Market = 1, Cancel = 2 };

enum class ExecType : uint8_t {
    Ack         = 0,
    PartialFill = 1,
    Fill        = 2,
    Cancel      = 3,
    Reject      = 4,
    Trade       = 5
};

enum class RejectReason : uint8_t {
    None            = 0,
    InvalidArg      = 1,
    DuplicateId     = 2,
    BookFull        = 3,
    LevelFull       = 4,
    NotFound        = 5,
    NotLive         = 6,
    AccountInvalid  = 7,
    NoLiquidity     = 8
};

enum class OrderStatus : uint8_t {
    Free     = 0,
    Live     = 1,
    Filled   = 2,
    Canceled = 3
};

// Ingress command. Copied by value into a pre-allocated ring; never heap-allocated
// on the hot path.
struct Command {
    uint64_t    order_id{kInvalidId};
    int64_t     price{0};       // integer ticks
    uint32_t    qty{0};
    uint32_t    user_id{0};
    Side        side{Side::Buy};
    CommandType type{CommandType::Limit};
};

// One cache-line Order. `next`/`prev` are pool indices (not pointers) so the
// book remains valid under relocation and stays inside a single 64-byte line.
struct alignas(kCacheLine) Order {
    uint64_t    id{kInvalidId};
    int64_t     price{0};
    uint32_t    remaining{0};
    uint32_t    quantity{0};
    uint32_t    user_id{0};
    uint32_t    next{kInvalidIndex};
    uint32_t    prev{kInvalidIndex};
    uint32_t    level_idx{kInvalidIndex};
    Side        side{Side::Buy};
    OrderType   type{OrderType::Limit};
    OrderStatus status{OrderStatus::Free};
    uint8_t     pad_[13]{};
};

static_assert(sizeof(Order) == kCacheLine, "Order must occupy exactly one cache line");
static_assert(alignof(Order) == kCacheLine, "Order must be 64-byte aligned");
static_assert(std::is_trivially_copyable_v<Order>);

// One cache-line price level. Levels of a side form a sorted intrusive DLL
// (bids high-to-low, asks low-to-high). FIFO of orders hangs off head/tail.
struct alignas(kCacheLine) PriceLevel {
    int64_t  price{0};
    uint64_t total_qty{0};
    uint32_t head{kInvalidIndex};
    uint32_t tail{kInvalidIndex};
    uint32_t next{kInvalidIndex};
    uint32_t prev{kInvalidIndex};
    uint32_t count{0};
    Side     side{Side::Buy};
    uint8_t  pad_[27]{};
};

static_assert(sizeof(PriceLevel) == kCacheLine);
static_assert(alignof(PriceLevel) == kCacheLine);
static_assert(std::is_trivially_copyable_v<PriceLevel>);

struct alignas(kCacheLine) Account {
    uint64_t buy_notional{0};
    uint64_t sell_notional{0};
    int64_t  position{0};       // +long / -short in qty
    uint32_t live_orders{0};
    uint32_t id{0};
    uint8_t  pad_[32]{};
};

static_assert(sizeof(Account) == kCacheLine);
static_assert(alignof(Account) == kCacheLine);

struct Trade {
    uint64_t trade_id{kInvalidId};
    uint64_t maker_id{kInvalidId};
    uint64_t taker_id{kInvalidId};
    uint32_t maker_user{0};
    uint32_t taker_user{0};
    int64_t  price{0};
    uint32_t qty{0};
    Side     taker_side{Side::Buy};
};

struct ExecutionReport {
    uint64_t      order_id{kInvalidId};
    uint64_t      trade_id{kInvalidId};
    int64_t       price{0};
    uint32_t      qty{0};
    uint32_t      remaining{0};
    ExecType      exec{ExecType::Ack};
    RejectReason  reason{RejectReason::None};
    Side          side{Side::Buy};
};

struct DepthLevel {
    int64_t  price{0};
    uint64_t qty{0};
    uint32_t count{0};
};

[[nodiscard]] constexpr Side opposite(Side s) noexcept {
    return s == Side::Buy ? Side::Sell : Side::Buy;
}

}  // namespace nanomatch
