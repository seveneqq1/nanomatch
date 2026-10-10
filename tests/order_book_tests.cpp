#include "core/order_book.hpp"

#include <cstdio>
#include <cstdlib>

namespace {

int g_failed = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
            ++g_failed;                                                                   \
        }                                                                                 \
    } while (0)

using Book = nanomatch::OrderBook<2048, 128, 64, 2048>;

nanomatch::Command limit(uint64_t id, nanomatch::Side side, int64_t px, uint32_t qty,
                         uint32_t user = 1) {
    nanomatch::Command c{};
    c.order_id = id;
    c.price    = px;
    c.qty      = qty;
    c.user_id  = user;
    c.side     = side;
    c.type     = nanomatch::CommandType::Limit;
    return c;
}

nanomatch::Command market(uint64_t id, nanomatch::Side side, uint32_t qty,
                          uint32_t user = 1) {
    nanomatch::Command c{};
    c.order_id = id;
    c.qty      = qty;
    c.user_id  = user;
    c.side     = side;
    c.type     = nanomatch::CommandType::Market;
    return c;
}

nanomatch::Command cancel(uint64_t id) {
    nanomatch::Command c{};
    c.order_id = id;
    c.type     = nanomatch::CommandType::Cancel;
    return c;
}

void drain(Book& book) {
    nanomatch::Trade            t{};
    nanomatch::ExecutionReport  e{};
    while (book.try_pop_trade(t)) {
    }
    while (book.try_pop_exec(e)) {
    }
}

void test_full_match() {
    Book book;
    CHECK(book.process(limit(1, nanomatch::Side::Sell, 100, 10, 1)) ==
          nanomatch::RejectReason::None);
    CHECK(book.process(limit(2, nanomatch::Side::Buy, 100, 10, 2)) ==
          nanomatch::RejectReason::None);
    CHECK(book.trade_count() == 1);
    nanomatch::Trade t{};
    CHECK(book.try_pop_trade(t));
    CHECK(t.qty == 10);
    CHECK(t.price == 100);
    CHECK(t.maker_id == 1);
    CHECK(t.taker_id == 2);
    CHECK(book.find_order(1) == nullptr);
    CHECK(book.find_order(2) == nullptr);
    CHECK(book.resting() == 0);
    CHECK(!book.snapshot_bid().valid);
    CHECK(!book.snapshot_ask().valid);
    CHECK(book.account(2).position == 10);
    CHECK(book.account(1).position == -10);
    drain(book);
}

void test_partial_fill() {
    Book book;
    CHECK(book.process(limit(1, nanomatch::Side::Sell, 50, 10)) ==
          nanomatch::RejectReason::None);
    CHECK(book.process(limit(2, nanomatch::Side::Buy, 50, 6, 2)) ==
          nanomatch::RejectReason::None);
    CHECK(book.trade_count() == 1);
    nanomatch::Trade t{};
    CHECK(book.try_pop_trade(t));
    CHECK(t.qty == 6);
    CHECK(t.price == 50);
    const auto* rest = book.find_order(1);
    CHECK(rest != nullptr);
    CHECK(rest->remaining == 4);
    CHECK(book.find_order(2) == nullptr);
    CHECK(book.snapshot_ask().qty == 4);
    CHECK(book.resting() == 1);
    drain(book);
}

void test_price_time_priority() {
    Book book;
    CHECK(book.process(limit(1, nanomatch::Side::Sell, 500, 10, 1)) ==
          nanomatch::RejectReason::None);
    CHECK(book.process(limit(2, nanomatch::Side::Sell, 500, 10, 1)) ==
          nanomatch::RejectReason::None);
    CHECK(book.process(limit(3, nanomatch::Side::Buy, 500, 10, 2)) ==
          nanomatch::RejectReason::None);
    nanomatch::Trade t{};
    CHECK(book.try_pop_trade(t));
    CHECK(t.maker_id == 1);
    CHECK(t.qty == 10);
    CHECK(book.find_order(1) == nullptr);
    const auto* second = book.find_order(2);
    CHECK(second != nullptr);
    CHECK(second->remaining == 10);
    CHECK(book.snapshot_ask().count == 1);
    drain(book);
}

void test_price_priority_across_levels() {
    Book book;
    CHECK(book.process(limit(1, nanomatch::Side::Sell, 102, 5)) ==
          nanomatch::RejectReason::None);
    CHECK(book.process(limit(2, nanomatch::Side::Sell, 100, 5)) ==
          nanomatch::RejectReason::None);
    CHECK(book.process(limit(3, nanomatch::Side::Sell, 101, 5)) ==
          nanomatch::RejectReason::None);
    CHECK(book.process(limit(4, nanomatch::Side::Buy, 102, 8, 2)) ==
          nanomatch::RejectReason::None);

    nanomatch::Trade t1{}, t2{};
    CHECK(book.try_pop_trade(t1));
    CHECK(book.try_pop_trade(t2));
    CHECK(t1.price == 100 && t1.qty == 5 && t1.maker_id == 2);
    CHECK(t2.price == 101 && t2.qty == 3 && t2.maker_id == 3);
    CHECK(book.find_order(2) == nullptr);
    CHECK(book.find_order(3)->remaining == 2);
    CHECK(book.find_order(1)->remaining == 5);
    CHECK(book.snapshot_ask().price == 101);
    drain(book);
}

void test_market_walks_book() {
    Book book;
    CHECK(book.process(limit(1, nanomatch::Side::Sell, 100, 5)) ==
          nanomatch::RejectReason::None);
    CHECK(book.process(limit(2, nanomatch::Side::Sell, 101, 5)) ==
          nanomatch::RejectReason::None);
    CHECK(book.process(limit(3, nanomatch::Side::Sell, 102, 5)) ==
          nanomatch::RejectReason::None);
    CHECK(book.process(market(4, nanomatch::Side::Buy, 12, 2)) ==
          nanomatch::RejectReason::None);

    CHECK(book.trade_count() == 3);
    nanomatch::Trade t{};
    CHECK(book.try_pop_trade(t) && t.price == 100 && t.qty == 5);
    CHECK(book.try_pop_trade(t) && t.price == 101 && t.qty == 5);
    CHECK(book.try_pop_trade(t) && t.price == 102 && t.qty == 2);
    CHECK(book.find_order(4) == nullptr);
    CHECK(book.find_order(3)->remaining == 3);
    CHECK(book.snapshot_ask().price == 102);
    drain(book);
}

void test_market_empty_book() {
    Book book;
    CHECK(book.process(market(1, nanomatch::Side::Buy, 10)) ==
          nanomatch::RejectReason::NoLiquidity);
    CHECK(book.trade_count() == 0);
    CHECK(book.resting() == 0);
    drain(book);
}

void test_cancel_removes_and_frees_slot() {
    Book book;
    CHECK(book.process(limit(1, nanomatch::Side::Sell, 100, 10)) ==
          nanomatch::RejectReason::None);
    CHECK(book.resting() == 1);
    CHECK(book.process(cancel(1)) == nanomatch::RejectReason::None);
    CHECK(book.find_order(1) == nullptr);
    CHECK(book.resting() == 0);
    CHECK(!book.snapshot_ask().valid);

    CHECK(book.process(limit(2, nanomatch::Side::Buy, 100, 10, 2)) ==
          nanomatch::RejectReason::None);
    CHECK(book.trade_count() == 0);
    CHECK(book.find_order(2)->remaining == 10);
    CHECK(book.snapshot_bid().price == 100);

    CHECK(book.process(cancel(99)) == nanomatch::RejectReason::NotFound);
    drain(book);
}

void test_no_cross_after_resting() {
    Book book;
    CHECK(book.process(limit(1, nanomatch::Side::Buy, 99, 10, 1)) ==
          nanomatch::RejectReason::None);
    CHECK(book.process(limit(2, nanomatch::Side::Sell, 100, 10, 2)) ==
          nanomatch::RejectReason::None);
    CHECK(book.trade_count() == 0);
    CHECK(book.snapshot_bid().price == 99);
    CHECK(book.snapshot_ask().price == 100);
    drain(book);
}

void test_duplicate_and_bad_args() {
    Book book;
    CHECK(book.process(limit(1, nanomatch::Side::Buy, 10, 1)) ==
          nanomatch::RejectReason::None);
    CHECK(book.process(limit(1, nanomatch::Side::Buy, 10, 1)) ==
          nanomatch::RejectReason::DuplicateId);
    CHECK(book.process(limit(2, nanomatch::Side::Buy, 10, 0)) ==
          nanomatch::RejectReason::InvalidArg);
    CHECK(book.process(limit(3, nanomatch::Side::Buy, -1, 1)) ==
          nanomatch::RejectReason::InvalidArg);
    CHECK(book.process(limit(4, nanomatch::Side::Buy, 10, 1, 999)) ==
          nanomatch::RejectReason::AccountInvalid);
    drain(book);
}

void test_depth_and_fifo_append() {
    Book book;
    CHECK(book.process(limit(1, nanomatch::Side::Buy, 10, 1, 1)) ==
          nanomatch::RejectReason::None);
    CHECK(book.process(limit(2, nanomatch::Side::Buy, 11, 2, 1)) ==
          nanomatch::RejectReason::None);
    CHECK(book.process(limit(3, nanomatch::Side::Buy, 10, 4, 1)) ==
          nanomatch::RejectReason::None);
    nanomatch::DepthLevel d[4]{};
    const uint32_t        n = book.depth(nanomatch::Side::Buy, d, 4);
    CHECK(n == 2);
    CHECK(d[0].price == 11 && d[0].qty == 2 && d[0].count == 1);
    CHECK(d[1].price == 10 && d[1].qty == 5 && d[1].count == 2);

    CHECK(book.process(limit(4, nanomatch::Side::Sell, 10, 1, 2)) ==
          nanomatch::RejectReason::None);
    nanomatch::Trade t{};
    CHECK(book.try_pop_trade(t));
    CHECK(t.maker_id == 2);  // best price 11, not the older 10s
    CHECK(t.price == 11);
    drain(book);
}

void test_sell_takes_bids() {
    Book book;
    CHECK(book.process(limit(1, nanomatch::Side::Buy, 50, 3, 1)) ==
          nanomatch::RejectReason::None);
    CHECK(book.process(limit(2, nanomatch::Side::Buy, 51, 3, 1)) ==
          nanomatch::RejectReason::None);
    CHECK(book.process(limit(3, nanomatch::Side::Sell, 50, 4, 2)) ==
          nanomatch::RejectReason::None);
    nanomatch::Trade t{};
    CHECK(book.try_pop_trade(t) && t.price == 51 && t.qty == 3);
    CHECK(book.try_pop_trade(t) && t.price == 50 && t.qty == 1);
    CHECK(book.find_order(1)->remaining == 2);
    drain(book);
}

void test_lock_free_ingress() {
    Book book;
    CHECK(book.submit(limit(1, nanomatch::Side::Sell, 7, 2)));
    CHECK(book.submit(limit(2, nanomatch::Side::Buy, 7, 2, 2)));
    CHECK(book.drain() == 2);
    CHECK(book.trade_count() == 1);
    drain(book);
}

void test_cancel_middle_of_fifo() {
    Book book;
    CHECK(book.process(limit(1, nanomatch::Side::Sell, 9, 1)) ==
          nanomatch::RejectReason::None);
    CHECK(book.process(limit(2, nanomatch::Side::Sell, 9, 1)) ==
          nanomatch::RejectReason::None);
    CHECK(book.process(limit(3, nanomatch::Side::Sell, 9, 1)) ==
          nanomatch::RejectReason::None);
    CHECK(book.process(cancel(2)) == nanomatch::RejectReason::None);
    CHECK(book.process(limit(4, nanomatch::Side::Buy, 9, 2, 2)) ==
          nanomatch::RejectReason::None);
    nanomatch::Trade t{};
    CHECK(book.try_pop_trade(t) && t.maker_id == 1);
    CHECK(book.try_pop_trade(t) && t.maker_id == 3);
    CHECK(book.resting() == 0);
    drain(book);
}

}  // namespace

int main() {
    test_full_match();
    test_partial_fill();
    test_price_time_priority();
    test_price_priority_across_levels();
    test_market_walks_book();
    test_market_empty_book();
    test_cancel_removes_and_frees_slot();
    test_no_cross_after_resting();
    test_duplicate_and_bad_args();
    test_depth_and_fifo_append();
    test_sell_takes_bids();
    test_lock_free_ingress();
    test_cancel_middle_of_fifo();

    if (g_failed) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("order_book_tests: all passed\n");
    return 0;
}
