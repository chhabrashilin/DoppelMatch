// Matching semantics, one scenario at a time. Every scenario runs against all four implementations.

#include "helpers.hpp"

using namespace exsim;
using namespace exsim::test;

constexpr Side B = Side::Buy;
constexpr Side S = Side::Sell;

BOOK_TEST(rests_without_crossing) {
  Harness<Book> h;
  auto ev = h.send(new_order(1, B, 1500, 10));
  REQUIRE(ev.size() == 1);
  CHECK_ACCEPTED(ev[0], 1, B, 1500, 10);
  h.send(new_order(2, B, 1499, 5));
  h.send(new_order(3, S, 1510, 7));
  CHECK_EQ(h.book.best_bid().value(), 1500);
  CHECK_EQ(h.book.best_ask().value(), 1510);
  CHECK_EQ(h.book.best_bid_qty(), 10u);
  CHECK_EQ(h.book.best_ask_qty(), 7u);
  CHECK_EQ(h.book.order_count(), 3u);
  CHECK(h.book.check_invariants());
}

BOOK_TEST(fills_at_maker_price) {
  Harness<Book> h;
  h.send(new_order(1, S, 1505, 10));
  auto ev = h.send(new_order(2, B, 1510, 10));
  REQUIRE(ev.size() == 2);
  CHECK_ACCEPTED(ev[0], 2, B, 1510, 10);
  CHECK_TRADE(ev[1], 2, 1, 1505, 10, 0);
  CHECK(ev[1].side == B);
  CHECK_EQ(h.book.order_count(), 0u);
  CHECK(!h.book.best_bid().has_value());
  CHECK(!h.book.best_ask().has_value());
  CHECK(h.book.check_invariants());
}

BOOK_TEST(price_then_time_priority) {
  Harness<Book> h;
  h.send(new_order(1, S, 1505, 5));
  h.send(new_order(2, S, 1505, 5));
  h.send(new_order(3, S, 1504, 5));
  auto ev = h.send(new_order(4, B, 1505, 12));
  REQUIRE(ev.size() == 4);
  CHECK_TRADE(ev[1], 4, 3, 1504, 5, 0);  // better price first
  CHECK_TRADE(ev[2], 4, 1, 1505, 5, 0);  // then earlier arrival
  CHECK_TRADE(ev[3], 4, 2, 1505, 2, 3);
  CHECK_EQ(h.book.order_count(), 1u);
  CHECK_EQ(h.book.find_order(2)->qty, 3u);
  CHECK_EQ(h.book.best_ask_qty(), 3u);
  CHECK(h.book.check_invariants());
}

BOOK_TEST(sell_side_sweeps_bids_high_to_low) {
  Harness<Book> h;
  h.send(new_order(1, B, 1500, 5));
  h.send(new_order(2, B, 1502, 5));
  h.send(new_order(3, B, 1501, 5));
  auto ev = h.send(new_order(4, S, 1501, 20));
  REQUIRE(ev.size() == 3);
  CHECK_TRADE(ev[1], 4, 2, 1502, 5, 0);
  CHECK_TRADE(ev[2], 4, 3, 1501, 5, 0);
  CHECK_EQ(h.book.best_ask().value(), 1501);  // remainder rests at its limit
  CHECK_EQ(h.book.best_ask_qty(), 10u);
  CHECK_EQ(h.book.best_bid().value(), 1500);
  CHECK(h.book.check_invariants());
}

BOOK_TEST(taker_remainder_rests) {
  Harness<Book> h;
  h.send(new_order(1, S, 1505, 5));
  auto ev = h.send(new_order(2, B, 1506, 20));
  REQUIRE(ev.size() == 2);
  CHECK_TRADE(ev[1], 2, 1, 1505, 5, 0);
  CHECK_EQ(h.book.best_bid().value(), 1506);
  CHECK_EQ(h.book.best_bid_qty(), 15u);
  CHECK(!h.book.best_ask().has_value());
  CHECK(h.book.check_invariants());
}

BOOK_TEST(cancel_semantics) {
  Harness<Book> h;
  h.send(new_order(1, B, 1500, 10));
  h.send(new_order(2, B, 1500, 10));
  h.send(new_order(3, B, 1500, 10));
  auto ev = h.send(cancel(2));  // middle of the FIFO
  REQUIRE(ev.size() == 1);
  CHECK_CANCELED(ev[0], 2, 1500, 10, Reason::UserCanceled);
  CHECK(ev[0].side == B);
  CHECK_EQ(h.book.best_bid_qty(), 20u);
  ev = h.send(cancel(2));
  REQUIRE(ev.size() == 1);
  CHECK_REJECTED(ev[0], 2, Reason::UnknownOrder);
  CHECK(ev[0].request == MsgType::Cancel);
  CHECK(h.book.check_invariants());
  ev = h.send(new_order(4, S, 1500, 20));  // FIFO intact around the hole
  REQUIRE(ev.size() == 3);
  CHECK_TRADE(ev[1], 4, 1, 1500, 10, 0);
  CHECK_TRADE(ev[2], 4, 3, 1500, 10, 0);
  CHECK_EQ(h.book.order_count(), 0u);
  CHECK(h.book.check_invariants());
}

BOOK_TEST(cancel_head_and_tail) {
  Harness<Book> h;
  for (OrderId id = 1; id <= 4; ++id) h.send(new_order(id, S, 1600, 1));
  h.send(cancel(1));  // head
  h.send(cancel(4));  // tail
  CHECK(h.book.check_invariants());
  h.send(new_order(5, S, 1600, 1));  // appends after 3
  std::vector<OrderId> order;
  h.book.for_each_order(S, [&](Price, OrderId id, Qty) { order.push_back(id); });
  CHECK((order == std::vector<OrderId>{2, 3, 5}));
  h.send(cancel(2));
  h.send(cancel(3));
  h.send(cancel(5));
  CHECK(!h.book.best_ask().has_value());
  CHECK(h.book.check_invariants());
}

BOOK_TEST(best_price_recovers_across_gaps) {
  Harness<Book> h;
  h.send(new_order(1, B, 1000, 1));  // band floor
  h.send(new_order(2, B, 1900, 1));
  h.send(new_order(3, B, 1700, 1));
  h.send(new_order(4, S, 2023, 1));  // band ceiling
  h.send(new_order(5, S, 1901, 1));
  h.send(cancel(2));
  CHECK_EQ(h.book.best_bid().value(), 1700);
  h.send(cancel(3));
  CHECK_EQ(h.book.best_bid().value(), 1000);
  h.send(cancel(5));
  CHECK_EQ(h.book.best_ask().value(), 2023);
  auto ev = h.send(new_order(6, B, 2023, 1));  // lift the ceiling
  CHECK_TRADE(ev[1], 6, 4, 2023, 1, 0);
  CHECK(!h.book.best_ask().has_value());
  h.send(cancel(1));
  CHECK(!h.book.best_bid().has_value());
  CHECK(h.book.check_invariants());
}

BOOK_TEST(modify_reduce_keeps_priority) {
  Harness<Book> h;
  h.send(new_order(1, B, 1500, 10));
  h.send(new_order(2, B, 1500, 10));
  auto ev = h.send(modify(1, 1500, 5));
  REQUIRE(ev.size() == 1);
  CHECK_EQ(ev[0].type, EventType::Modified);
  CHECK_EQ(ev[0].qty, 5u);
  CHECK_EQ(h.book.best_bid_qty(), 15u);
  ev = h.send(new_order(3, S, 1500, 5));
  CHECK_TRADE(ev[1], 3, 1, 1500, 5, 0);
  CHECK(h.book.check_invariants());
}

BOOK_TEST(modify_increase_loses_priority) {
  Harness<Book> h;
  h.send(new_order(1, B, 1500, 10));
  h.send(new_order(2, B, 1500, 10));
  h.send(modify(1, 1500, 20));
  auto ev = h.send(new_order(3, S, 1500, 10));
  CHECK_TRADE(ev[1], 3, 2, 1500, 10, 0);
  CHECK_EQ(h.book.find_order(1)->qty, 20u);
  CHECK(h.book.check_invariants());
}

BOOK_TEST(modify_reprice_can_trade) {
  Harness<Book> h;
  h.send(new_order(1, B, 1500, 10));
  h.send(new_order(2, S, 1502, 5));
  auto ev = h.send(modify(1, 1502, 10));
  REQUIRE(ev.size() == 2);
  CHECK_EQ(ev[0].type, EventType::Modified);
  CHECK_EQ(ev[0].price, 1502);
  CHECK_TRADE(ev[1], 1, 2, 1502, 5, 0);
  CHECK_EQ(h.book.best_bid().value(), 1502);
  CHECK_EQ(h.book.best_bid_qty(), 5u);
  CHECK(!h.book.best_ask().has_value());
  CHECK(h.book.check_invariants());
}

BOOK_TEST(modify_rejects) {
  Harness<Book> h;
  h.send(new_order(1, B, 1500, 10));
  auto ev = h.send(modify(9, 1500, 5));
  CHECK_REJECTED(ev[0], 9, Reason::UnknownOrder);
  CHECK(ev[0].request == MsgType::Modify);
  ev = h.send(modify(1, 1500, 0));
  CHECK_REJECTED(ev[0], 1, Reason::InvalidQty);
  ev = h.send(modify(1, 5000, 5));
  CHECK_REJECTED(ev[0], 1, Reason::InvalidPrice);
  CHECK_EQ(h.book.find_order(1)->qty, 10u);
  CHECK(h.book.check_invariants());
}

BOOK_TEST(ioc_never_rests) {
  Harness<Book> h;
  h.send(new_order(1, S, 1505, 5));
  auto ev = h.send(new_order(2, B, 1505, 8, Tif::Ioc));
  REQUIRE(ev.size() == 3);
  CHECK_TRADE(ev[1], 2, 1, 1505, 5, 0);
  CHECK_CANCELED(ev[2], 2, 1505, 3, Reason::IocExpired);
  ev = h.send(new_order(3, B, 1500, 5, Tif::Ioc));
  REQUIRE(ev.size() == 2);
  CHECK_CANCELED(ev[1], 3, 1500, 5, Reason::IocExpired);
  CHECK_EQ(h.book.order_count(), 0u);
  CHECK(h.book.check_invariants());
}

BOOK_TEST(fok_is_all_or_nothing) {
  Harness<Book> h;
  h.send(new_order(1, S, 1505, 5));
  h.send(new_order(2, S, 1506, 5));
  auto ev = h.send(new_order(3, B, 1506, 11, Tif::Fok));
  REQUIRE(ev.size() == 2);
  CHECK_CANCELED(ev[1], 3, 1506, 11, Reason::FokUnfilled);
  ev = h.send(new_order(4, B, 1505, 6, Tif::Fok));  // enough size exists, but not within the limit
  CHECK_CANCELED(ev[1], 4, 1505, 6, Reason::FokUnfilled);
  CHECK_EQ(h.book.order_count(), 2u);
  ev = h.send(new_order(5, B, 1506, 10, Tif::Fok));
  REQUIRE(ev.size() == 3);
  CHECK_TRADE(ev[1], 5, 1, 1505, 5, 0);
  CHECK_TRADE(ev[2], 5, 2, 1506, 5, 0);
  CHECK_EQ(h.book.order_count(), 0u);
  CHECK(h.book.check_invariants());
}

BOOK_TEST(market_orders_sweep_then_expire) {
  Harness<Book> h;
  h.send(new_order(1, S, 1505, 5));
  h.send(new_order(2, S, 1600, 5));
  auto ev = h.send(market(3, B, 20));
  REQUIRE(ev.size() == 4);
  CHECK_ACCEPTED(ev[0], 3, B, 0, 20);
  CHECK_TRADE(ev[1], 3, 1, 1505, 5, 0);
  CHECK_TRADE(ev[2], 3, 2, 1600, 5, 0);
  CHECK_CANCELED(ev[3], 3, 0, 10, Reason::IocExpired);
  ev = h.send(market(4, S, 7));  // empty book
  REQUIRE(ev.size() == 2);
  CHECK_CANCELED(ev[1], 4, 0, 7, Reason::IocExpired);
  CHECK_EQ(h.book.order_count(), 0u);
  CHECK(h.book.check_invariants());
}

BOOK_TEST(post_only_never_takes) {
  Harness<Book> h;
  h.send(new_order(1, S, 1505, 5));
  auto ev = h.send(new_order(2, B, 1505, 5, Tif::Day, OrdType::Limit, kFlagPostOnly));
  REQUIRE(ev.size() == 1);
  CHECK_REJECTED(ev[0], 2, Reason::WouldCross);
  ev = h.send(new_order(3, B, 1504, 5, Tif::Day, OrdType::Limit, kFlagPostOnly));
  REQUIRE(ev.size() == 1);
  CHECK_EQ(ev[0].type, EventType::Accepted);
  CHECK_EQ(h.book.best_bid().value(), 1504);
  CHECK(h.book.check_invariants());
}

BOOK_TEST(validation_order_and_reasons) {
  Harness<Book> h;
  CHECK_REJECTED(h.send(new_order(0, B, 1500, 1))[0], 0, Reason::InvalidOrderId);
  CHECK_REJECTED(h.send(new_order(1, B, 1500, 0))[0], 1, Reason::InvalidQty);
  CHECK_REJECTED(h.send(new_order(1, B, 999, 1))[0], 1, Reason::InvalidPrice);
  CHECK_REJECTED(h.send(new_order(1, B, 2024, 1))[0], 1, Reason::InvalidPrice);
  CHECK_EQ(h.send(new_order(1, B, 1000, 1))[0].type, EventType::Accepted);
  CHECK_EQ(h.send(new_order(2, S, 2023, 1))[0].type, EventType::Accepted);
  CHECK_REJECTED(h.send(new_order(1, S, 1500, 1))[0], 1, Reason::DuplicateOrderId);
  h.send(cancel(1));
  CHECK_EQ(h.send(new_order(1, B, 1001, 1))[0].type, EventType::Accepted);  // dead ids may be reused
  CHECK(h.book.check_invariants());
}

BOOK_TEST(book_full_cancels_remainder) {
  Harness<Book> h;  // capacity 64
  for (OrderId id = 1; id <= 64; ++id) h.send(new_order(id, B, 1000 + static_cast<Price>(id), 1));
  CHECK_EQ(h.book.order_count(), 64u);
  auto ev = h.send(new_order(65, B, 1100, 3));
  REQUIRE(ev.size() == 2);
  CHECK_CANCELED(ev[1], 65, 1100, 3, Reason::BookFull);
  h.send(cancel(10));
  ev = h.send(new_order(66, B, 1100, 3));
  CHECK_EQ(ev.size(), 1u);
  CHECK(h.book.contains(66));
  CHECK(h.book.check_invariants());
}

BOOK_TEST(stp_cancel_resting) {
  Harness<Book> h(small_config(Stp::CancelResting));
  h.send(new_order(1, S, 1505, 5, Tif::Day, OrdType::Limit, 0, /*owner*/ 7));
  h.send(new_order(2, S, 1505, 5, Tif::Day, OrdType::Limit, 0, 8));
  auto ev = h.send(new_order(3, B, 1505, 8, Tif::Day, OrdType::Limit, 0, 7));
  REQUIRE(ev.size() == 3);
  CHECK_CANCELED(ev[1], 1, 1505, 5, Reason::SelfTrade);
  CHECK(ev[1].side == S);
  CHECK_TRADE(ev[2], 3, 2, 1505, 5, 0);
  CHECK_EQ(h.book.best_bid().value(), 1505);
  CHECK_EQ(h.book.best_bid_qty(), 3u);
  CHECK(h.book.check_invariants());
}

BOOK_TEST(stp_cancel_incoming) {
  Harness<Book> h(small_config(Stp::CancelIncoming));
  h.send(new_order(1, S, 1504, 5, Tif::Day, OrdType::Limit, 0, 8));
  h.send(new_order(2, S, 1505, 5, Tif::Day, OrdType::Limit, 0, 7));
  auto ev = h.send(new_order(3, B, 1505, 8, Tif::Day, OrdType::Limit, 0, 7));
  REQUIRE(ev.size() == 3);
  CHECK_TRADE(ev[1], 3, 1, 1504, 5, 0);
  CHECK_CANCELED(ev[2], 3, 1505, 3, Reason::SelfTrade);
  CHECK(h.book.contains(2));
  CHECK_EQ(h.book.order_count(), 1u);
  CHECK(h.book.check_invariants());
}

BOOK_TEST(stp_aware_fok) {
  Harness<Book> h(small_config(Stp::CancelResting));
  h.send(new_order(1, S, 1505, 5, Tif::Day, OrdType::Limit, 0, 7));
  h.send(new_order(2, S, 1505, 5, Tif::Day, OrdType::Limit, 0, 8));
  // 10 lots rest, but 5 are the aggressor's own, so an 8-lot FOK is not fillable.
  auto ev = h.send(new_order(3, B, 1505, 8, Tif::Fok, OrdType::Limit, 0, 7));
  REQUIRE(ev.size() == 2);
  CHECK_CANCELED(ev[1], 3, 1505, 8, Reason::FokUnfilled);
  CHECK_EQ(h.book.order_count(), 2u);  // nothing was self-trade-canceled either
  CHECK(h.book.check_invariants());
}

// Coinbase's default "decrement and cancel": the smaller order is cancelled and the larger is decremented
// by its size; equal sizes cancel both. The rules below were confirmed on real Coinbase data
// (docs/VALIDATION.md): an 11-lot incoming order meeting its own 5-lot resting order was decremented to 6
// and then traded 6 with the next resting order.
BOOK_TEST(stp_decrement_cancel_smaller_resting_order) {
  Harness<Book> h(small_config(Stp::DecrementCancel));
  h.send(new_order(1, S, 1505, 5, Tif::Day, OrdType::Limit, 0, 7));
  h.send(new_order(2, S, 1505, 5, Tif::Day, OrdType::Limit, 0, 8));
  auto ev = h.send(new_order(3, B, 1505, 8, Tif::Day, OrdType::Limit, 0, 7));
  REQUIRE(ev.size() == 4);
  CHECK_CANCELED(ev[1], 1, 1505, 5, Reason::SelfTrade);  // own smaller resting order cancelled
  CHECK_EQ(ev[2].type, EventType::Modified);              // incoming decremented 8 -> 3
  CHECK_EQ(ev[2].order_id, OrderId{3});
  CHECK_EQ(ev[2].qty, Qty{3});
  CHECK(ev[2].reason == Reason::SelfTrade);
  CHECK_TRADE(ev[3], 3, 2, 1505, 3, 2);
  CHECK_EQ(h.book.order_count(), 1u);  // only order 2's remaining 2 lots
  CHECK(h.book.check_invariants());
}

BOOK_TEST(stp_decrement_cancel_larger_resting_order) {
  Harness<Book> h(small_config(Stp::DecrementCancel));
  h.send(new_order(1, S, 1505, 10, Tif::Day, OrdType::Limit, 0, 7));
  auto ev = h.send(new_order(2, B, 1505, 4, Tif::Day, OrdType::Limit, 0, 7));
  REQUIRE(ev.size() == 3);
  CHECK_EQ(ev[1].type, EventType::Modified);  // resting decremented 10 -> 6
  CHECK_EQ(ev[1].order_id, OrderId{1});
  CHECK_EQ(ev[1].qty, Qty{6});
  CHECK_CANCELED(ev[2], 2, 1505, 4, Reason::SelfTrade);  // incoming cancelled
  CHECK_EQ(h.book.find_order(1)->qty, 6u);
  CHECK_EQ(h.book.best_ask_qty(), 6u);
  CHECK(h.book.check_invariants());
}

BOOK_TEST(stp_decrement_cancel_equal_sizes_cancel_both) {
  Harness<Book> h(small_config(Stp::DecrementCancel));
  h.send(new_order(1, S, 1505, 5, Tif::Day, OrdType::Limit, 0, 7));
  auto ev = h.send(new_order(2, B, 1505, 5, Tif::Day, OrdType::Limit, 0, 7));
  REQUIRE(ev.size() == 3);
  CHECK_CANCELED(ev[1], 1, 1505, 5, Reason::SelfTrade);
  CHECK_CANCELED(ev[2], 2, 1505, 5, Reason::SelfTrade);
  CHECK_EQ(h.book.order_count(), 0u);
  CHECK(h.book.check_invariants());
}

BOOK_TEST(stp_mode_is_chosen_per_order) {
  // The book defaults to cancel-incoming; one order asks for cancel-resting instead.
  Harness<Book> h(small_config(Stp::CancelIncoming));
  h.send(new_order(1, S, 1505, 5, Tif::Day, OrdType::Limit, 0, 7));
  h.send(new_order(2, S, 1505, 5, Tif::Day, OrdType::Limit, 0, 8));
  auto ev = h.send(new_order(3, B, 1505, 5, Tif::Day, OrdType::Limit, stp_flag(Stp::CancelResting), 7));
  REQUIRE(ev.size() == 3);
  CHECK_CANCELED(ev[1], 1, 1505, 5, Reason::SelfTrade);
  CHECK_TRADE(ev[2], 3, 2, 1505, 5, 0);
  // an order without an override uses the book default (cancel incoming)
  h.send(new_order(4, S, 1506, 5, Tif::Day, OrdType::Limit, 0, 9));
  ev = h.send(new_order(5, B, 1506, 5, Tif::Day, OrdType::Limit, 0, 9));  // book default: cancel incoming
  CHECK_CANCELED(ev[1], 5, 1506, 5, Reason::SelfTrade);
  CHECK(h.book.check_invariants());
}

BOOK_TEST(stp_decrement_cancel_fok_counts_decrements_but_not_cancellation) {
  Harness<Book> h(small_config(Stp::DecrementCancel));
  h.send(new_order(1, S, 1505, 3, Tif::Day, OrdType::Limit, 0, 7));  // own, smaller: decrements the FOK by 3
  h.send(new_order(2, S, 1505, 5, Tif::Day, OrdType::Limit, 0, 8));
  auto ev = h.send(new_order(3, B, 1505, 8, Tif::Fok, OrdType::Limit, 0, 7));  // 3 decremented + 5 filled
  CHECK_TRADE(ev.back(), 3, 2, 1505, 5, 0);
  h.send(new_order(4, S, 1506, 9, Tif::Day, OrdType::Limit, 0, 7));  // own, larger: would cancel the FOK
  h.send(new_order(5, S, 1506, 9, Tif::Day, OrdType::Limit, 0, 8));
  ev = h.send(new_order(6, B, 1506, 4, Tif::Fok, OrdType::Limit, 0, 7));
  CHECK_CANCELED(ev.back(), 6, 1506, 4, Reason::FokUnfilled);
  CHECK_EQ(h.book.order_count(), 2u);  // nothing touched
  CHECK(h.book.check_invariants());
}

BOOK_TEST(stp_none_allows_self_trade) {
  Harness<Book> h;
  h.send(new_order(1, S, 1505, 5, Tif::Day, OrdType::Limit, 0, 7));
  auto ev = h.send(new_order(2, B, 1505, 5, Tif::Day, OrdType::Limit, 0, 7));
  CHECK_TRADE(ev[1], 2, 1, 1505, 5, 0);
}

BOOK_TEST(engine_routes_by_symbol) {
  MatchingEngine<Book> engine(2, small_config());
  VectorSink sink;
  auto c = new_order(1, B, 1500, 5);
  c.symbol = 1;
  engine.process(c, sink);
  c.symbol = 7;
  engine.process(c, sink);
  REQUIRE(sink.events.size() == 2);
  CHECK_EQ(sink.events[0].symbol, 1u);
  CHECK_REJECTED(sink.events[1], 1, Reason::UnknownSymbol);
  CHECK(engine.book(1).contains(1));
  CHECK(!engine.book(0).contains(1));
}
