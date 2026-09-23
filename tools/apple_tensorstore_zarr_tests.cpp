#include <filesystem>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>
#include <tensorstore/context.h>
#include <tensorstore/open.h>
#include <tensorstore/spec.h>

namespace {

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    const auto base = std::filesystem::temp_directory_path();
    for (int attempt = 0; attempt < 100; ++attempt) {
      path_ = base / ("crimson-tensorstore-zarr-" +
                      std::to_string(attempt));
      std::error_code error;
      if (std::filesystem::create_directory(path_, error)) {
        return;
      }
    }
    path_.clear();
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

nlohmann::json Zarr2Metadata() {
  return {
      {"chunks", {2}},       {"compressor", nullptr},
      {"dtype", "|u1"},     {"fill_value", 0},
      {"filters", nullptr},  {"order", "C"},
      {"shape", {2}},
  };
}

nlohmann::json Zarr3Metadata() {
  return {
      {"shape", {2}},
      {"data_type", "uint8"},
      {"chunk_grid",
       {{"name", "regular"}, {"configuration", {{"chunk_shape", {2}}}}}},
      {"chunk_key_encoding", {{"name", "default"}}},
      {"codecs", {{{"name", "bytes"}}}},
      {"fill_value", 0},
  };
}

bool OpenArray(const char* array_driver, const char* kvstore_driver,
               const std::filesystem::path& path) {
  nlohmann::json kvstore = {{"driver", kvstore_driver}};
  if (kvstore_driver == std::string("file")) {
    kvstore["path"] = path.string() + "/";
  }

  nlohmann::json spec = {
      {"driver", array_driver}, {"kvstore", std::move(kvstore)},
      {"metadata", std::string(array_driver) == "zarr" ? Zarr2Metadata()
                                                        : Zarr3Metadata()},
      {"create", true},         {"open", true},
  };

  auto parsed_spec = tensorstore::Spec::FromJson(spec);
  if (!parsed_spec.ok()) {
    std::cerr << array_driver << "/" << kvstore_driver
              << " spec parse failed: " << parsed_spec.status() << '\n';
    return false;
  }

  auto store = tensorstore::Open(*parsed_spec, tensorstore::Context::Default())
                   .result();
  if (!store.ok()) {
    std::cerr << array_driver << "/" << kvstore_driver
              << " open failed: " << store.status() << '\n';
    return false;
  }
  if (store->rank() != 1 || store->domain().shape()[0] != 2) {
    std::cerr << array_driver << "/" << kvstore_driver
              << " returned an unexpected domain\n";
    return false;
  }
  if (kvstore_driver == std::string("file")) {
    const auto metadata_file =
        path / (std::string(array_driver) == "zarr" ? ".zarray" : "zarr.json");
    if (!std::filesystem::is_regular_file(metadata_file)) {
      std::cerr << array_driver << "/file did not persist " << metadata_file
                << '\n';
      return false;
    }
  }
  return true;
}

}  // namespace

int main() {
  TemporaryDirectory temporary_directory;
  if (temporary_directory.path().empty()) {
    std::cerr << "Unable to create a temporary test directory\n";
    return 1;
  }

  bool passed = true;
  for (const char* array_driver : {"zarr", "zarr3"}) {
    passed &= OpenArray(array_driver, "memory", {});
    passed &= OpenArray(array_driver, "file",
                        temporary_directory.path() / array_driver);
  }

  if (!passed) {
    return 1;
  }
  std::cout << "TensorStore Zarr v2/v3 memory/file capability: PASS\n";
  return 0;
}
