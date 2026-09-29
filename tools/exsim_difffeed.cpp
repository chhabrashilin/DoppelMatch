// exsim_difffeed: the C++ half of the cross-language differential test.
//
// The OCaml reference model (ocaml/lib/exsim_ref.ml) is a second implementation of the matching rules,
// written independently and purely functionally. Both sides speak one canonical text format, so their
// outputs for the same command stream can be compared with `cmp`. scripts/ocaml_diff.sh runs the loop.
//
//   exsim_difffeed --gen 1000000 --seed 7 --out cmds.txt     write a random stream (first line CONFIG)
//   exsim_difffeed --in cmds.txt [--book aos|soa|hybrid|ref] print the events, one per line (streamed; a
//                                                           CONFIG line anywhere starts a fresh book)
//
// Commands: "N id side price qty ord_type tif flags owner", "X id", "U id price qty".
// Events:   "A id side price qty", "R id side request reason", "T taker maker side price qty leaves",
//           "C id side price qty reason", "M id side price qty reason".
//
// The generator is the one in tests/test_differential.cpp: a narrow band, a small capacity and four owners,
// so band edges, BookFull and every self-trade prevention mode are hit constantly.

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "args.hpp"
#include "exsim/matching_engine.hpp"
#include "exsim/reference_book.hpp"
#include "exsim/workload.hpp"

namespace {

using namespace exsim;

Command random_command(Rng& rng, Price& mid, OrderId& next_id, const BookConfig& cfg) {
  const Price lo = cfg.min_price, hi = cfg.min_price + static_cast<Price>(cfg.num_levels) - 1;
  if (rng.chance(0.05)) mid = std::clamp<Price>(mid + static_cast<Price>(rng.below(5)) - 2, lo + 5, hi - 5);
  auto recent_id = [&] { return next_id <= 1 ? OrderId{1} : next_id - 1 - rng.below(std::min<OrderId>(next_id - 1, 300)); };
  auto near_price = [&] { return mid + static_cast<Price>(rng.below(31)) - 15; };

  const double r = rng.uniform();
  Command c{};
  if (r < 0.50) {
    c.type = MsgType::NewOrder;
    c.order_id = rng.chance(0.02) ? recent_id() : next_id++;
    if (rng.chance(0.005)) c.order_id = 0;
    c.side = rng.chance(0.5) ? Side::Buy : Side::Sell;
    c.price = near_price();
    c.qty = rng.chance(0.01) ? 0 : static_cast<Qty>(1 + rng.below(20));
    c.owner = static_cast<OwnerId>(1 + rng.below(4));
    const double t = rng.uniform();
    if (t < 0.05) c.ord_type = OrdType::Market;
    if (t >= 0.05 && t < 0.13) c.tif = Tif::Ioc;
    if (t >= 0.13 && t < 0.17) c.tif = Tif::Fok;
    if (t >= 0.17 && t < 0.22) c.flags = kFlagPostOnly;
    if (rng.chance(0.3)) c.flags |= stp_flag(static_cast<Stp>(rng.below(4)));
  } else if (r < 0.80) {
    c.type = MsgType::Cancel;
    c.order_id = rng.chance(0.9) ? recent_id() : rng.below(next_id + 10);
  } else {
    c.type = MsgType::Modify;
    c.order_id = rng.chance(0.95) ? recent_id() : rng.below(next_id + 10);
    c.price = near_price();
    c.qty = rng.chance(0.01) ? 0 : static_cast<Qty>(1 + rng.below(25));
  }
  return c;
}

void write_command(std::FILE* f, const Command& c) {
  switch (c.type) {
    case MsgType::NewOrder:
      std::fprintf(f, "N %" PRIu64 " %u %" PRId64 " %" PRIu64 " %u %u %u %u\n", c.order_id, unsigned(c.side), c.price,
                   c.qty, unsigned(c.ord_type), unsigned(c.tif), unsigned(c.flags), unsigned(c.owner));
      break;
    case MsgType::Cancel: std::fprintf(f, "X %" PRIu64 "\n", c.order_id); break;
    case MsgType::Modify: std::fprintf(f, "U %" PRIu64 " %" PRId64 " %" PRIu64 "\n", c.order_id, c.price, c.qty); break;
  }
}

bool parse_command(const std::string& line, Command& c) {
  std::istringstream in(line);
  std::string tag;
  if (!(in >> tag)) return false;
  c = Command{};
  if (tag == "N") {
    unsigned side, ot, tif, flags, owner;
    in >> c.order_id >> side >> c.price >> c.qty >> ot >> tif >> flags >> owner;
    c.type = MsgType::NewOrder, c.side = static_cast<Side>(side), c.ord_type = static_cast<OrdType>(ot);
    c.tif = static_cast<Tif>(tif), c.flags = static_cast<std::uint8_t>(flags), c.owner = owner;
  } else if (tag == "X") {
    in >> c.order_id;
    c.type = MsgType::Cancel;
  } else if (tag == "U") {
    in >> c.order_id >> c.price >> c.qty;
    c.type = MsgType::Modify;
  } else {
    return false;
  }
  return !in.fail();
}

struct TextSink {
  std::FILE* out;
  std::uint64_t n = 0;
  void on_event(const Event& e) {
    ++n;
    const unsigned side = unsigned(e.side), reason = unsigned(e.reason);
    switch (e.type) {
      case EventType::Accepted:
        std::fprintf(out, "A %" PRIu64 " %u %" PRId64 " %" PRIu64 "\n", e.order_id, side, e.price, e.qty);
        break;
      case EventType::Rejected:
        std::fprintf(out, "R %" PRIu64 " %u %u %u\n", e.order_id, side, unsigned(e.request), reason);
        break;
      case EventType::Trade:
        std::fprintf(out, "T %" PRIu64 " %" PRIu64 " %u %" PRId64 " %" PRIu64 " %" PRIu64 "\n", e.order_id, e.maker_id,
                     side, e.price, e.qty, e.leaves);
        break;
      case EventType::Canceled:
        std::fprintf(out, "C %" PRIu64 " %u %" PRId64 " %" PRIu64 " %u\n", e.order_id, side, e.price, e.qty, reason);
        break;
      case EventType::Modified:
        std::fprintf(out, "M %" PRIu64 " %u %" PRId64 " %" PRIu64 " %u\n", e.order_id, side, e.price, e.qty, reason);
        break;
    }
  }
};

bool parse_config(const std::string& line, BookConfig& cfg) {
  std::istringstream h(line);
  std::string tag;
  unsigned stp = 0;
  h >> tag >> cfg.min_price >> cfg.num_levels >> cfg.max_orders >> stp;
  cfg.stp = static_cast<Stp>(stp);
  return tag == "CONFIG" && !h.fail();
}

// Streams the file: a CONFIG line starts a fresh book (the first line, and again wherever the producer rebuilt its
// book, as exsim_l3replay does after a gap in the exchange feed). Returns {commands, events}.
template <class Book>
std::pair<std::uint64_t, std::uint64_t> replay(std::istream& in) {
  std::unique_ptr<Book> book;
  TextSink sink{stdout};
  std::uint64_t n = 0;
  std::string line;
  Command c;
  BookConfig cfg;
  while (std::getline(in, line)) {
    if (parse_config(line, cfg)) {
      book = std::make_unique<Book>(0, cfg);
      continue;
    }
    if (!parse_command(line, c)) continue;
    if (!book) tools::Args::die("a command before the first CONFIG line");
    ++n;
    switch (c.type) {
      case MsgType::NewOrder: book->add(c, sink); break;
      case MsgType::Cancel: book->cancel(c, sink); break;
      case MsgType::Modify: book->modify(c, sink); break;
    }
  }
  return {n, sink.n};
}

}  // namespace

int main(int argc, char** argv) {
  const tools::Args args(argc, argv);

  if (args.has("gen")) {
    const std::uint64_t n = args.u64("gen", 1'000'000);
    Rng rng(args.u64("seed", 1));
    BookConfig cfg;
    cfg.min_price = 1000, cfg.num_levels = 128, cfg.max_orders = 200;
    cfg.stp = static_cast<Stp>(args.u64("stp", rng.below(4)));
    std::FILE* f = std::fopen(args.str("out", "cmds.txt").c_str(), "w");
    if (f == nullptr) tools::Args::die("cannot open --out");
    std::fprintf(f, "CONFIG %" PRId64 " %u %u %u\n", cfg.min_price, cfg.num_levels, cfg.max_orders, unsigned(cfg.stp));
    Price mid = cfg.min_price + static_cast<Price>(cfg.num_levels / 2);
    OrderId next_id = 1;
    for (std::uint64_t i = 0; i < n; ++i) write_command(f, random_command(rng, mid, next_id, cfg));
    std::fclose(f);
    return 0;
  }

  if (!args.has("in")) tools::Args::die("--gen N or --in <file> is required");
  std::ifstream in(args.str("in", ""));
  if (!in) tools::Args::die("cannot open --in");
  const std::string book = args.str("book", "hybrid");
  std::pair<std::uint64_t, std::uint64_t> r;
  if (book == "aos")
    r = replay<AosBook>(in);
  else if (book == "soa")
    r = replay<SoaBook>(in);
  else if (book == "hybrid")
    r = replay<HybridBook>(in);
  else if (book == "ref")
    r = replay<ReferenceBook>(in);
  else
    tools::Args::die("--book must be aos, soa, hybrid or ref");
  std::fprintf(stderr, "%" PRIu64 " commands, %" PRIu64 " events\n", r.first, r.second);
  return 0;
}
