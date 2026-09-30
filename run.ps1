# Runs DoppelMatch from Windows PowerShell by forwarding to a Linux build under WSL.
# Needs WSL with cmake, ninja and a C++20 compiler installed in the default distribution.
#
#   .\run.ps1 build                    # configure and build (cmake --preset release)
#   .\run.ps1 test
#   .\run.ps1 demo                     # gen + replay + bench + pipeline, about a minute
#   .\run.ps1 gen [messages]           # write a capture, default 10M
#   .\run.ps1 replay [book]            # book: aos (default) | soa | hybrid | ref
#   .\run.ps1 bench                    # all design points + SPSC ring
#   .\run.ps1 pipeline [msgs/sec]      # saturated, or paced if a rate is given
#   .\run.ps1 jitter                   # machine noise floor
param(
  [Parameter(Position = 0)][string]$Cmd = "demo",
  [Parameter(Position = 1)][string]$Arg
)

# The repository as a WSL path, e.g. C:\src\DoppelMatch -> /mnt/c/src/DoppelMatch
$win = $PSScriptRoot
$src = "/mnt/" + $win.Substring(0, 1).ToLower() + $win.Substring(2).Replace('\', '/')
$bin = "$src/build/release"

function Run([string]$bash) { wsl -- bash -c $bash }

switch ($Cmd) {
  "build"    { Run "cd '$src' && cmake --preset release > /dev/null && cmake --build --preset release" }
  "test"     { Run "cd '$bin' && ./exsim_tests | tail -3" }
  "gen"      { $n = if ($Arg) { $Arg } else { "10000000" }; Run "cd '$bin' && ./exsim_gen --out flow.bin --messages $n" }
  "replay"   { $b = if ($Arg) { $Arg } else { "aos" }; Run "cd '$bin' && ./exsim_replay --in flow.bin --book $b" }
  "bench"    { Run "cd '$bin' && ./exsim_bench --spsc" }
  "pipeline" { $r = if ($Arg) { "--rate $Arg" } else { "" }; Run "cd '$bin' && ./exsim_pipeline --in flow.bin $r" }
  "jitter"   { Run "cd '$bin' && ./exsim_pipeline --jitter 10" }
  "demo"     {
    Run "cd '$bin' && ./exsim_tests | tail -1 && ./exsim_gen --out flow.bin --messages 5000000 && ./exsim_replay --in flow.bin --book aos && ./exsim_bench --messages 2000000 --reps 5 --no-latency --impl ref,aos && ./exsim_pipeline --in flow.bin --rate 1000000 | grep -E 'throughput|match|digest'"
  }
  default    { Write-Host "Unknown command '$Cmd'. Try: build, test, demo, gen, replay, bench, pipeline, jitter" }
}
