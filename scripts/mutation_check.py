#!/usr/bin/env python3
"""Planted-bug check: does the test suite notice realistic mistakes?

Each mutant below is a one-line bug. For each, the script copies the repository to a scratch directory, applies the bug,
rebuilds exsim_tests and runs it. A mutant is caught if any test fails, the binary crashes, or the per-test watchdog
reports a hang. Exits non-zero if any mutant survives. The results are in docs/TESTING.md.

    python3 scripts/mutation_check.py [--cxx g++] [--work /tmp/exsim-mutants]
"""
import argparse, os, re, shutil, subprocess, sys, tempfile, time

# (id, the bug, file, exact text replaced, replacement)
MUTANTS = [
    ("overfill", "a taker fills the maker's whole size, not the smaller of the two",
     "include/exsim/order_book.hpp", "const Qty fill = avail < qty ? avail : qty;", "const Qty fill = avail;"),
    ("level-total", "cancel forgets to subtract the order from its level's total",
     "include/exsim/order_book.hpp", "    lv.qty -= store_.qty(s);\n    if (lv.head == kNil) {", "    if (lv.head == kNil) {"),
    ("amend-grows", "a size increase at the same price keeps queue priority",
     "include/exsim/order_book.hpp", "if (ix == static_cast<std::int32_t>(store_.level(s)) && c.qty <= store_.qty(s)) {",
     "if (ix == static_cast<std::int32_t>(store_.level(s)) && c.qty <= store_.qty(s) + 1) {"),
    ("fok-always", "fill-or-kill never checks available liquidity",
     "include/exsim/order_book.hpp", "    std::uint64_t need = qty;\n", "    std::uint64_t need = qty;\n    if (need) return true;\n"),
    ("stp-equal", "decrement-and-cancel with equal sizes cancels only the resting order",
     "include/exsim/order_book.hpp", "if (mq == qty) {  // equal: both are cancelled",
     "if (mq == qty && false) {  // equal: both are cancelled"),
    ("stale-tail", "removing the tail order leaves the level's tail pointing at it",
     "include/exsim/order_book.hpp", "lv.tail = lv.head == kNil ? kNil : p;", "lv.tail = lv.head == kNil ? kNil : s;"),
    ("bitmap-summary", "the bitmap's summary bit is not cleared when a word empties",
     "include/exsim/price_bitmap.hpp", "if (words_[w] == 0) summary_[w >> 6] &= ~bit(w);", ""),
    ("robin-hood", "insertion drops the Robin Hood swap",
     "include/exsim/order_index.hpp", "if (const std::uint32_t sd = dist(i, slots_[i].fp); sd < d) {",
     "if (const std::uint32_t sd = dist(i, slots_[i].fp); sd < d && false) {"),
    ("delete-stop", "backward-shift deletion stops one entry early",
     "include/exsim/order_index.hpp", "slots_[j].value != kNil && dist(j, slots_[j].fp) != 0;",
     "slots_[j].value != kNil && dist(j, slots_[j].fp) > 1;"),
    ("crc-ignored", "the journal reader skips the CRC check",
     "include/exsim/journal.hpp", "if (Crc32c::of(buf.data(), buf.size()) != m.crc)", "if (false)"),
    ("rate-burst", "the rate limiter admits one message more than its burst",
     "include/exsim/risk.hpp", "cfg_.burst ? cfg_.burst - 1 : 0", "cfg_.burst"),
    ("collar-sign", "the price collar only checks prices above the last trade",
     "include/exsim/risk.hpp", "if ((d < 0 ? -d : d) > cfg_.collar_ticks)", "if (d > cfg_.collar_ticks)"),
]


def sh(cmd, timeout=None):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=timeout)


def run_tests(bld):  # not through a shell, so a crash shows up as a negative return code (the signal)
    return subprocess.run([os.path.join(bld, "exsim_tests")], capture_output=True, text=True, timeout=900)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cxx", default="g++")
    ap.add_argument("--work", default=os.path.join(tempfile.gettempdir(), "exsim-mutants"))
    args = ap.parse_args()
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    src, bld = os.path.join(args.work, "src"), os.path.join(args.work, "build")
    shutil.rmtree(args.work, ignore_errors=True)  # a stale build would test an old mutant
    shutil.copytree(root, src, ignore=shutil.ignore_patterns(".git", "build", "results", "docs", "data", "ui", "ocaml"))

    r = sh(f"cmake -S {src} -B {bld} -G Ninja -DCMAKE_CXX_COMPILER={args.cxx} -DCMAKE_BUILD_TYPE=Release -DEXSIM_NATIVE=OFF")
    if r.returncode != 0:
        sys.exit(r.stdout + r.stderr)
    if sh(f"cmake --build {bld} --target exsim_tests").returncode != 0:
        sys.exit("the unmodified source does not build")
    base = run_tests(bld)
    if base.returncode != 0:
        sys.exit("the unmodified suite fails:\n" + base.stdout[-2000:])
    print("baseline:", base.stdout.strip().splitlines()[-1], flush=True)

    survived = 0
    for mid, what, rel, old, new in MUTANTS:
        path = os.path.join(src, rel)
        orig = open(path).read()
        if orig.count(old) != 1:
            print(f"{mid:15} STALE: the text it replaces no longer appears exactly once in {rel}", flush=True)
            survived += 1
            continue
        open(path, "w").write(orig.replace(old, new))
        t0 = time.time()
        try:
            if sh(f"cmake --build {bld} --target exsim_tests").returncode != 0:
                verdict, detail = "CAUGHT", "does not compile"
            else:
                t = run_tests(bld)
                lines = t.stdout.strip().splitlines()
                fails = [l.split()[1] for l in lines if l.startswith("[FAIL]")]
                hang = [l for l in lines if l.startswith("[HANG]")]
                m = re.search(r" (\d+) failures", t.stdout)
                if t.returncode < 0:
                    verdict, detail = "CAUGHT", f"crash (signal {-t.returncode})"
                elif hang:
                    verdict, detail = "CAUGHT", hang[0][len("[HANG] "):]
                elif t.returncode != 0 or not m or int(m.group(1)) != 0:
                    verdict, detail = "CAUGHT", f"{len(fails)} failing cases, first {fails[0] if fails else '?'}"
                else:
                    verdict, detail = "SURVIVED", "every test passed"
                    survived += 1
        finally:
            open(path, "w").write(orig)
        print(f"{mid:15} {verdict:9} {time.time() - t0:4.0f}s  {what}: {detail}", flush=True)

    print(f"\n{len(MUTANTS) - survived}/{len(MUTANTS)} planted bugs caught")
    sys.exit(1 if survived else 0)


if __name__ == "__main__":
    main()
