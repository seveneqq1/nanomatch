#include "core/order_book.hpp"

#include <cstdio>

namespace {

void print_side(const char* name, const nanomatch::DepthLevel* lvl, uint32_t n) {
    std::printf("  %s (%u levels)\n", name, n);
    for (uint32_t i = 0; i < n; ++i) {
        std::printf("    %lld  qty=%llu  n=%u\n",
                    static_cast<long long>(lvl[i].price),
                    static_cast<unsigned long long>(lvl[i].qty), lvl[i].count);
    }
}

const char* exec_name(nanomatch::ExecType e) {
    switch (e) {
        case nanomatch::ExecType::Ack:
            return "ACK";
        case nanomatch::ExecType::PartialFill:
            return "PARTIAL";
        case nanomatch::ExecType::Fill:
            return "FILL";
        case nanomatch::ExecType::Cancel:
            return "CANCEL";
        case nanomatch::ExecType::Reject:
            return "REJECT";
        case nanomatch::ExecType::Trade:
            return "TRADE";
    }
    return "?";
}

}  // namespace

int main() {
    using Book = nanomatch::OrderBook<1u << 16, 1u << 12, 1u << 10, 1u << 14>;
    Book book;

    std::printf("nanomatch demo  orders=%u  levels=%u  accounts=%u\n",
                Book::max_orders(), Book::max_levels(), Book::max_accounts());

    const nanomatch::Command seed[] = {
        {1, 100, 10, 1, nanomatch::Side::Sell, nanomatch::CommandType::Limit},
        {2, 101, 15, 1, nanomatch::Side::Sell, nanomatch::CommandType::Limit},
        {3,  99,  8, 2, nanomatch::Side::Buy,  nanomatch::CommandType::Limit},
        {4,  98, 12, 2, nanomatch::Side::Buy,  nanomatch::CommandType::Limit},
        {5, 100,  6, 3, nanomatch::Side::Buy,  nanomatch::CommandType::Limit},  // crosses 6 @ 100
        {6,   0, 20, 3, nanomatch::Side::Buy,  nanomatch::CommandType::Market}, // walks the ask
        {3,   0,  0, 2, nanomatch::Side::Buy, nanomatch::CommandType::Cancel},
    };

    for (const auto& cmd : seed) {
        const auto why = book.process(cmd);
        if (why != nanomatch::RejectReason::None && cmd.type != nanomatch::CommandType::Cancel) {
            std::printf("reject id=%llu reason=%d\n",
                        static_cast<unsigned long long>(cmd.order_id),
                        static_cast<int>(why));
        }
    }

    std::printf("\ntrades (%llu)\n", static_cast<unsigned long long>(book.trade_count()));
    nanomatch::Trade tr{};
    while (book.try_pop_trade(tr)) {
        const char* side = tr.taker_side == nanomatch::Side::Buy ? "buy" : "sell";
        std::printf("  trade#%llu  %s %u @ %lld  maker=%llu taker=%llu\n",
                    static_cast<unsigned long long>(tr.trade_id), side, tr.qty,
                    static_cast<long long>(tr.price),
                    static_cast<unsigned long long>(tr.maker_id),
                    static_cast<unsigned long long>(tr.taker_id));
    }

    std::printf("\nreports\n");
    nanomatch::ExecutionReport er{};
    while (book.try_pop_exec(er)) {
        std::printf("  id=%llu  %s  qty=%u  rem=%u  px=%lld\n",
                    static_cast<unsigned long long>(er.order_id), exec_name(er.exec),
                    er.qty, er.remaining, static_cast<long long>(er.price));
    }

    nanomatch::DepthLevel bids[8]{};
    nanomatch::DepthLevel asks[8]{};
    const uint32_t nb = book.depth(nanomatch::Side::Buy, bids, 8);
    const uint32_t na = book.depth(nanomatch::Side::Sell, asks, 8);
    std::printf("\nbook  resting=%u\n", book.resting());
    print_side("bids", bids, nb);
    print_side("asks", asks, na);

    const auto bb = book.snapshot_bid();
    const auto ba = book.snapshot_ask();
    std::printf("\nTOB  bid=%s%lld  ask=%s%lld  spread=%lld\n",
                bb.valid ? "" : "none ",
                bb.valid ? static_cast<long long>(bb.price) : 0,
                ba.valid ? "" : "none ",
                ba.valid ? static_cast<long long>(ba.price) : 0,
                (bb.valid && ba.valid) ? static_cast<long long>(ba.price - bb.price) : 0);
    std::printf("account 3 position=%lld buy_notional=%llu\n",
                static_cast<long long>(book.account(3).position),
                static_cast<unsigned long long>(book.account(3).buy_notional));
    return 0;
}
