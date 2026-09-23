#include "recording_path_resolution.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

namespace {

void Require(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    const auto base = fs::temp_directory_path();
    for (int attempt = 0; attempt < 100; ++attempt) {
      path_ = base / ("crimson-recording-path-" + std::to_string(attempt));
      std::error_code error;
      if (fs::create_directory(path_, error)) {
        return;
      }
    }
    throw std::runtime_error("could not create temporary directory");
  }

  ~TemporaryDirectory() {
    std::error_code error;
    fs::remove_all(path_, error);
  }

  const fs::path &path() const { return path_; }

private:
  fs::path path_;
};

void TestRecordingRootInference() {
  Require(crimson::media::InferRecordingRootFromArchive(
              "/mounted/recording/zarr/analysis.zarr") ==
              fs::path("/mounted/recording"),
          "zarr archive did not infer its recording root");
  Require(crimson::media::InferRecordingRootFromArchive(
              "/mounted/recording/analysis.zarr") ==
              fs::path("/mounted/recording"),
          "direct archive did not infer its recording root");
  Require(crimson::media::InferRecordingRootFromArchive({}).empty(),
          "empty archive path produced a recording root");
}

void TestStoredPathResolution() {
  TemporaryDirectory temporary;
  const fs::path recording_root = temporary.path() / "recording-identity";
  fs::create_directories(recording_root / "cams");

  auto relative = crimson::media::ResolveStoredRecordingPath(recording_root,
                                                             "cams/main.mp4");
  Require(relative.kind ==
              crimson::media::RecordingPathResolutionKind::RecordingRelative,
          "relative path has the wrong resolution kind");
  Require(relative.resolved_path == recording_root / "cams/main.mp4",
          "relative path resolved outside the recording root");

  const fs::path existing = recording_root / "cams/existing.mp4";
  {
    std::ofstream stream(existing);
    stream << "fixture";
  }
  auto absolute =
      crimson::media::ResolveStoredRecordingPath(recording_root, existing);
  Require(absolute.kind ==
              crimson::media::RecordingPathResolutionKind::StoredAbsolute,
          "existing absolute path has the wrong resolution kind");
  Require(absolute.resolved_path == existing,
          "existing absolute path was changed");

  auto relocated = crimson::media::ResolveStoredRecordingPath(
      recording_root, "/groups/lab/recording-identity/cams/relocated.mp4");
  Require(relocated.kind ==
              crimson::media::RecordingPathResolutionKind::RelocatedAbsolute,
          "foreign absolute path was not classified as relocated");
  Require(relocated.resolved_path == recording_root / "cams/relocated.mp4",
          "foreign absolute path retained the wrong mount prefix");

  const fs::path unrelated = "/groups/lab/other-recording/cams/main.mp4";
  auto unresolved =
      crimson::media::ResolveStoredRecordingPath(recording_root, unrelated);
  Require(unresolved.kind ==
              crimson::media::RecordingPathResolutionKind::UnresolvedAbsolute,
          "unrelated absolute path was unexpectedly relocated");
  Require(unresolved.resolved_path == unrelated,
          "unrelated absolute path was changed");

  auto empty = crimson::media::ResolveStoredRecordingPath(recording_root, {});
  Require(empty.kind == crimson::media::RecordingPathResolutionKind::Empty &&
              empty.resolved_path.empty(),
          "empty path did not remain empty");
}

} // namespace

int main() {
  try {
    TestRecordingRootInference();
    TestStoredPathResolution();
    std::cout << "recording_path_resolution_tests: PASS\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "recording_path_resolution_tests: FAIL: " << error.what()
              << '\n';
    return 1;
  }
}
