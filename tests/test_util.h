#pragma once

#include <stdlib.h>
#include <unistd.h>

#include <filesystem>
#include <string>

// A fresh directory under $TMPDIR, removed when the object goes away.
class TempDir {
 public:
  TempDir() {
    std::string pattern = (std::filesystem::temp_directory_path() / "exptest.XXXXXX").string();
    if (mkdtemp(pattern.data()) != nullptr) path_ = pattern;
  }
  ~TempDir() {
    std::error_code ignored;
    if (!path_.empty()) std::filesystem::remove_all(path_, ignored);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const std::string& path() const { return path_; }
  std::string file(const std::string& name) const { return path_ + "/" + name; }

 private:
  std::string path_;
};
