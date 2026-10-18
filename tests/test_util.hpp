// Unique scratch paths so tests can run in parallel processes (ctest -j) without sharing files.
#pragma once

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#ifdef _WIN32
#include <process.h>
#define AQO_GETPID _getpid
#else
#include <unistd.h>
#define AQO_GETPID getpid
#endif

namespace aqo::testing {

inline std::string unique_path(const std::string& tag, const std::string& ext) {
  std::string name = "aqo_" + tag;
  if (const auto* info = ::testing::UnitTest::GetInstance()->current_test_info()) {
    name += std::string("_") + info->test_suite_name() + "_" + info->name();
  }
  for (char& c : name)
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') c = '_';
  return (std::filesystem::temp_directory_path() / (name + "_" + std::to_string(AQO_GETPID()) + ext)).string();
}

}  // namespace aqo::testing
