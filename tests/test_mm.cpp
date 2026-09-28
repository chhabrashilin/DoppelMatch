// The quote-position model and the book mirror, tested on hand-computed scenarios.

#include <cmath>

#include "exsim/md.hpp"
#include "exsim/md_mirror.hpp"
#include "exsim/mm.hpp"
#include "test_framework.hpp"

using namespace exsim;
using namespace exsim::mm;

namespace {

Quote live_bid(std::int64_t px, std::uint64_t size, double ahead) {
  Quote q;
  q.active = true, q.px = px, q.remaining = size, q.ahead = ahead;
  return q;
}

// Builds a Record whose `levels` point into `store` (kept alive by the caller).
md::Record diff_of(std::uint64_t first, std::uint64_t last, const std::vector<md::LevelUpdate>& bids,
                   const std::vector<md::LevelUpdate>& asks, std::vector<md::LevelUpdate>& store) {
  store = bids;
  store.insert(store.end(), asks.begin(), asks.end());
  md::Record r{};
  r.kind = md::Kind::Diff;
  r.first_id = first, r.last_id = last;
  r.n_bids = static_cast<std::uint32_t>(bids.size()), r.n_asks = static_cast<std::uint32_t>(asks.size());
  r.levels = store.data();
  return r;
}

}  // namespace

TEST(queue_trade_at_price_consumes_queue_ahead_first) {
  Quote q = live_bid(100, 10, /*ahead=*/50);
  CHECK_EQ(QuoteLogic::on_trade(q, Side::Buy, 100, 30, FillModel::Queue), 0ull);  // eats 30 of the 50 ahead
  CHECK_EQ(q.ahead, 20.0);
  CHECK_EQ(QuoteLogic::on_trade(q, Side::Buy, 100, 25, FillModel::Queue), 5ull);  // 20 ahead, then 5 of ours
  CHECK_EQ(q.remaining, 5ull);
  CHECK(q.active);
  CHECK_EQ(QuoteLogic::on_trade(q, Side::Buy, 100, 100, FillModel::Queue), 5ull);  // finishes us
  CHECK(!q.active);
}

TEST(queue_trade_through_price_fills_in_full) {
  Quote q = live_bid(100, 10, 1000);
  CHECK_EQ(QuoteLogic::on_trade(q, Side::Buy, 101, 5, FillModel::Queue), 0ull);  // above our bid: never reached us
  CHECK_EQ(QuoteLogic::on_trade(q, Side::Buy, 99, 1, FillModel::Queue), 10ull);  // swept through: everything filled
  CHECK(!q.active);
}

TEST(queue_asks_are_symmetric) {
  Quote q;
  q.active = true, q.px = 200, q.remaining = 10, q.ahead = 4;
  CHECK_EQ(QuoteLogic::on_trade(q, Side::Sell, 199, 100, FillModel::Queue), 0ull);  // below our ask: not reached
  CHECK_EQ(QuoteLogic::on_trade(q, Side::Sell, 200, 12, FillModel::Queue), 8ull);   // 4 ahead, 8 of ours
  CHECK_EQ(QuoteLogic::on_trade(q, Side::Sell, 201, 1, FillModel::Queue), 2ull);    // through: the rest
  CHECK(!q.active);
}

TEST(optimistic_model_ignores_the_queue) {
  Quote q = live_bid(100, 10, 1'000'000);
  CHECK_EQ(QuoteLogic::on_trade(q, Side::Buy, 100, 4, FillModel::Optimistic), 4ull);
  CHECK_EQ(q.remaining, 6ull);
}

TEST(cancellations_are_prorated_and_trades_are_not_double_counted) {
  Quote q = live_bid(100, 10, /*ahead=*/60);
  // level was 100. 20 lots traded at our price (already removed from `ahead` by on_trade), and the
  // level shrank by 50 in total, so 30 were cancellations. 30 of the remaining 80 (=100-20) were
  // cancelled; our share of those is ahead/base = 40/80... set up the state as the sim would.
  QuoteLogic::on_trade(q, Side::Buy, 100, 20, FillModel::Queue);  // ahead 60 -> 40
  CHECK_EQ(q.ahead, 40.0);
  QuoteLogic::on_level_change(q, /*before=*/100, /*after=*/50);
  // cancelled = (100-50) - 20 = 30; base = 100-20 = 80; ahead loses 30*40/80 = 15
  CHECK(std::abs(q.ahead - 25.0) < 1e-9);
  CHECK_EQ(q.traded_since_diff, 0.0);
}

TEST(power_rule_blames_cancellations_on_the_larger_part_of_the_queue) {
  // 40 ahead, 40 behind, 30 cancelled: proportional takes 15 from ahead; with power 3 a and b are equal, so
  // also 15. With 60 ahead and 20 behind: proportional takes 30 * 60/80 = 22.5 from ahead, power 3 takes
  // 30 * 216000/(216000 + 8000) = 28.93. The power rule concentrates cancellations on the larger side.
  Quote even = live_bid(100, 10, 40), even3 = even;
  QuoteLogic::on_level_change(even, 80, 50, 1.0);
  QuoteLogic::on_level_change(even3, 80, 50, 3.0);
  CHECK(std::abs(even.ahead - 25.0) < 1e-9);
  CHECK(std::abs(even3.ahead - 25.0) < 1e-9);
  Quote front = live_bid(100, 10, 60), front3 = front;
  QuoteLogic::on_level_change(front, 80, 50, 1.0);
  QuoteLogic::on_level_change(front3, 80, 50, 3.0);
  CHECK(std::abs(front.ahead - 37.5) < 1e-9);
  CHECK(std::abs(front3.ahead - (60.0 - 30.0 * 216000.0 / 224000.0)) < 1e-9);
  // and with most of the queue behind us, power 3 blames the back: we move up much less than proportionally
  Quote back = live_bid(100, 10, 20), back3 = back;
  QuoteLogic::on_level_change(back, 80, 50, 1.0);   // 30 * 20/80 = 7.5 from ahead
  QuoteLogic::on_level_change(back3, 80, 50, 3.0);  // 30 * 8000/224000 = 1.07 from ahead
  CHECK(std::abs(back.ahead - 12.5) < 1e-9);
  CHECK(std::abs(back3.ahead - (20.0 - 30.0 * 8000.0 / 224000.0)) < 1e-9);
}

TEST(level_growth_never_improves_position_and_ahead_is_capped_by_the_level) {
  Quote q = live_bid(100, 10, 30);
  QuoteLogic::on_level_change(q, 40, 500);  // new orders queue behind us
  CHECK_EQ(q.ahead, 30.0);
  QuoteLogic::on_level_change(q, 500, 10);  // level collapsed below our estimated position
  CHECK(q.ahead <= 10.0);
}

TEST(avellaneda_stoikov_quotes) {
  Config c;
  c.strategy = StrategyKind::AvellanedaStoikov;
  c.gamma = 0.1, c.k = 0.5, c.tau_s = 30;
  const double s2 = 4.0;  // tick^2 / s
  // flat inventory: symmetric about mid. half = 0.5*0.1*4*30 + ln(1.2)/0.1 = 6 + 1.8232
  auto d = Strategy::desired(c, 1000.0, 990, 1010, 0.0, s2);
  CHECK_EQ(*d.bid, 992);   // floor(1000 - 7.8232)
  CHECK_EQ(*d.ask, 1008);  // ceil(1000 + 7.8232)
  // long 5 units: reservation shifts down by 5*0.1*4*30 = 60 ticks, so both quotes drop and the ask
  // is offered to get rid of the inventory (the ask is clamped just above the best bid, never crossing).
  d = Strategy::desired(c, 1000.0, 990, 1010, 5.0, s2);
  CHECK(*d.bid < *d.ask);
  CHECK(*d.ask <= 1010);          // the sell side is now aggressive
  CHECK(*d.ask >= 990 + 1);       // but never crosses the best bid
  CHECK(*d.bid <= 1010 - 1);
  // at the inventory cap the strategy stops adding to the position
  c.max_inventory_units = 5;
  d = Strategy::desired(c, 1000.0, 990, 1010, 5.0, s2);
  CHECK(!d.bid.has_value());
  CHECK(d.ask.has_value());
  d = Strategy::desired(c, 1000.0, 990, 1010, -5.0, s2);
  CHECK(d.bid.has_value());
  CHECK(!d.ask.has_value());
}

TEST(join_touch_and_fixed_spread) {
  Config c;
  c.strategy = StrategyKind::JoinTouch;
  auto d = Strategy::desired(c, 1000.5, 1000, 1001, 0, 1);
  CHECK_EQ(*d.bid, 1000);
  CHECK_EQ(*d.ask, 1001);
  c.strategy = StrategyKind::FixedSpread;
  c.half_spread_ticks = 3;
  d = Strategy::desired(c, 1000.5, 1000, 1001, 0, 1);
  CHECK_EQ(*d.bid, 997);   // floor(997.5)
  CHECK_EQ(*d.ask, 1004);  // ceil(1003.5)
}

TEST(vol_estimator_converges_to_the_true_variance_rate) {
  VolEstimator v(5.0);
  // mid moves +-2 ticks every 100 ms: variance rate = 4 / 0.1 = 40 tick^2/s
  double mid = 1000;
  std::uint64_t t = 1'000'000'000;
  for (int i = 0; i < 2000; ++i) {
    mid += (i % 2 == 0) ? 2 : -2;
    t += 100'000'000;
    v.update(mid, t);
  }
  CHECK(std::abs(v.variance_rate() - 40.0) < 1.0);
}

TEST(mirror_applies_removals_before_additions_so_a_repricing_batch_never_crosses) {
  md::Record snap{};
  std::vector<md::LevelUpdate> store;
  store = {{9990, 100}, {9980, 100}, {10010, 100}, {10020, 100}};
  snap.kind = md::Kind::Snapshot, snap.n_bids = 2, snap.n_asks = 2, snap.levels = store.data(), snap.last_id = 1;
  BookMirror m(10000);
  m.seed(snap);
  CHECK_EQ(*m.book().best_bid(), 9990);
  CHECK_EQ(*m.book().best_ask(), 10010);

  // The market rallies in ONE batch: the old ask at 10010 is removed and a new bid appears at 10015.
  // Applied additions-first, the bid at 10015 would cross the stale ask at 10010 and trade.
  std::vector<md::LevelUpdate> s2;
  m.apply(diff_of(2, 5, {{10015, 50}}, {{10010, 0}}, s2));
  CHECK_EQ(m.phantom_trades(), 0ull);
  CHECK_EQ(*m.book().best_bid(), 10015);
  CHECK_EQ(*m.book().best_ask(), 10020);
  CHECK_EQ(m.qty_at(Side::Buy, 10015), 50ull);
  CHECK_EQ(m.qty_at(Side::Sell, 10010), 0ull);
  CHECK(m.book().check_invariants());
}

TEST(mirror_knows_its_completeness_horizon_and_asks_for_a_reseed_as_the_market_moves) {
  // A snapshot of 10 levels per side: bids 9990 down to 9900, asks 10010 up to 10100 (step 10).
  std::vector<md::LevelUpdate> store;
  for (int i = 0; i < 10; ++i) store.push_back({9990 - 10 * i, 100});
  for (int i = 0; i < 10; ++i) store.push_back({10010 + 10 * i, 100});
  md::Record snap{};
  snap.kind = md::Kind::Snapshot, snap.n_bids = 10, snap.n_asks = 10, snap.levels = store.data();
  BookMirror m(10000, 1024);
  m.seed(snap);
  CHECK_EQ(m.bid_horizon(), 9900);
  CHECK_EQ(m.ask_horizon(), 10100);
  CHECK(!m.needs_reseed());

  // A level beyond the horizon can appear via a diff, but the mirror does not vouch for it.
  std::vector<md::LevelUpdate> s;
  m.apply(diff_of(1, 1, {}, {{10150, 7}}, s));
  CHECK_EQ(m.top(Side::Sell, 20).size(), 11u);
  CHECK_EQ(m.top(Side::Sell, 20, /*within_horizon=*/true).size(), 10u);

  // The market rallies: the first five asks are consumed. Half the seeded ask depth (90 ticks) is gone
  // when the best ask is more than 45 ticks from the horizon of 10100, i.e. above 10055.
  m.apply(diff_of(2, 2, {}, {{10010, 0}, {10020, 0}, {10030, 0}, {10040, 0}}, s));
  CHECK_EQ(*m.book().best_ask(), 10050);
  CHECK(!m.needs_reseed());  // 10100 - 10050 = 50 ticks left, more than half of 90
  m.apply(diff_of(3, 3, {}, {{10050, 0}}, s));
  CHECK_EQ(*m.book().best_ask(), 10060);
  CHECK(m.needs_reseed());   // 40 < 45
}

TEST(mirror_handles_shrink_growth_delete_and_out_of_band_prices) {
  md::Record snap{};
  std::vector<md::LevelUpdate> store{{9990, 100}, {10010, 100}};
  snap.kind = md::Kind::Snapshot, snap.n_bids = 1, snap.n_asks = 1, snap.levels = store.data();
  BookMirror m(10000, /*num_levels=*/1024);
  m.seed(snap);
  std::vector<md::LevelUpdate> s;
  m.apply(diff_of(1, 1, {{9990, 40}}, {{10010, 250}}, s));  // shrink one, grow the other
  CHECK_EQ(m.qty_at(Side::Buy, 9990), 40ull);
  CHECK_EQ(m.qty_at(Side::Sell, 10010), 250ull);
  m.apply(diff_of(2, 2, {{9990, 0}, {9000000, 5}}, {}, s));  // delete, and one price far outside the band
  CHECK_EQ(m.qty_at(Side::Buy, 9990), 0ull);
  CHECK_EQ(m.out_of_band(), 1ull);
  CHECK(!m.book().best_bid().has_value());
  const auto top = m.top(Side::Sell, 5);
  REQUIRE(top.size() == 1);
  CHECK_EQ(top[0].px, 10010);
  CHECK_EQ(top[0].qty, 250ull);
  CHECK(m.book().check_invariants());
}
