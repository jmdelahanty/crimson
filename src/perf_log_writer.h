#pragma once

#include <chrono>
#include <filesystem>
#include <fstream>

struct PerfLogWriter {
  std::ofstream stream;
  std::filesystem::path csv_path;
  std::filesystem::path metadata_path;
  std::chrono::steady_clock::time_point start_steady{};
  std::chrono::steady_clock::time_point last_sample_steady{};

  bool open(const std::filesystem::path &output_path);
  bool enabled() const;
  void close();
};

struct MaskPerfLogWriter {
  std::ofstream stream;
  std::filesystem::path jsonl_path;
  int samples_since_flush = 0;
  std::chrono::steady_clock::time_point last_flush_steady{};

  bool open(const std::filesystem::path &output_path);
  bool enabled() const;
  void close();
};

std::filesystem::path
defaultMaskPerfLogPath(const std::filesystem::path &default_buffer_dump_root);
