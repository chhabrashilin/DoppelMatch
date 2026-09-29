// exsim_mdlisten: a market-data subscriber (Linux). Rebuilds the level-2 book of every symbol from the exchange's
// public feed (mdfeed.hpp), recovering from gaps through snapshots, and prints a digest of the book it holds. At the
// end of the feed that digest must equal the one the server prints for its own engine.
//
//   exsim_mdlisten --group 239.255.0.2:31010 [--timeout-s 120]

#include <csignal>
#include <cstdio>

#include "args.hpp"
#include "exsim/mdfeed.hpp"
#include "gateway.hpp"

using namespace exsim;

int main(int argc, char** argv) {
  const tools::Args args(argc, argv);
  std::signal(SIGINT, tools::on_stop_signal);
  std::signal(SIGTERM, tools::on_stop_signal);
  md_feed::Book book(args.str("group", "239.255.0.2:31010"));
  const std::uint64_t deadline = seqstream::now_ns() + args.u64("timeout-s", 120) * 1'000'000'000ull;
  std::printf("MD_LISTENING\n");
  std::fflush(stdout);
  bool was_synced = false;
  while (!tools::g_stop && !book.ended() && seqstream::now_ns() < deadline) {
    book.poll(10);
    if (book.synced() && !was_synced) std::printf("SYNCED at incremental %llu\n", static_cast<unsigned long long>(book.last_seq()));
    was_synced = book.synced();
  }
  const auto& s = book.stats();
  std::printf("%s incrementals=%llu gaps=%llu snapshots_used=%llu trades=%llu last_seq=%llu l2=%016llx\n",
              book.ended() ? "MD_END" : "MD_STOPPED", static_cast<unsigned long long>(s.incrementals),
              static_cast<unsigned long long>(s.gaps), static_cast<unsigned long long>(s.snapshots_used),
              static_cast<unsigned long long>(s.trades), static_cast<unsigned long long>(book.last_seq()),
              static_cast<unsigned long long>(book.digest()));
  return book.ended() ? 0 : 1;
}
