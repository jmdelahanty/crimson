#include "recording_clip_index.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace {

using json = nlohmann::json;

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    const auto seed =
        std::chrono::steady_clock::now().time_since_epoch().count();
    for (int attempt = 0; attempt < 100; ++attempt) {
      path_ = std::filesystem::temp_directory_path() /
              ("crimson-recording-clips-" + std::to_string(seed) + "-" +
               std::to_string(attempt));
      std::error_code error;
      if (std::filesystem::create_directory(path_, error)) {
        return;
      }
    }
    throw std::runtime_error("Could not create temporary directory");
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      throw std::runtime_error(std::string("CHECK failed: ") + #condition +    \
                               " at " + __FILE__ + ":" +                       \
                               std::to_string(__LINE__));                      \
    }                                                                          \
  } while (false)

void WriteText(const std::filesystem::path &path, const std::string &value) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  output << value;
  CHECK(output.good());
}

json Clip(size_t index, int64_t start, int64_t stop) {
  const std::string id = "clip_" + std::to_string(index);
  return {{"clip_index", index},
          {"clip_id", id},
          {"recording_id", "recording_fixture"},
          {"camera_serial", "2010095"},
          {"video_path", "clips/" + id + "/camera.mov"},
          {"status", "materialized"},
          {"start_is_keyframe", true},
          {"final_clip", index == 1},
          {"actual_start_frame", start},
          {"end_frame_exclusive", stop},
          {"first_clip_local_frame_index", 0},
          {"last_clip_local_frame_index", stop - start - 1},
          {"frame_count", stop - start}};
}

json ValidIndex() {
  return {{"status", "ok"},
          {"mode", "materialized_stream_copy"},
          {"recording_id", "recording_fixture"},
          {"camera_serial", "2010095"},
          {"clip_count", 2},
          {"source", {{"total_frames", 5}, {"fps", 30.0}}},
          {"clips", {Clip(0, 0, 3), Clip(1, 3, 5)}},
          {"checks",
           {{{"code", "metadata_rows_match_keyframe_total_frames"},
             {"status", "ok"}},
            {{"code", "recording_frame_id_continuity"}, {"status", "ok"}},
            {{"code", "clip_start_is_keyframe"},
             {"clip_id", "clip_0"},
             {"status", "ok"}},
            {{"code", "clip_recording_frame_id_continuity"},
             {"clip_id", "clip_0"},
             {"status", "ok"}},
            {{"code", "clip_start_is_keyframe"},
             {"clip_id", "clip_1"},
             {"status", "ok"}},
            {{"code", "clip_recording_frame_id_continuity"},
             {"clip_id", "clip_1"},
             {"status", "ok"}}}}};
}

std::filesystem::path WriteFixture(const std::filesystem::path &root,
                                   const json &document,
                                   bool write_second_video = true) {
  WriteText(root / "clips/clip_0/camera.mov", "clip zero");
  if (write_second_video) {
    WriteText(root / "clips/clip_1/camera.mov", "clip one");
  }
  const auto index = root / "recording_clip_index.json";
  WriteText(index, document.dump(2) + '\n');
  return index;
}

bool Rejected(const std::filesystem::path &root, const json &document,
              bool write_second_video = true) {
  std::string error;
  return !crimson::media::RecordingClipIndex::Open(
              WriteFixture(root, document, write_second_video), &error)
              .has_value() &&
         !error.empty();
}

void TestValidMapping() {
  TemporaryDirectory temporary;
  std::string error;
  auto index = crimson::media::RecordingClipIndex::Open(
      WriteFixture(temporary.path(), ValidIndex()), &error);
  CHECK(index.has_value());
  CHECK(error.empty());
  CHECK(index->recordingId() == "recording_fixture");
  CHECK(index->cameraSerial() == "2010095");
  CHECK(index->totalFrameCount() == 5);
  CHECK(index->framesPerSecond() == 30.0);
  CHECK(index->clips().size() == 2);
  CHECK(index->clip(2) == nullptr);

  const auto zero = index->resolveParentFrame(0);
  const auto two = index->resolveParentFrame(2);
  const auto three = index->resolveParentFrame(3);
  const auto four = index->resolveParentFrame(4);
  CHECK(zero && zero->clip->index == 0 && zero->clip_local_frame == 0);
  CHECK(two && two->clip->index == 0 && two->clip_local_frame == 2);
  CHECK(three && three->clip->index == 1 && three->clip_local_frame == 0);
  CHECK(four && four->clip->index == 1 && four->clip_local_frame == 1);
  CHECK(!index->resolveParentFrame(-1));
  CHECK(!index->resolveParentFrame(5));
}

void TestMalformedIndexesFailClosed() {
  {
    TemporaryDirectory temporary;
    auto document = ValidIndex();
    document["clips"][1]["actual_start_frame"] = 4;
    CHECK(Rejected(temporary.path(), document));
  }
  {
    TemporaryDirectory temporary;
    auto document = ValidIndex();
    document["clips"][1]["actual_start_frame"] = 2;
    CHECK(Rejected(temporary.path(), document));
  }
  {
    TemporaryDirectory temporary;
    auto document = ValidIndex();
    document["clips"][1]["clip_id"] = "clip_0";
    CHECK(Rejected(temporary.path(), document));
  }
  {
    TemporaryDirectory temporary;
    auto document = ValidIndex();
    document["clips"][0]["start_is_keyframe"] = false;
    CHECK(Rejected(temporary.path(), document));
  }
  {
    TemporaryDirectory temporary;
    auto document = ValidIndex();
    document["clips"][0]["video_path"] = "../escape.mov";
    CHECK(Rejected(temporary.path(), document));
  }
  {
    TemporaryDirectory temporary;
    auto document = ValidIndex();
    document["source"]["total_frames"] = 6;
    CHECK(Rejected(temporary.path(), document));
  }
  {
    TemporaryDirectory temporary;
    auto document = ValidIndex();
    document["checks"][2]["status"] = "failed";
    CHECK(Rejected(temporary.path(), document));
  }
  {
    TemporaryDirectory temporary;
    CHECK(Rejected(temporary.path(), ValidIndex(), false));
  }
}

} // namespace

int main() {
  try {
    TestValidMapping();
    TestMalformedIndexesFailClosed();
    std::cout << "recording_clip_index_tests: PASS\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "recording_clip_index_tests: FAIL: " << error.what() << '\n';
    return 1;
  }
}
