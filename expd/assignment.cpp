#include "assignment.h"

#include <algorithm>
#include <charconv>

#include "sha256.h"

namespace expd {
namespace {

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

std::vector<std::string_view> split_whitespace(std::string_view line) {
  std::vector<std::string_view> tokens;
  size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && is_space(line[i])) ++i;
    size_t start = i;
    while (i < line.size() && !is_space(line[i])) ++i;
    if (i > start) tokens.push_back(line.substr(start, i - start));
  }
  return tokens;
}

}  // namespace

uint32_t bucket_for(std::string_view salt, std::string_view experiment, std::string_view device_id) {
  std::string input;
  input.reserve(salt.size() + experiment.size() + device_id.size() + 2);
  input.append(salt);
  input.push_back('\0');
  input.append(experiment);
  input.push_back('\0');
  input.append(device_id);

  // The first 8 bytes of the digest as a big-endian number. 2^64 is so much larger than
  // 10000 that the modulo's bias is far too small to matter.
  std::array<uint8_t, 32> digest = sha256(input);
  uint64_t x = 0;
  for (int i = 0; i < 8; ++i) x = (x << 8) | digest[i];
  return static_cast<uint32_t>(x % kBuckets);
}

const Arm* arm_for(const Experiment& experiment, uint32_t bucket) {
  uint32_t end = 0;
  for (const Arm& arm : experiment.arms) {
    end += arm.weight;
    if (bucket < end) return &arm;
  }
  return nullptr;
}

std::vector<Assignment> assign(const std::vector<Experiment>& experiments, std::string_view device_id) {
  std::vector<Assignment> assignments;
  assignments.reserve(experiments.size());
  for (const Experiment& e : experiments) {
    if (const Arm* arm = arm_for(e, bucket_for(e.salt, e.name, device_id))) {
      assignments.push_back({e.name, arm->name});
    }
  }
  std::sort(assignments.begin(), assignments.end(),
            [](const Assignment& a, const Assignment& b) { return a.experiment < b.experiment; });
  return assignments;
}

bool valid_name(std::string_view name, size_t max_len) {
  if (name.empty() || name.size() > max_len) return false;
  return std::all_of(name.begin(), name.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '-' || c == '.';
  });
}

bool parse_config(std::string_view text, std::vector<Experiment>* out, std::string* error) {
  std::vector<Experiment> experiments;
  size_t line_no = 0;
  while (!text.empty()) {
    size_t newline = text.find('\n');
    std::string_view line = text.substr(0, newline);
    text = newline == std::string_view::npos ? std::string_view() : text.substr(newline + 1);
    ++line_no;
    auto fail = [&](const std::string& message) {
      *error = "line " + std::to_string(line_no) + ": " + message;
      return false;
    };

    std::vector<std::string_view> tokens = split_whitespace(line.substr(0, line.find('#')));
    if (tokens.empty()) continue;
    if (tokens.size() < 3) return fail("expected <experiment> <salt> <arm>:<weight> ...");

    Experiment experiment{std::string(tokens[0]), std::string(tokens[1]), {}};
    if (!valid_name(experiment.name, kMaxExperimentName)) {
      return fail("bad experiment name '" + experiment.name + "'");
    }
    if (!valid_name(experiment.salt, kMaxSalt)) return fail("bad salt '" + experiment.salt + "'");
    for (const Experiment& seen : experiments) {
      if (seen.name == experiment.name) return fail("experiment '" + seen.name + "' is listed twice");
    }

    uint32_t total = 0;
    for (size_t i = 2; i < tokens.size(); ++i) {
      std::string_view token = tokens[i];
      size_t colon = token.find(':');
      if (colon == std::string_view::npos) {
        return fail("expected <arm>:<weight>, got '" + std::string(token) + "'");
      }
      Arm arm{std::string(token.substr(0, colon)), 0};
      std::string_view weight = token.substr(colon + 1);
      auto [end, ec] = std::from_chars(weight.data(), weight.data() + weight.size(), arm.weight);
      if (weight.empty() || ec != std::errc() || end != weight.data() + weight.size()) {
        return fail("bad weight in '" + std::string(token) + "'");
      }
      if (!valid_name(arm.name, kMaxVariantName)) return fail("bad arm name '" + arm.name + "'");
      for (const Arm& seen : experiment.arms) {
        if (seen.name == arm.name) return fail("arm '" + arm.name + "' is listed twice");
      }
      if (arm.weight > kBuckets - total) return fail("weights add up to more than 10000");
      total += arm.weight;
      experiment.arms.push_back(std::move(arm));
    }
    if (total != kBuckets) return fail("weights add up to " + std::to_string(total) + ", not 10000");
    experiments.push_back(std::move(experiment));
  }
  *out = std::move(experiments);
  return true;
}

}  // namespace expd
