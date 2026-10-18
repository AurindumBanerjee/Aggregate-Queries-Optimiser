// Out-of-process crash test: a worker process is killed (TerminateProcess / SIGKILL) at random
// moments while it commits transactions; the parent then recovers the database and checks that it
// equals the workload model after the acknowledged transactions (plus possibly the one in flight).
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <thread>

#include "test_util.hpp"
#include "workload.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace aqo {
namespace {
using namespace aqo::testing;

#ifdef _WIN32
struct Proc {
  PROCESS_INFORMATION pi{};
  bool start(const std::string& cmd) {
    STARTUPINFOA si{};
    si.cb = sizeof si;
    std::string mutable_cmd = cmd;
    return CreateProcessA(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi) != 0;
  }
  void kill_now() {
    TerminateProcess(pi.hProcess, 137);
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
  }
};
#else
struct Proc {
  pid_t pid = -1;
  bool start(const std::string& cmd) {
    pid = fork();
    if (pid == 0) {
      execl("/bin/sh", "sh", "-c", ("exec " + cmd).c_str(), static_cast<char*>(nullptr));
      _exit(127);
    }
    return pid > 0;
  }
  void kill_now() {
    ::kill(pid, SIGKILL);
    int st;
    waitpid(pid, &st, 0);
  }
};
#endif

std::uint64_t last_ack(const std::string& path) {
  std::ifstream in(path);
  std::uint64_t v = 0, last = 0;
  while (in >> v) last = v;  // a torn final line parses as a smaller/odd number only if killed mid-write
  return last;
}

void run_kill_test(std::uint64_t seed, bool sync_commit, int rounds, int ckpt_every) {
  std::string db = unique_path("kill_" + std::to_string(seed), ".db");
  std::string ack = db + ".ack";
  for (auto* ext : {"", ".wal", ".wal.tmp", ".ack"}) std::filesystem::remove(db + ext);
  std::mt19937_64 rng(seed);
  std::uint64_t cur = 0;  // number of workload transactions the recovered database must reflect
  std::uint64_t total_acked = 0, in_flight_commits = 0;
  for (int r = 0; r < rounds; ++r) {
    std::filesystem::remove(ack);
    std::string cmd = std::string(AQO_CRASH_WORKER_PATH) + " \"" + db + "\" " + std::to_string(seed) + " " +
                      std::to_string(cur) + " \"" + ack + "\" 16 " + (sync_commit ? "1" : "0") + " " +
                      std::to_string(ckpt_every);
    Proc p;
    ASSERT_TRUE(p.start(cmd)) << cmd;
    std::this_thread::sleep_for(std::chrono::milliseconds(40 + rng() % 360));
    p.kill_now();

    std::uint64_t acked_through = std::max(cur, last_ack(ack));  // txns [0, acked_through) acknowledged
    total_acked += acked_through - cur;
    StorageEngine e([&] {
      EngineOptions o;
      o.path = db;
      o.pool_frames = 64;
      return o;
    }());
    auto table = open_table(e, false);
    Model got = table ? dump(*table) : Model{};
    if (table) {
      ASSERT_EQ(table->validate(), "") << "round " << r;
    }
    if (got == model_after(seed, acked_through)) {
      cur = acked_through;
    } else if (got == model_after(seed, acked_through + 1)) {
      cur = acked_through + 1;  // the kill landed after the commit record became durable but before the ack
      ++in_flight_commits;
    } else {
      FAIL() << "round " << r << ": database matches neither " << acked_through << " nor " << acked_through + 1
             << " committed transactions (has " << got.size() << " keys)";
    }
  }
  EXPECT_GT(total_acked, 0u) << "worker never made progress; kill timing too aggressive";
  std::printf("[kill-test] seed %llu: %llu txns acked across %d kills, %llu in-flight commits recovered\n",
              static_cast<unsigned long long>(seed), static_cast<unsigned long long>(total_acked), rounds, static_cast<unsigned long long>(in_flight_commits));
  for (auto* ext : {"", ".wal", ".wal.tmp", ".ack"}) std::filesystem::remove(db + ext);
}

TEST(KillNine, RecoversAfterRandomProcessKillsNoFsync) { run_kill_test(11, false, 25, 0); }
TEST(KillNine, RecoversAfterRandomProcessKillsWithFsync) { run_kill_test(12, true, 12, 0); }
TEST(KillNine, RecoversWhenKilledAroundCheckpoints) { run_kill_test(13, false, 25, 7); }

}  // namespace
}  // namespace aqo
