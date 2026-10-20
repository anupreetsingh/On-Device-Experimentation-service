#include "assignment.h"

#include <gtest/gtest.h>
#include <sys/stat.h>

#include <string>

#include "device_id.h"
#include "sha256.h"
#include "test_util.h"

#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#endif

namespace expd {
namespace {

constexpr char kDevice[] = "6F1C2B7A-3D4E-4F50-9A6B-7C8D9E0F1A2B";

std::string hex(const std::array<uint8_t, 32>& digest) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  for (uint8_t b : digest) {
    out += digits[b >> 4];
    out += digits[b & 15];
  }
  return out;
}

Experiment split(const std::string& name, const std::string& salt, uint32_t treatment) {
  return {name, salt, {{"control", kBuckets - treatment}, {"treatment", treatment}}};
}

TEST(Sha256, KnownVectors) {
  EXPECT_EQ(hex(sha256("")), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(hex(sha256("abc")), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  // 56 bytes: the length no longer fits in the last block, so padding takes two.
  EXPECT_EQ(hex(sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

#ifdef __APPLE__
TEST(Sha256, MatchesCommonCryptoAtEveryLength) {
  std::string input;
  for (int len = 0; len < 300; ++len) {
    std::array<uint8_t, 32> expected;
    CC_SHA256(input.data(), CC_LONG(input.size()), expected.data());
    ASSERT_EQ(sha256(input), expected) << "length " << len;
    input.push_back(char('a' + len % 26));
  }
}
#endif

// Golden values, computed independently with Python's hashlib. If these change, every
// device in every experiment changes arm.
TEST(Bucketing, GoldenValues) {
  EXPECT_EQ(bucket_for("k3f9", "new_search_ranking", kDevice), 1673u);
  EXPECT_EQ(bucket_for("k3fa", "new_search_ranking", kDevice), 4016u);
  EXPECT_EQ(bucket_for("", "", ""), 8966u);
}

TEST(Bucketing, ArmsCoverConsecutiveRanges) {
  Experiment e = split("exp", "s", 3000);  // control [0, 7000), treatment [7000, 10000)
  EXPECT_EQ(arm_for(e, 0)->name, "control");
  EXPECT_EQ(arm_for(e, 6999)->name, "control");
  EXPECT_EQ(arm_for(e, 7000)->name, "treatment");
  EXPECT_EQ(arm_for(e, 9999)->name, "treatment");
  EXPECT_EQ(arm_for(e, 10000), nullptr);
}

TEST(Bucketing, TenPercentIsTenPercent) {
  Experiment e = split("new_search_ranking", "k3f9", 1000);
  int treatment = 0;
  constexpr int kDevices = 1'000'000;
  for (int i = 0; i < kDevices; ++i) {
    if (arm_for(e, bucket_for(e.salt, e.name, "device-" + std::to_string(i)))->name == "treatment") {
      ++treatment;
    }
  }
  // Within 0.1 percentage points of 10%.
  EXPECT_NEAR(treatment, kDevices / 10, kDevices / 1000);
}

TEST(Bucketing, ExperimentsAreIndependent) {
  // If bucketing ignored the experiment, the same devices would be in treatment in both.
  Experiment a = split("exp_a", "s1", 5000);
  Experiment b = split("exp_b", "s2", 5000);
  int both = 0;
  constexpr int kDevices = 200'000;
  for (int i = 0; i < kDevices; ++i) {
    std::string id = "device-" + std::to_string(i);
    bool in_a = arm_for(a, bucket_for(a.salt, a.name, id))->name == "treatment";
    bool in_b = arm_for(b, bucket_for(b.salt, b.name, id))->name == "treatment";
    if (in_a && in_b) ++both;
  }
  EXPECT_NEAR(both, kDevices / 4, kDevices / 200);
}

TEST(Bucketing, RampingUpKeepsDevicesInTreatment) {
  Experiment at10 = split("exp", "s", 1000);
  Experiment at50 = split("exp", "s", 5000);
  for (int i = 0; i < 100'000; ++i) {
    uint32_t bucket = bucket_for("s", "exp", "device-" + std::to_string(i));
    if (arm_for(at10, bucket)->name == "treatment") {
      ASSERT_EQ(arm_for(at50, bucket)->name, "treatment") << "bucket " << bucket;
    }
  }
}

TEST(Bucketing, NewSaltReshuffles) {
  Experiment before = split("exp", "k3f9", 5000);
  Experiment after = split("exp", "k3fa", 5000);
  int moved = 0;
  constexpr int kDevices = 100'000;
  for (int i = 0; i < kDevices; ++i) {
    std::string id = "device-" + std::to_string(i);
    if (arm_for(before, bucket_for(before.salt, before.name, id))->name !=
        arm_for(after, bucket_for(after.salt, after.name, id))->name) {
      ++moved;
    }
  }
  EXPECT_NEAR(moved, kDevices / 2, kDevices / 20);
}

TEST(Assign, GivesOneVariantPerExperimentSortedByName) {
  std::vector<Experiment> experiments = {split("zeta", "s", 0), split("alpha", "s", 10000)};
  std::vector<Assignment> got = assign(experiments, kDevice);
  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(got[0].experiment, "alpha");
  EXPECT_EQ(got[0].variant, "treatment");
  EXPECT_EQ(got[1].experiment, "zeta");
  EXPECT_EQ(got[1].variant, "control");
}

TEST(Config, ParsesExperimentsAndComments) {
  std::vector<Experiment> experiments;
  std::string error;
  ASSERT_TRUE(parse_config(
      "# experiment  salt  arms\n"
      "new_search_ranking k3f9 control:5000 treatment:5000\n"
      "\n"
      "   photo_grid  a1  off:9000 on:1000   # 10% rollout\n"
      "three_way s c:3334 b:3333 a:3333",
      &experiments, &error))
      << error;
  ASSERT_EQ(experiments.size(), 3u);
  EXPECT_EQ(experiments[0].name, "new_search_ranking");
  EXPECT_EQ(experiments[0].salt, "k3f9");
  EXPECT_EQ(experiments[1].arms[1].name, "on");
  EXPECT_EQ(experiments[1].arms[1].weight, 1000u);
  EXPECT_EQ(experiments[2].arms.size(), 3u);
}

TEST(Config, EmptyConfigHasNoExperiments) {
  std::vector<Experiment> experiments = {split("old", "s", 1)};
  std::string error;
  ASSERT_TRUE(parse_config("# nothing running\n", &experiments, &error));
  EXPECT_TRUE(experiments.empty());
}

TEST(Config, RejectsBadLines) {
  const char* bad[] = {
      "exp s control:5000 treatment:4999",                // doesn't add up to 10000
      "exp s control:6000 treatment:6000",                // more than 10000
      "exp s control:5000 control:5000",                  // arm twice
      "exp s control 10000",                              // no colon
      "exp s control:ten",                                // not a number
      "exp s control:-1 treatment:10001",                 // negative
      "exp s",                                            // no arms
      "bad/name s control:10000",                         // bad character
      "exp s variant_name_too_long:10000",                // arm over 15 characters
      "exp s a:10000\nexp t a:10000",                     // experiment twice
  };
  for (const char* text : bad) {
    std::vector<Experiment> experiments;
    std::string error;
    EXPECT_FALSE(parse_config(text, &experiments, &error)) << text;
    EXPECT_FALSE(error.empty()) << text;
  }
}

TEST(Config, ErrorNamesTheLine) {
  std::vector<Experiment> experiments;
  std::string error;
  EXPECT_FALSE(parse_config("ok s a:10000\n\nbroken s a:1\n", &experiments, &error));
  EXPECT_EQ(error.rfind("line 3:", 0), 0u) << error;
}

TEST(DeviceId, CreatedOnceThenReused) {
  TempDir dir;
  std::string error;
  std::string first = load_or_create_device_id(dir.file("device_id"), &error);
  ASSERT_EQ(first.size(), 36u) << error;
  EXPECT_EQ(first[14], '4');  // version 4 UUID
  EXPECT_EQ(load_or_create_device_id(dir.file("device_id"), &error), first);

  struct stat st;
  ASSERT_EQ(stat(dir.file("device_id").c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0600u);
}

}  // namespace
}  // namespace expd
