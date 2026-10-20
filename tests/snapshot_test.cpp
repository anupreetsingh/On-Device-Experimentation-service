#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>

#include "files.h"
#include "snapshot_format.h"
#include "snapshot_writer.h"
#include "test_util.h"

namespace expd {
namespace {

const std::vector<Assignment> kAssignments = {
    {"new_search_ranking", "treatment"},
    {"photo_grid", "on"},
    {"dark_mode_banner", "control"},
};

const snapshot::Entry* check(const std::string& bytes, uint32_t* count, uint64_t* generation) {
  return snapshot::validate(bytes.data(), bytes.size(), count, generation);
}

TEST(Snapshot, RoundTrip) {
  std::string bytes = encode_snapshot(kAssignments, 7);
  ASSERT_EQ(bytes.size(), 24u + 3 * 64u);

  uint32_t count = 0;
  uint64_t generation = 0;
  const snapshot::Entry* entries = check(bytes, &count, &generation);
  ASSERT_NE(entries, nullptr);
  EXPECT_EQ(count, 3u);
  EXPECT_EQ(generation, 7u);
  EXPECT_STREQ(entries[0].experiment, "dark_mode_banner");  // sorted

  for (const Assignment& a : kAssignments) {
    const snapshot::Entry* e = snapshot::find(entries, count, a.experiment.c_str());
    ASSERT_NE(e, nullptr) << a.experiment;
    EXPECT_EQ(e->variant, a.variant);
  }
  EXPECT_EQ(snapshot::find(entries, count, "missing"), nullptr);
  EXPECT_EQ(snapshot::find(entries, count, ""), nullptr);
}

TEST(Snapshot, EmptySnapshotIsValid) {
  std::string bytes = encode_snapshot({}, 1);
  uint32_t count = 99;
  uint64_t generation = 0;
  ASSERT_NE(check(bytes, &count, &generation), nullptr);
  EXPECT_EQ(count, 0u);
}

TEST(Snapshot, RejectsDamagedFiles) {
  const std::string good = encode_snapshot(kAssignments, 1);
  uint32_t count;
  uint64_t generation;

  std::string flipped = good;
  flipped[24 + 70] ^= 1;  // one bit in the second entry
  EXPECT_EQ(check(flipped, &count, &generation), nullptr) << "checksum";

  EXPECT_EQ(check(good.substr(0, good.size() - 1), &count, &generation), nullptr) << "truncated";
  EXPECT_EQ(check(good.substr(0, good.size() - 64), &count, &generation), nullptr) << "short";
  EXPECT_EQ(check(good + std::string(64, '\0'), &count, &generation), nullptr) << "long";
  EXPECT_EQ(check(good.substr(0, 10), &count, &generation), nullptr) << "no header";

  std::string magic = good;
  magic[0] = 'X';
  EXPECT_EQ(check(magic, &count, &generation), nullptr) << "magic";

  std::string version = good;
  version[4] = 2;
  EXPECT_EQ(check(version, &count, &generation), nullptr) << "version";
}

TEST(Snapshot, RejectsUnsortedEntries) {
  std::string bytes = encode_snapshot({{"a", "x"}, {"b", "y"}}, 1);
  // Swap the two entries and fix the checksum, so only the order is wrong.
  std::string first = bytes.substr(24, 64);
  bytes.replace(24, 64, bytes.substr(88, 64));
  bytes.replace(88, 64, first);
  uint32_t checksum = snapshot::fnv1a(bytes.data() + 24, 128);
  std::memcpy(bytes.data() + 12, &checksum, sizeof checksum);

  uint32_t count;
  uint64_t generation;
  EXPECT_EQ(check(bytes, &count, &generation), nullptr);
}

TEST(Snapshot, PublishReplacesTheFileAndLeavesNoTemp) {
  TempDir dir;
  std::string error;
  EXPECT_EQ(current_generation(dir.path()), 0u);
  ASSERT_TRUE(publish_snapshot(dir.path(), kAssignments, 1, &error)) << error;
  ASSERT_TRUE(publish_snapshot(dir.path(), kAssignments, 2, &error)) << error;
  EXPECT_EQ(current_generation(dir.path()), 2u);
  EXPECT_NE(access(dir.file("snapshot.tmp").c_str(), F_OK), 0);

  struct stat st;
  ASSERT_EQ(stat(dir.file("snapshot").c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0644u);
}

// What makes the rename swap safe: an app that mapped the old file keeps reading the old
// data after expd publishes, and nothing it reads is ever half-written.
TEST(Snapshot, OldMappingSurvivesPublish) {
  TempDir dir;
  std::string error;
  ASSERT_TRUE(publish_snapshot(dir.path(), {{"exp", "control"}}, 1, &error)) << error;

  int fd = open(dir.file("snapshot").c_str(), O_RDONLY);
  ASSERT_GE(fd, 0);
  struct stat st;
  ASSERT_EQ(fstat(fd, &st), 0);
  void* old_map = mmap(nullptr, size_t(st.st_size), PROT_READ, MAP_SHARED, fd, 0);
  close(fd);
  ASSERT_NE(old_map, MAP_FAILED);

  ASSERT_TRUE(publish_snapshot(dir.path(), {{"exp", "treatment"}}, 2, &error)) << error;

  uint32_t count;
  uint64_t generation;
  const snapshot::Entry* entries = snapshot::validate(old_map, size_t(st.st_size), &count, &generation);
  ASSERT_NE(entries, nullptr);
  EXPECT_EQ(generation, 1u);
  EXPECT_STREQ(snapshot::find(entries, count, "exp")->variant, "control");
  munmap(old_map, size_t(st.st_size));

  EXPECT_EQ(current_generation(dir.path()), 2u);
}

}  // namespace
}  // namespace expd
