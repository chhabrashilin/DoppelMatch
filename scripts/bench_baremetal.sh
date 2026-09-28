#!/usr/bin/env bash
# Benchmarks on real hardware, with the machine's configuration recorded next to the numbers.
#
#   scripts/bench_baremetal.sh [build-dir]      ->  results/baremetal/<host>-<date>.txt
#
# Needs Linux, CMake, Ninja and GCC 13+ or Clang 17+. No root required. For the most stable numbers (optional, root):
#   sudo cpupower frequency-set -g performance                      # fixed frequency governor
#   echo 1 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo  # or the AMD boost equivalent
#   boot with isolcpus=2,4 nohz_full=2,4 so the pinned cores see no other work
#   sudo sysctl kernel.perf_event_paranoid=1                        # lets perf read hardware counters
# The report states which of these were in effect, so numbers from different machines stay comparable.
set -uo pipefail
cd "$(dirname "$0")/.."
BUILD=${1:-build/baremetal}
OUT=results/baremetal/$(hostname -s)-$(date +%Y%m%d-%H%M).txt
mkdir -p results/baremetal

{
  echo "# exchange-sim bare-metal run, $(date -u +%Y-%m-%dT%H:%MZ)"
  echo "## machine"
  echo "kernel: $(uname -srm)"
  lscpu | grep -E "^(Model name|CPU\(s\)|Thread\(s\) per core|Core\(s\) per socket|Socket\(s\)|L1d|L2|L3|Hypervisor vendor|Virtualization type|CPU max MHz)" || true
  echo "virtualization: $(systemd-detect-virt 2>/dev/null || echo unknown)"
  echo "governor (cpu2): $(cat /sys/devices/system/cpu/cpu2/cpufreq/scaling_governor 2>/dev/null || echo n/a)"
  echo "intel no_turbo: $(cat /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null || echo n/a); amd boost: $(cat /sys/devices/system/cpu/cpufreq/boost 2>/dev/null || echo n/a)"
  echo "kernel command line: $(cat /proc/cmdline)"
  echo "perf_event_paranoid: $(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo n/a)"
  echo "compiler: $(${CXX:-c++} --version | head -1)"
  echo "commit: $(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
} > "$OUT"

echo "building into $BUILD (Release, -march=native) ..."
cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release > /dev/null && cmake --build "$BUILD" > /dev/null || { echo "build failed"; exit 1; }

run() {  # run <title> <command...>: appends the command's output to the report
  echo "== $1" | tee -a "$OUT"
  { echo; echo "## $1"; echo "\$ ${*:2}"; "${@:2}" 2>&1; } >> "$OUT"
}
run "noise floor: time stolen from a pinned core" "$BUILD/exsim_pipeline" --jitter 10
run "design points (15 interleaved reps, latency, SPSC ring)" "$BUILD/exsim_bench" --messages 5000000 --reps 15 --spsc
run "footprint effect (small capacity)" "$BUILD/exsim_bench" --max-orders 16384 --impl aos-scatter,aos --reps 9 --no-latency
run "locality hash under attack" "$BUILD/exsim_bench" --adversarial --messages 200000 --reps 1 --no-latency
run "id-index deletion" "$BUILD/exsim_bench_index"
"$BUILD/exsim_gen" --out "$BUILD/flow.bin" --messages 10000000 > /dev/null
run "pipeline, saturated" "$BUILD/exsim_pipeline" --in "$BUILD/flow.bin"
run "pipeline, paced at 1M msg/s" "$BUILD/exsim_pipeline" --in "$BUILD/flow.bin" --rate 1000000
if perf stat -e L1-dcache-load-misses true > /dev/null 2>&1; then
  run "hardware cache counters per layout (perf)" bash scripts/cache_profile.sh "$BUILD" 1000000
else
  echo "== hardware counters unavailable (perf missing or perf_event_paranoid too high); skipped" | tee -a "$OUT"
fi
run "gateway over loopback TCP" bash scripts/e2e_gateway.sh "$BUILD"
echo
echo "report: $OUT"
