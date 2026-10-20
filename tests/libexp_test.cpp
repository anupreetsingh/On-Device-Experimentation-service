// libexp keeps process-wide state and reads EXP_STATE_DIR once, so these tests run in their
// own executable, in order, against one state directory set up in main().
#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "exp.h"
#include "files.h"
#include "libexp_internal.h"
#include "snapshot_writer.h"
#include "test_daemon.h"
#include "test_util.h"

namespace expd {
namespace {

std::string g_dir;

// Publishes the way expd does: new file, rename, notification.
void publish(const std::vector<Assignment>& assignments, uint64_t generation, bool kill = false) {
  std::string error;
  ASSERT_TRUE(publish_snapshot(g_dir, assignments, generation, &error)) << error;
  announce_snapshot(kill);
}

std::string variant(const char* experiment) {
  char out[EXP_VARIANT_MAX];
  return exp_variant(experiment, out, sizeof out) == 0 ? out : "<none>";
}

// Refreshes until `generation` is mapped. notifyd delivers notifications asynchronously, so
// exp_refresh() may need a moment to see one.
bool refresh_until(uint64_t generation) {
  for (int i = 0; i < 200; ++i) {
    exp_refresh();
    if (exp_snapshot_generation() == generation) return true;
    usleep(10'000);
  }
  return false;
}

TEST(LibExp, NoSnapshotAndNoDaemonMeansDefault) {
  char out[EXP_VARIANT_MAX] = "untouched";
  EXPECT_EQ(exp_variant("new_search_ranking", out, sizeof out), -1);
  EXPECT_STREQ(out, "untouched");
  EXPECT_EQ(exp_snapshot_generation(), 0u);
  EXPECT_EQ(exp_variant(nullptr, out, sizeof out), -1);
}

TEST(LibExp, MapsTheSnapshotAtRefresh) {
  publish({{"new_search_ranking", "treatment"}, {"photo_grid", "on"}}, 1);
  ASSERT_TRUE(refresh_until(1));
  EXPECT_EQ(variant("new_search_ranking"), "treatment");
  EXPECT_EQ(variant("photo_grid"), "on");
  EXPECT_EQ(variant("missing"), "<none>");
}

TEST(LibExp, KeepsTheSessionsSnapshotUntilRefresh) {
  publish({{"new_search_ranking", "control"}, {"photo_grid", "on"}}, 2);
  // No refresh yet: the app keeps reading the snapshot it mapped, so nothing changes mid-session.
  EXPECT_EQ(variant("new_search_ranking"), "treatment");
  ASSERT_TRUE(refresh_until(2));
  EXPECT_EQ(variant("new_search_ranking"), "control");
  EXPECT_EQ(exp_refresh(), 0);  // nothing new
}

TEST(LibExp, KeepsTheOldSnapshotWhenTheNewOneIsCorrupt) {
  std::string error;
  ASSERT_TRUE(write_file_atomically(g_dir + "/snapshot", "EXPS garbage", 0644, &error));
  announce_snapshot(false);
  for (int i = 0; i < 20; ++i) {
    EXPECT_EQ(exp_refresh(), 0);
    usleep(5'000);
  }
  EXPECT_EQ(exp_snapshot_generation(), 2u);
  EXPECT_EQ(variant("new_search_ranking"), "control");
}

TEST(LibExp, BufferTooSmallFails) {
  char out[4] = "abc";
  EXPECT_EQ(exp_variant("new_search_ranking", out, sizeof out), -1);  // "control" needs 8
  EXPECT_STREQ(out, "abc");
}

#ifdef __APPLE__
TEST(LibExp, KillSwitchRemapsWithoutRefresh) {
  publish({{"photo_grid", "on"}}, 3, /*kill=*/true);  // new_search_ranking turned off
  bool off = false;
  for (int i = 0; i < 200 && !off; ++i) {
    off = variant("new_search_ranking") == "<none>";
    if (!off) usleep(10'000);
  }
  EXPECT_TRUE(off);
  EXPECT_EQ(exp_snapshot_generation(), 3u);
}
#endif

// Run under ThreadSanitizer: readers race with remaps and must only ever see a whole snapshot.
TEST(LibExp, LookupsRaceWithRemaps) {
  publish({{"a", "x"}, {"b", "y"}}, 10);
  ASSERT_TRUE(refresh_until(10));

  std::atomic<bool> done{false};
  std::atomic<int> bad{0};
  std::vector<std::thread> readers;
  for (int t = 0; t < 4; ++t) {
    readers.emplace_back([&] {
      while (!done.load()) {
        std::string a = variant("a");
        if ((a != "x" && a != "z") || variant("b") != "y") bad.fetch_add(1);
      }
    });
  }
  int remaps = 0;
  for (uint64_t generation = 11; generation < 41; ++generation) {
    publish({{"a", generation % 2 ? "z" : "x"}, {"b", "y"}}, generation);
    remaps += exp_refresh();
  }
  done.store(true);
  for (std::thread& t : readers) t.join();
  EXPECT_EQ(bad.load(), 0);
  EXPECT_GT(remaps, 0);  // the readers really did race with pointer swaps
}

TEST(LibExp, SlowPathAsksExpd) {
  TestDaemon daemon(g_dir, "new_search_ranking k3f9 control:5000 treatment:5000\n");
  char out[EXP_VARIANT_MAX];
  ASSERT_EQ(exp_internal_variant_via_socket("new_search_ranking", out, sizeof out), 0);
  EXPECT_STREQ(out, "control");
  EXPECT_EQ(exp_internal_variant_via_socket("missing", out, sizeof out), -1);
}

}  // namespace
}  // namespace expd

int main(int argc, char** argv) {
  // Set before libexp's first call, which is when it reads EXP_STATE_DIR.
  static TempDir dir;
  expd::g_dir = dir.path();
  setenv("EXP_STATE_DIR", dir.path().c_str(), 1);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
