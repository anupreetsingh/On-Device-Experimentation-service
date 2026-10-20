#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "exposure_log.h"
#include "files.h"
#include "test_daemon.h"
#include "test_util.h"
#include "uds_client.h"

namespace expd {
namespace {

using namespace std::chrono_literals;

Exposure make(int64_t n) {
  Exposure e{};
  e.time = n;
  std::snprintf(e.experiment, sizeof e.experiment, "exp%lld", (long long)n);
  std::strcpy(e.variant, "control");
  return e;
}

size_t count_lines(const std::string& path) {
  std::string text;
  if (!read_file(path, &text)) return 0;
  return size_t(std::count(text.begin(), text.end(), '\n'));
}

TEST(RingBuffer, DrainsInOrder) {
  RingBuffer ring(4);
  for (int i = 1; i <= 3; ++i) ring.push(make(i));
  std::vector<Exposure> batch;
  ring.drain(&batch);
  ASSERT_EQ(batch.size(), 3u);
  for (int i = 0; i < 3; ++i) EXPECT_EQ(batch[size_t(i)].time, i + 1);
  EXPECT_EQ(ring.size(), 0u);
}

TEST(RingBuffer, OverwritesTheOldestWhenFullAndDrainsAcrossTheWrap) {
  RingBuffer ring(4);
  for (int i = 0; i < 6; ++i) ring.push(make(i));  // 0 and 1 overwritten; head at slot 2
  EXPECT_EQ(ring.dropped(), 2u);

  std::vector<Exposure> batch;
  ring.drain(&batch);  // slots 2, 3, then wraps to 0, 1
  ASSERT_EQ(batch.size(), 4u);
  for (int i = 0; i < 4; ++i) EXPECT_EQ(batch[size_t(i)].time, i + 2);
}

TEST(RingBuffer, ReportsHighWaterAndDrops) {
  RingBuffer ring(8);
  int high_water = 0;
  for (int i = 0; i < 8; ++i) {
    if (ring.push(make(i)) == RingBuffer::Push::kHighWater) ++high_water;
  }
  EXPECT_EQ(high_water, 3);  // the 6th, 7th and 8th push: 75% and up
  EXPECT_EQ(ring.push(make(99)), RingBuffer::Push::kDropped);
  EXPECT_EQ(ring.dropped(), 1u);
}

TEST(ExposureLog, StopFlushesWhatIsBuffered) {
  TempDir dir;
  ExposureLog log({dir.file("exposures.log"), 4096, 60s, 10s});
  std::string error;
  ASSERT_TRUE(log.start(&error)) << error;
  log.record("new_search_ranking", "treatment");
  log.record("photo_grid", "on");
  log.stop();

  std::string text;
  ASSERT_TRUE(read_file(dir.file("exposures.log"), &text));
  EXPECT_NE(text.find("\tnew_search_ranking\ttreatment\n"), std::string::npos) << text;
  EXPECT_NE(text.find("\tphoto_grid\ton\n"), std::string::npos) << text;
  EXPECT_EQ(log.stats().written, 2u);
}

TEST(ExposureLog, TimerFlushesWithoutStop) {
  TempDir dir;
  ExposureLog log({dir.file("exposures.log"), 4096, 20ms, 5ms});
  std::string error;
  ASSERT_TRUE(log.start(&error)) << error;
  log.record("exp", "control");
  for (int i = 0; i < 200 && log.stats().written == 0; ++i) std::this_thread::sleep_for(5ms);
  EXPECT_EQ(log.stats().written, 1u);
  log.stop();
}

TEST(ExposureLog, HighWaterFlushesEarly) {
  TempDir dir;
  ExposureLog log({dir.file("exposures.log"), 100, 60s, 10s});  // the timer won't fire
  std::string error;
  ASSERT_TRUE(log.start(&error)) << error;
  for (int i = 0; i < 80; ++i) log.record("exp", "control");
  for (int i = 0; i < 200 && log.stats().written == 0; ++i) std::this_thread::sleep_for(5ms);
  EXPECT_GE(log.stats().written, 75u);
  log.stop();
  EXPECT_EQ(count_lines(dir.file("exposures.log")), 80u);
}

// Run under ThreadSanitizer: the event loop pushes while the writer drains.
TEST(ExposureLog, ProducerRacesWithWriter) {
  TempDir dir;
  ExposureLog log({dir.file("exposures.log"), 256, 1ms, 0ms});
  std::string error;
  ASSERT_TRUE(log.start(&error)) << error;
  std::thread producer([&] {
    for (int i = 0; i < 20'000; ++i) log.record("exp", "control");
  });
  producer.join();
  log.stop();
  ExposureLog::Stats stats = log.stats();
  EXPECT_EQ(stats.recorded, 20'000u);
  EXPECT_EQ(stats.written + stats.dropped, 20'000u);
  EXPECT_EQ(count_lines(dir.file("exposures.log")), stats.written);
}

TEST(Server, ExposeIsRecordedWithoutAReply) {
  TempDir dir;
  TestDaemon daemon(dir.path(), "exp k3f9 control:5000 treatment:5000\n");
  int fd = uds::connect_to(daemon.socket_path());
  ASSERT_GE(fd, 0);
  ASSERT_TRUE(uds::send_frame(fd, "EXPOSE exp control"));
  ASSERT_TRUE(uds::send_frame(fd, "EXPOSE bad/name control"));  // ignored
  std::string reply;
  ASSERT_TRUE(uds::request(fd, "STATS", &reply));  // the first reply is STATS's: EXPOSE sent none
  EXPECT_NE(reply.find("exposures=1 "), std::string::npos) << reply;
  close(fd);

  daemon.stop();
  EXPECT_EQ(count_lines(dir.file("exposures.log")), 1u);
}

}  // namespace
}  // namespace expd
