#include "zarr/analysis_crop_geometry_repository.h"
#include "zarr/archive_context.h"
#include "zarr/tensorstore_analysis_crop_geometry_repository.h"

#include <tensorstore/open.h>
#include <tensorstore/tensorstore.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

namespace ts = tensorstore;
using Clock = std::chrono::steady_clock;
using json = nlohmann::json;

struct Options {
  std::filesystem::path store;
  std::string mask_run;
  std::string crop_run;
};

bool ParseOptions(int argc, char **argv, Options *options) {
  if (!options) {
    return false;
  }
  for (int index = 1; index < argc; ++index) {
    if (index + 1 >= argc) {
      return false;
    }
    const std::string argument = argv[index];
    const std::string value = argv[++index];
    if (argument == "--store") {
      options->store = value;
    } else if (argument == "--mask-run") {
      options->mask_run = value;
    } else if (argument == "--crop-run") {
      options->crop_run = value;
    } else {
      return false;
    }
  }
  return !options->store.empty() && !options->mask_run.empty() &&
         !options->crop_run.empty();
}

std::string FileRoot(const std::filesystem::path &root) {
  std::string value =
      std::filesystem::absolute(root).lexically_normal().string();
  if (value.empty() || value.back() != '/') {
    value.push_back('/');
  }
  return value;
}

template <typename T, ts::DimensionIndex Rank>
std::optional<ts::TensorStore<T, Rank>>
OpenExact(const std::filesystem::path &root, const std::string &path,
          std::string *error) {
  json spec = {{"driver", "zarr3"},
               {"kvstore", {{"driver", "file"}, {"path", FileRoot(root)}}},
               {"path", path},
               {"recheck_cached_metadata", "open"},
               {"recheck_cached_data", "open"}};
  auto opened =
      ts::Open<T, Rank>(spec, ts::OpenMode::open, ts::ReadWriteMode::read)
          .result();
  if (!opened.ok()) {
    if (error) {
      *error = path + ": " + opened.status().ToString();
    }
    return std::nullopt;
  }
  return std::move(*opened);
}

template <typename T>
bool ReadVector(const ts::TensorStore<T, 1> &store, std::vector<T> *output,
                std::string *error) {
  auto read = ts::Read(store).result();
  if (!read.ok() || read->rank() != 1 || read->byte_strides().size() != 1) {
    if (error) {
      *error = read.ok() ? "Vector read has an invalid rank or stride"
                         : read.status().ToString();
    }
    return false;
  }
  const size_t size = static_cast<size_t>(read->shape()[0]);
  output->resize(size);
  const auto *origin = reinterpret_cast<const uint8_t *>(
      read->byte_strided_origin_pointer().get());
  for (size_t index = 0; index < size; ++index) {
    (*output)[index] = *reinterpret_cast<const T *>(
        origin + static_cast<ts::Index>(index) * read->byte_strides()[0]);
  }
  return true;
}

bool ReadFloat4(const ts::TensorStore<float, 2> &store,
                std::vector<std::array<float, 4>> *output, std::string *error) {
  auto read = ts::Read(store).result();
  if (!read.ok() || read->rank() != 2 || read->shape()[1] != 4 ||
      read->byte_strides().size() != 2) {
    if (error) {
      *error = read.ok() ? "Placement read has an invalid shape or stride"
                         : read.status().ToString();
    }
    return false;
  }
  const size_t rows = static_cast<size_t>(read->shape()[0]);
  output->resize(rows);
  const auto *origin = reinterpret_cast<const uint8_t *>(
      read->byte_strided_origin_pointer().get());
  for (size_t row = 0; row < rows; ++row) {
    for (size_t column = 0; column < 4; ++column) {
      (*output)[row][column] = *reinterpret_cast<const float *>(
          origin + static_cast<ts::Index>(row) * read->byte_strides()[0] +
          static_cast<ts::Index>(column) * read->byte_strides()[1]);
    }
  }
  return true;
}

bool SameFloat(float lhs, float rhs) {
  return std::memcmp(&lhs, &rhs, sizeof(float)) == 0;
}

double ElapsedMs(Clock::time_point started) {
  return std::chrono::duration<double, std::milli>(Clock::now() - started)
      .count();
}

} // namespace

int main(int argc, char **argv) {
  Options options;
  if (!ParseOptions(argc, argv, &options)) {
    std::cerr << "Usage: subject_mask_crop_join_probe --store PATH "
                 "--mask-run RUN --crop-run RUN\n";
    return 2;
  }

  const auto started = Clock::now();
  std::string error;
  auto archive = crimson::zarr::ArchiveContext::Open(options.store, &error);
  auto crop = archive ? crimson::zarr::OpenAnalysisCropGeometryRepository(
                            archive, options.crop_run, &error)
                      : nullptr;
  const bool crop_valid = crop != nullptr;
  const size_t expected_rows = crop_valid ? crop->descriptor().row_count : 0;
  const size_t expected_frames =
      crop_valid ? crop->descriptor().camera_frame_count : 0;
  crop.reset();

  const std::string mask_base =
      "refined_subject_masks_runs/" + options.mask_run;
  const std::string crop_base = "crop_runs/" + options.crop_run;
  auto source_rows = OpenExact<int64_t, 1>(
      options.store, mask_base + "/source_crop_row_ids", &error);
  auto mask_keys = OpenExact<uint64_t, 1>(options.store,
                                          mask_base + "/instance_key", &error);
  auto mask_placements = OpenExact<float, 2>(
      options.store, mask_base + "/source_crop_xywh", &error);
  auto crop_keys = OpenExact<uint64_t, 1>(options.store,
                                          crop_base + "/instance_key", &error);
  auto crop_placements = OpenExact<float, 2>(
      options.store, crop_base + "/source_crop_xywh", &error);

  std::vector<int64_t> row_ids;
  std::vector<uint64_t> mask_key_values;
  std::vector<uint64_t> crop_key_values;
  std::vector<std::array<float, 4>> mask_placement_values;
  std::vector<std::array<float, 4>> crop_placement_values;
  bool read_valid = crop_valid && source_rows && mask_keys && mask_placements &&
                    crop_keys && crop_placements;
  if (read_valid) {
    read_valid = ReadVector(*source_rows, &row_ids, &error) &&
                 ReadVector(*mask_keys, &mask_key_values, &error) &&
                 ReadFloat4(*mask_placements, &mask_placement_values, &error) &&
                 ReadVector(*crop_keys, &crop_key_values, &error) &&
                 ReadFloat4(*crop_placements, &crop_placement_values, &error);
  }

  size_t key_mismatches = 0;
  size_t placement_mismatches = 0;
  size_t out_of_range_rows = 0;
  size_t non_identity_rows = 0;
  if (read_valid && row_ids.size() == expected_rows &&
      mask_key_values.size() == expected_rows &&
      mask_placement_values.size() == expected_rows) {
    for (size_t row = 0; row < expected_rows; ++row) {
      const int64_t source_row = row_ids[row];
      if (source_row < 0 ||
          static_cast<uint64_t>(source_row) >= crop_key_values.size() ||
          static_cast<uint64_t>(source_row) >= crop_placement_values.size()) {
        ++out_of_range_rows;
        continue;
      }
      if (static_cast<size_t>(source_row) != row) {
        ++non_identity_rows;
      }
      const size_t source = static_cast<size_t>(source_row);
      if (mask_key_values[row] != crop_key_values[source]) {
        ++key_mismatches;
      }
      for (size_t column = 0; column < 4; ++column) {
        if (!SameFloat(mask_placement_values[row][column],
                       crop_placement_values[source][column])) {
          ++placement_mismatches;
          break;
        }
      }
    }
  } else if (read_valid) {
    error = "Mask/crop compact-column row counts differ";
    read_valid = false;
  }

  const bool valid = read_valid && out_of_range_rows == 0 &&
                     key_mismatches == 0 && placement_mismatches == 0;
  const json result = {
      {"schema_id", "crimson.subject_mask.crop_join_probe"},
      {"schema_version", 1},
      {"status", valid ? "pass" : "fail"},
      {"store", options.store.string()},
      {"mask_run", options.mask_run},
      {"crop_run", options.crop_run},
      {"frame_count", expected_frames},
      {"row_count", expected_rows},
      {"source_crop_row_ids_read", row_ids.size()},
      {"instance_keys_compared", mask_key_values.size()},
      {"placements_compared", mask_placement_values.size()},
      {"out_of_range_rows", out_of_range_rows},
      {"non_identity_source_rows", non_identity_rows},
      {"instance_key_mismatches", key_mismatches},
      {"placement_mismatches", placement_mismatches},
      {"elapsed_ms", ElapsedMs(started)},
      {"error", error},
  };
  std::cout << result.dump(2) << '\n';
  return valid ? 0 : 1;
}
