# Runs DoppelMatch from Windows PowerShell by forwarding to the WSL (Linux) build.
#
#   .\run.ps1 test
#   .\run.ps1 demo                     # gen + replay + bench + pipeline, ~1 minute
#   .\run.ps1 gen [messages]           # write a capture, default 10M
#   .\run.ps1 replay [book]            # book: aos (default) | soa | hybrid | ref
#   .\run.ps1 bench                    # all design points + SPSC ring
#   .\run.ps1 pipeline [msgs/sec]      # saturated, or paced if a rate is given
#   .\run.ps1 jitter                   # machine noise floor
#   .\run.ps1 build                    # rebuild after editing code
param(
  [Parameter(Position = 0)][string]$Cmd = "demo",
  [Parameter(Position = 1)][string]$Arg
)

$bin = '$HOME/build/exsim-rel'
$tc = '$HOME/bin/tc'

function Run([string]$bash) { wsl -d Ubuntu -- bash -c $bash }

switch ($Cmd) {
  "test"     { Run "cd $bin && ./exsim_tests | tail -3" }
  "gen"      { $n = if ($Arg) { $Arg } else { "10000000" }; Run "cd $bin && ./exsim_gen --out flow.bin --messages $n" }
  "replay"   { $b = if ($Arg) { $Arg } else { "aos" }; Run "cd $bin && ./exsim_replay --in flow.bin --book $b" }
  "bench"    { Run "cd $bin && ./exsim_bench --spsc" }
  "pipeline" { $r = if ($Arg) { "--rate $Arg" } else { "" }; Run "cd $bin && ./exsim_pipeline --in flow.bin $r" }
  "jitter"   { Run "cd $bin && ./exsim_pipeline --jitter 10" }
  "build"    {
    $win = (Get-Location).Path
    $src = "/mnt/" + $win.Substring(0, 1).ToLower() + $win.Substring(2).Replace('\', '/')
    Run "$tc bash -c 'export CXXFLAGS=\`"-isystem \`$CONDA_PREFIX/include\`"; cmake -S $src -B $bin -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=\`$CXX >/dev/null && cmake --build $bin'"
  }
  "demo"     {
    Run "cd $bin && ./exsim_tests | tail -1 && ./exsim_gen --out flow.bin --messages 5000000 && ./exsim_replay --in flow.bin --book aos && ./exsim_bench --messages 2000000 --reps 5 --no-latency --impl ref,aos && ./exsim_pipeline --in flow.bin --rate 1000000 | grep -E 'throughput|match|digest'"
  }
  default    { Write-Host "Unknown command '$Cmd'. Try: test, demo, gen, replay, bench, pipeline, jitter, build" }
}
