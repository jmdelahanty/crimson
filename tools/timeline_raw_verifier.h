#pragma once

// Acceptance-only raw array comparison. Deliberately bypasses the production
// timeline repositories, selectors, frame-index cache and decimation helpers.
#include <tensorstore/box.h>
#include <tensorstore/cast.h>
#include <tensorstore/index_space/index_transform.h>
#include <tensorstore/open.h>
#include <tensorstore/tensorstore.h>
#include <algorithm>
#include <map>

class RawTimelineVerifier {
  using Store = tensorstore::TensorStore<double>;
  std::string root_;
  tensorstore::Context context_;
  std::map<std::string, Store> stores_;
  size_t comparisons_ = 0;
  Store& store(const std::string& path) {
    auto found = stores_.find(path);
    if (found != stores_.end()) return found->second;
    const nlohmann::json spec = {
        {"driver", "zarr3"},
        {"kvstore", {{"driver", "file"}, {"path", root_ + "/"}}},
        {"path", path}};
    auto opened = tensorstore::Open<>(spec, tensorstore::OpenMode::open,
                                     tensorstore::ReadWriteMode::read, context_).result();
    if (!opened.ok()) throw std::runtime_error("Direct open: " + path + ": " + opened.status().ToString());
    auto converted = tensorstore::Cast<double>(*opened);
    if (!converted.ok()) throw std::runtime_error("Direct cast: " + path);
    return stores_.emplace(path, *converted).first->second;
  }
  double at(const std::string& path, std::initializer_list<int64_t> indices) {
    auto& array = store(path);
    if (array.rank() != static_cast<int>(indices.size()))
      throw std::runtime_error("Direct rank mismatch: " + path);
    tensorstore::Box<> box(array.domain().box());
    size_t dimension = 0;
    for (int64_t index : indices) {
      if (index < 0 || index >= array.domain().shape()[dimension])
        throw std::runtime_error("Direct index out of bounds: " + path);
      box.origin()[dimension] = index;
      box.shape()[dimension++] = 1;
    }
    auto values = tensorstore::Read(array | tensorstore::IdentityTransform(box)).result();
    if (!values.ok()) throw std::runtime_error("Direct read: " + path);
    return *values->byte_strided_origin_pointer();
  }
  size_t channel(const std::string& path, const std::string& name) {
    auto& array = store(path);
    auto values = tensorstore::Read(array).result();
    if (!values.ok() || values->rank() != 2) throw std::runtime_error("Direct channel index read failed");
    const auto rows = values->shape()[0], width = values->shape()[1];
    for (int64_t row = 0; row < rows; ++row) {
      std::string candidate;
      for (int64_t col = 0; col < width; ++col) {
        const char character = static_cast<char>(values->data()[row * width + col]);
        if (!character) break;
        candidate.push_back(character);
      }
      if (candidate == name) return row;
    }
    throw std::runtime_error("Direct channel not found: " + name);
  }
  size_t frameRow(const std::string& path, int64_t frame) {
    size_t low = 0, high = store(path).domain().shape()[0];
    while (low < high) {
      const size_t middle = low + (high - low) / 2;
      if (at(path, {static_cast<int64_t>(middle)}) < frame) low = middle + 1;
      else high = middle;
    }
    if (at(path, {static_cast<int64_t>(low)}) != frame)
      throw std::runtime_error("Direct acquisition-frame identity not found");
    return low;
  }
  void same(double actual, const nlohmann::json& value, const std::string& label) {
    ++comparisons_;
    if (value.is_null() && !std::isfinite(actual)) return;
    if (!value.is_number() || !std::isfinite(actual) ||
        std::abs(actual - value.get<double>()) > 1e-6 * std::max(1.0, std::abs(actual)))
      throw std::runtime_error("Direct value mismatch: " + label +
                               " raw=" + std::to_string(actual) +
                               " timeline=" + value.dump());
  }
  static std::vector<size_t> sampleIndices(size_t size) {
    if (!size) return {};
    std::vector<size_t> values = {0, size / 2, size - 1};
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return values;
  }
 public:
  explicit RawTimelineVerifier(std::string root) : root_(std::move(root)) {
    auto context = tensorstore::Context::FromJson(
        {{"cache_pool", {{"total_bytes_limit", 16 * 1024 * 1024}}}});
    if (!context.ok()) throw std::runtime_error("Direct context creation failed");
    context_ = *context;
  }
  size_t verify(const nlohmann::json& window) {
    const std::string eyes = "analysis/eye_angle_runs/" +
        window.at("eye_angles").at("run").get<std::string>();
    for (const auto& trace : window.at("eye_angles").at("traces")) {
      const std::string field = trace.at("field");
      const auto col = channel(eyes + "/angle_channel_index/name", field);
      for (size_t index : sampleIndices(trace.at("frames").size())) {
        const int64_t frame = trace.at("frames").at(index);
        same(at(eyes + "/frame_angles", {frame, static_cast<int64_t>(col)}),
             trace.at("values").at(index), field);
        same(at(eyes + "/support/frame_time_seconds", {frame}),
             trace.at("times_seconds").at(index), "eye time");
      }
    }
    const std::string source = window.at("motion").at("source");
    const size_t variant_separator = source.rfind('/');
    const std::string qualified = source.substr(0, variant_separator);
    const size_t track_separator = qualified.rfind('/');
    const std::string track = "analysis/track_kinematics_runs/" +
        qualified.substr(0, track_separator) + "/tracks/" + qualified.substr(track_separator + 1);
    const std::string axis = track + "/source_acquisition_frame_index";
    for (const auto& trace : window.at("motion").at("traces")) {
      const std::string field = trace.at("field");
      if (field.rfind("speed_", 0) != 0) continue;
      const std::string units = trace.at("units") == "mm/s" ? "mm" : "px";
      const std::string path = track + "/movement/speed/" + field.substr(6) + "/" + units;
      for (size_t index : sampleIndices(trace.at("frames").size())) {
        const auto row = static_cast<int64_t>(frameRow(axis, trace.at("frames").at(index)));
        same(at(path, {row}), trace.at("values").at(index), field);
        same(at(track + "/time_seconds", {row}), trace.at("times_seconds").at(index), "motion time");
      }
    }
    const auto& bout = window.at("swim_bouts");
    const std::string base = "analysis/swim_bout_runs/" + bout.at("run").get<std::string>();
    const int64_t signal_id = bout.at("signal_id");
    const std::string signal_axis = base + "/signals/detector_signal_signal_ids";
    int64_t signal_row = -1;
    for (int64_t row = 0; row < store(signal_axis).domain().shape()[0]; ++row) {
      if (at(signal_axis, {row}) == signal_id) { signal_row = row; break; }
    }
    if (signal_row < 0) throw std::runtime_error("Direct detector signal ID not found");
    for (size_t index : sampleIndices(bout.at("detector_frames").size())) {
      const auto row = static_cast<int64_t>(frameRow(axis, bout.at("detector_frames").at(index)));
      same(at(base + "/signals/detector_signal_mm_s", {signal_row, row}),
           bout.at("detector_values").at(index), "bout detector");
    }
    for (const auto& interval : bout.at("intervals")) {
      const int64_t row = interval.at("source_index");
      same(at(base + "/tables/bouts/start_frame", {row}), interval.at("start"), "bout start");
      same(at(base + "/tables/bouts/end_frame", {row}), interval.at("end"), "bout end");
      same(at(base + "/tables/bouts/candidate_id", {row}), bout.at("candidate_id"), "bout candidate");
      same(at(base + "/tables/bouts/signal_id", {row}), bout.at("signal_id"), "bout signal");
    }
    return comparisons_;
  }
};
