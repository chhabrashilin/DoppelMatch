// Test runner. `exsim_tests [substring]` runs the cases whose names contain the substring.
//
// This translation unit also replaces global operator new with a counting version, which lets
// test_alloc.cpp prove that the matching hot path never touches the heap.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#if defined(__unix__)
#include <csignal>
#include <unistd.h>
#endif

#include "exsim/common.hpp"
#include "test_framework.hpp"

// A corrupted data structure (a FIFO cycle, a broken probe sequence) tends to hang rather than fail. The watchdog turns
// a hang into a failure that names the test, instead of a CI job that stalls until its own timeout.
namespace {
constexpr unsigned kCaseTimeoutS = 60;
const char* volatile g_current = "";
#if defined(__unix__)
extern "C" void on_hang(int) {
  const char* msg = "\n[HANG] test case did not finish within the watchdog limit: ";
  (void)!::write(STDOUT_FILENO, msg, std::strlen(msg));
  (void)!::write(STDOUT_FILENO, g_current, std::strlen(g_current));
  (void)!::write(STDOUT_FILENO, "\n", 1);
  ::_exit(2);
}
#endif
}  // namespace

std::atomic<unsigned long long> g_test_allocs{0};

// GCC's -Wmismatched-new-delete cannot see that these replacements pair malloc with free.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
#if !EXSIM_TSAN_BUILD
void* operator new(std::size_t n) {
  g_test_allocs.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
#endif
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

int main(int argc, char** argv) {
  using namespace exsim::test;
  const char* filter = argc > 1 ? argv[1] : "";
  int run = 0, failed_cases = 0;
#if defined(__unix__)
  std::signal(SIGALRM, on_hang);
#endif
  const auto t0 = std::chrono::steady_clock::now();
  for (const Case& c : registry()) {
    if (std::strstr(c.name, filter) == nullptr) continue;
    ++run;
    const int before = failures();
    const auto c0 = std::chrono::steady_clock::now();
    g_current = c.name;
#if defined(__unix__)
    ::alarm(kCaseTimeoutS);
#endif
    try {
      c.fn();
    } catch (const Failure&) {
    } catch (const std::exception& e) {
      report(__FILE__, __LINE__, std::string("unexpected exception: ") + e.what());
    }
#if defined(__unix__)
    ::alarm(0);
#endif
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count();
    const bool ok = failures() == before;
    failed_cases += ok ? 0 : 1;
    std::printf("[%s] %-52s %8.1f ms\n", ok ? " OK " : "FAIL", c.name, ms);
    std::fflush(stdout);  // so a crash or hang in the next case still shows how far the suite got
  }
  const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("\n%d/%d test cases passed, %lld assertions, %d failures (%.2f s)\n", run - failed_cases, run,
              assertions(), failures(), s);
  return failed_cases == 0 && run > 0 ? 0 : 1;
}
