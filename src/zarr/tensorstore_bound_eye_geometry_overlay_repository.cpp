#include "zarr/tensorstore_bound_eye_geometry_overlay_repository.h"

#include "zarr/archive_context_internal.h"
#include "zarr/canonical_json.h"
#include "zarr/shared_mask_frame_index.h"

#include <tensorstore/index_space/dim_expression.h>
#include <tensorstore/batch.h>
#include <tensorstore/open.h>
#include <tensorstore/tensorstore.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <deque>
#include <functional>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace crimson::zarr {
namespace {
namespace ts = tensorstore;
using json = nlohmann::json;

void error(std::string *out, std::string message) {
  if (out) *out = std::move(message);
}

std::string str(const json &object, std::string_view key) {
  auto it = object.find(std::string(key));
  return it != object.end() && it->is_string() ? it->get<std::string>() : "";
}

const json *obj(const json &object, std::string_view key) {
  auto it = object.find(std::string(key));
  return it != object.end() && it->is_object() ? &*it : nullptr;
}

bool runName(const std::string &name) {
  return !name.empty() && name != "." && name != ".." &&
         name.find('/') == std::string::npos &&
         name.find('\\') == std::string::npos;
}

template <typename T, ts::DimensionIndex R>
std::optional<ts::TensorStore<T, R>> open(const ArchiveContext::Impl &archive,
                                           const std::string &path,
                                           BoundEyeGeometryOverlayOpenMetrics *m,
                                           std::string *e) {
  auto spec = internal::MakeReadOnlyArraySpec(archive, path);
  if (!spec) { error(e, "Missing bound eye array: " + path); return {}; }
  auto result = ts::Open<T, R>(*spec, ts::OpenMode::open,
                                ts::ReadWriteMode::read, archive.context).result();
  if (!result.ok()) { error(e, path + ": " + result.status().ToString()); return {}; }
  if (m) ++m->exact_handle_opens;
  return *result;
}

template <typename T, ts::DimensionIndex R>
bool shape(const ts::TensorStore<T, R> &store,
           const std::array<size_t, static_cast<size_t>(R)> &expected) {
  for (ts::DimensionIndex i = 0; i < R; ++i)
    if (store.domain().shape()[i] != static_cast<ts::Index>(expected[i]))
      return false;
  return true;
}

template <typename T, ts::DimensionIndex R, typename Result>
bool copyReadResult(const Result &result, std::vector<T> *out, std::string *e) {
  if (!result.ok() || result->rank() != R ||
      result->byte_strides().size() != R) {
    error(e, result.ok() ? "Bound eye read shape mismatch"
                          : result.status().ToString()); return false;
  }
  size_t count = 1;
  for (ts::DimensionIndex i = 0; i < R; ++i)
    count *= static_cast<size_t>(result->shape()[i]);
  out->resize(count);
  const auto *base = reinterpret_cast<const uint8_t *>(
      result->byte_strided_origin_pointer().get());
  const auto strides = result->byte_strides();
  for (size_t linear = 0; linear < count; ++linear) {
    size_t remainder = linear;
    ts::Index offset = 0;
    for (ts::DimensionIndex i = R; i-- > 0;) {
      const size_t extent = static_cast<size_t>(result->shape()[i]);
      offset += static_cast<ts::Index>(remainder % extent) * strides[i];
      remainder /= extent;
    }
    (*out)[linear] = *reinterpret_cast<const T *>(base + offset);
  }
  return true;
}

template <typename T, ts::DimensionIndex R>
std::optional<ts::Box<R>> readBox(const ts::TensorStore<T, R> &store,
                                  size_t first, size_t last,
                                  size_t column, std::string *e) {
  if (first > last || last > static_cast<size_t>(store.domain().shape()[0]) ||
      (column != SIZE_MAX && (R < 2 ||
       column >= static_cast<size_t>(store.domain().shape()[1])))) {
    error(e, "Bound eye row/column range is invalid"); return {};
  }
  ts::Box<R> box(store.domain().box());
  box.origin()[0] = static_cast<ts::Index>(first);
  box.shape()[0] = static_cast<ts::Index>(last - first);
  if constexpr (R >= 2) {
    if (column != SIZE_MAX) {
      box.origin()[1] = static_cast<ts::Index>(column);
      box.shape()[1] = 1;
    }
  }
  return box;
}

template <typename T, ts::DimensionIndex R>
bool read(const ts::TensorStore<T, R> &store, size_t first, size_t last,
          std::vector<T> *out, std::string *e, size_t column = SIZE_MAX,
          uint64_t *future_wait_ns = nullptr) {
  auto box = readBox(store, first, last, column, e);
  if (!box) return false;
  if (first == last) { out->clear(); return true; }
  const auto began = std::chrono::steady_clock::now();
  auto result = ts::Read(store | ts::IdentityTransform(*box)).result();
  if (future_wait_ns)
    *future_wait_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - began).count();
  return copyReadResult<T, R>(result, out, e);
}

template <typename T, ts::DimensionIndex R>
bool readCount(const ts::TensorStore<T, R> &store, size_t first, size_t last,
               std::vector<T> *out, std::string *e,
               EyeGeometryOverlayRepository::AccessMetrics *m,
               std::string_view array, size_t column = SIZE_MAX) {
  auto it = std::find_if(m->per_array.begin(), m->per_array.end(),
      [array](const auto &entry) { return entry.array == array; });
  if (it == m->per_array.end()) {
    m->per_array.push_back({std::string(array)});
    it = std::prev(m->per_array.end());
  }
  ++m->payload_read_calls;
  ++it->calls;
  uint64_t waited = 0;
  const bool ok = read(store, first, last, out, e, column, &waited);
  ++it->future_elapsed_count;
  it->future_elapsed_ns_sum += waited;
  it->future_elapsed_ns_max = std::max(it->future_elapsed_ns_max, waited);
  if (!ok) { ++it->failures; return false; }
  const uint64_t bytes = out->size() * sizeof(T);
  m->logical_payload_bytes_read += bytes;
  it->logical_bytes += bytes;
  return true;
}

void addReadMetrics(EyeGeometryOverlayRepository::AccessMetrics *total,
                    const EyeGeometryOverlayRepository::AccessMetrics &delta) {
  total->payload_read_calls += delta.payload_read_calls;
  total->logical_payload_bytes_read += delta.logical_payload_bytes_read;
  total->peak_inflight_payload_reads = std::max(
      total->peak_inflight_payload_reads, delta.peak_inflight_payload_reads);
  for (const auto &source : delta.per_array) {
    auto it = std::find_if(total->per_array.begin(), total->per_array.end(),
        [&](const auto &entry) { return entry.array == source.array; });
    if (it == total->per_array.end()) {
      total->per_array.push_back(source);
      continue;
    }
    it->calls += source.calls;
    it->logical_bytes += source.logical_bytes;
    it->failures += source.failures;
    it->future_elapsed_count += source.future_elapsed_count;
    it->future_elapsed_ns_sum += source.future_elapsed_ns_sum;
    it->future_elapsed_ns_max = std::max(it->future_elapsed_ns_max,
                                          source.future_elapsed_ns_max);
  }
}

// A batch is submitted before any member is waited on. Releasing every future
// is mandatory, including on an error or stale-generation cancellation.
class ReadWave {
public:
  ReadWave(EyeGeometryOverlayRepository::AccessMetrics *metrics,
           std::string *error_message,
           const std::function<bool()> &cancelled,
           size_t max_inflight = 4)
      : metrics_(metrics), error_message_(error_message),
        cancelled_(cancelled), max_inflight_(max_inflight),
        batch_(ts::Batch::New()) {}

  ~ReadWave() { if (!waiters_.empty()) flush(); }

  template <typename T, ts::DimensionIndex R>
  bool add(const ts::TensorStore<T, R> &store, size_t first, size_t last,
           std::vector<T> *out, std::string_view array,
           size_t column = SIZE_MAX) {
    if (waiters_.size() >= max_inflight_) {
      if (!flush()) return false;
      if (cancelled_ && cancelled_()) {
        error(error_message_, "Bound eye request cancelled between read waves");
        return false;
      }
    }
    auto box = readBox(store, first, last, column, error_message_);
    if (!box) return false;
    if (first == last) { out->clear(); return true; }
    const std::string name(array);
    const auto began = std::chrono::steady_clock::now();
    auto future = ts::Read(store | ts::IdentityTransform(*box), batch_);
    waiters_.emplace_back([this, out, name, began,
                           future = std::move(future)]() mutable {
      auto result = future.result();
      const auto waited = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - began).count());
      auto it = std::find_if(metrics_->per_array.begin(),
          metrics_->per_array.end(), [&](const auto &entry) {
            return entry.array == name;
          });
      if (it == metrics_->per_array.end()) {
        metrics_->per_array.push_back({name});
        it = std::prev(metrics_->per_array.end());
      }
      ++metrics_->payload_read_calls;
      ++it->calls;
      ++it->future_elapsed_count;
      it->future_elapsed_ns_sum += waited;
      it->future_elapsed_ns_max = std::max(it->future_elapsed_ns_max, waited);
      if (!copyReadResult<T, R>(result, out, error_message_)) {
        ++it->failures;
        return false;
      }
      const uint64_t bytes = out->size() * sizeof(T);
      metrics_->logical_payload_bytes_read += bytes;
      it->logical_bytes += bytes;
      return true;
    });
    metrics_->peak_inflight_payload_reads = std::max<uint64_t>(
        metrics_->peak_inflight_payload_reads, waiters_.size());
    return true;
  }

  bool flush() {
    batch_.Release();
    bool ok = true;
    for (auto &waiter : waiters_) {
      // Even after a failure, drain all futures to keep the wave within its
      // concurrency and decoded-working-set admission.
      if (!waiter()) ok = false;
    }
    waiters_.clear();
    batch_ = ts::Batch::New();
    return ok;
  }

private:
  EyeGeometryOverlayRepository::AccessMetrics *metrics_;
  std::string *error_message_;
  const std::function<bool()> &cancelled_;
  size_t max_inflight_;
  ts::Batch batch_;
  std::vector<std::function<bool()>> waiters_;
};

std::vector<std::string> names(const ArchiveContext::Impl &archive,
                               const std::string &path, size_t expected,
                               BoundEyeGeometryOverlayOpenMetrics *m,
                               std::string *e) {
  auto store = open<uint8_t, 2>(archive, path, m, e);
  if (!store || store->domain().shape()[0] != static_cast<ts::Index>(expected) ||
      store->domain().shape()[1] <= 0 || store->domain().shape()[1] > 512)
    return {};
  std::vector<uint8_t> bytes;
  if (!read(*store, 0, expected, &bytes, e)) return {};
  const size_t width = static_cast<size_t>(store->domain().shape()[1]);
  std::vector<std::string> result;
  result.reserve(expected);
  for (size_t i = 0; i < expected; ++i) {
    size_t size = 0;
    while (size < width && bytes[i * width + size]) ++size;
    result.emplace_back(reinterpret_cast<const char *>(bytes.data() + i * width), size);
  }
  return result;
}

bool availableNames(const ArchiveContext::Impl &archive,
                    const std::string &base, size_t expected,
                    BoundEyeGeometryOverlayOpenMetrics *m,
                    std::vector<std::string> *result,
                    std::string *e) {
  *result = names(archive, base + "/name", expected, m, e);
  auto available = open<bool, 1>(archive, base + "/roi_available", m, e);
  std::vector<bool> flags;
  if (result->size() != expected || !available ||
      !shape(*available, std::array<size_t, 1>{expected}) ||
      !read(*available, 0, expected, &flags, e)) return false;
  std::unordered_set<std::string> unique;
  for (size_t i = 0; i < expected; ++i) {
    if ((*result)[i].empty() || !unique.insert((*result)[i]).second)
      return false;
    if (!flags[i]) (*result)[i].clear();
  }
  return true;
}

bool boundedStorageChunk(const ArchiveContext::Impl &archive,
                         const std::string &path, uint64_t limit) {
  const auto metadata = internal::ReadArchiveJson(archive, path + "/zarr.json");
  if (!metadata) return false;
  try {
    const auto &codecs = metadata->at("codecs");
    const auto &shape = codecs.size() == 1 &&
                                codecs.at(0).value("name", "") == "sharding_indexed"
                            ? codecs.at(0).at("configuration").at("chunk_shape")
                            : metadata->at("chunk_grid").at("configuration").at("chunk_shape");
    const auto dtype = metadata->at("data_type").get<std::string>();
    const uint64_t itemsize = dtype == "float32" ? 4 :
                              dtype == "float64" || dtype == "int64" ||
                                      dtype == "uint64" ? 8 :
                              dtype == "uint16" ? 2 : 1;
    uint64_t bytes = itemsize;
    for (const auto &value : shape) {
      const uint64_t extent = value.get<uint64_t>();
      if (!extent || extent > limit / bytes) return false;
      bytes *= extent;
    }
    return bytes <= limit;
  } catch (const json::exception &) { return false; }
}

size_t channel(const std::vector<std::string> &names, std::string_view target) {
  auto it = std::find(names.begin(), names.end(), target);
  return it == names.end() ? SIZE_MAX : static_cast<size_t>(it - names.begin());
}

bool coordinateAuthority(const ArchiveContext::Impl &archive,
                         const std::string &path,
                         const CanonicalOverlaySelection &selection,
                         const std::string &expected_digest,
                         const std::string &row_record_sha256) {
  auto attrs = internal::ReadArchiveAttributes(archive, path);
  if (!attrs || str(*attrs, "coordinate_descriptor_sha256") != expected_digest)
    return false;
  const auto *d = obj(*attrs, "coordinate_descriptor");
  const auto *extent = d ? obj(*d, "reference_extent") : nullptr;
  const auto *overlay = d ? obj(*d, "source_camera_overlay") : nullptr;
  const auto *directions = d ? obj(*d, "positive_directions") : nullptr;
  const auto *identity = d ? obj(*d, "row_identity") : nullptr;
  try {
    return d && extent && overlay && directions && identity &&
        CanonicalJsonSha256(*d) == expected_digest &&
        d->value("schema_id", "") == "palette.coordinate_descriptor" &&
        d->value("schema_version", 0) == 2 &&
        d->value("profile_id", "") == selection.coordinate_descriptor_profile &&
        d->value("space_id", "") == "source_camera_image_px" &&
        d->value("geometry_type", "") == "ellipse_cxcy_wh_angle" &&
        d->value("origin", "") == "top_left" &&
        d->at("components").get<std::vector<std::string>>() ==
          std::vector<std::string>({"center_x", "center_y", "width", "height", "angle"}) &&
        d->at("component_units").get<std::vector<std::string>>() ==
          std::vector<std::string>({"px", "px", "px", "px", "deg"}) &&
        identity->value("record_ref", "") ==
          "/" + selection.shape.group + "/" + selection.shape.run_id +
          "@row_identity_contract" &&
        identity->value("record_sha256", "") == row_record_sha256 &&
        directions->value("x", "") == "right" &&
        directions->value("y", "") == "down" &&
        extent->value("width", size_t{0}) == selection.source_width &&
        extent->value("height", size_t{0}) == selection.source_height &&
        overlay->value("status", "") == "direct";
  } catch (const json::exception &) { return false; }
}

EyeGeometryAxis axis(const float *ellipse, bool major, double crop_x,
                     double crop_y) {
  EyeGeometryAxis result;
  const double length = major ? ellipse[2] : ellipse[3];
  if (!std::isfinite(ellipse[0]) || !std::isfinite(ellipse[1]) ||
      !std::isfinite(length) || !std::isfinite(ellipse[4]) || length <= 0)
    return result;
  constexpr double pi = 3.14159265358979323846;
  const double radians = ellipse[4] * pi / 180.0 + (major ? 0.0 : pi / 2.0);
  const double dx = 0.5 * length * std::cos(radians);
  const double dy = 0.5 * length * std::sin(radians);
  const double x = ellipse[0] - crop_x;
  const double y = ellipse[1] - crop_y;
  result.valid = true;
  result.start = {x - dx, y - dy};
  result.end = {x + dx, y + dy};
  return result;
}

struct Sources {
  ts::TensorStore<int64_t, 1> offsets;
  ts::TensorStore<int64_t, 1> eye_frames, eye_acquisition, shape_frames, mask_frames;
  ts::TensorStore<uint64_t, 1> eye_keys, shape_keys, mask_keys;
  ts::TensorStore<int64_t, 1> shape_crop_rows, mask_crop_rows;
  ts::TensorStore<float, 2> crop;
  std::array<ts::TensorStore<float, 2>, 2> ellipse;
  std::array<ts::TensorStore<bool, 1>, 2> ellipse_success;
  ts::TensorStore<bool, 1> body_valid;
  std::array<ts::TensorStore<float, 2>, 3> body;
  ts::TensorStore<float, 2> angles;
  ts::TensorStore<float, 3> vectors;
  ts::TensorStore<uint16_t, 2> qa;
  std::array<size_t, 2> eye_angle, gaze_angle, gaze_vector, valid_eye;
  size_t vergence = SIZE_MAX, valid_frame = SIZE_MAX;
};

class Repository final : public EyeGeometryOverlayRepository {
public:
  Repository(EyeGeometryOverlayDescriptor descriptor,
             std::vector<int64_t> offsets, Sources sources,
             BoundEyeGeometryOverlayOpenRequest limits)
      : descriptor_(std::move(descriptor)), offsets_(std::move(offsets)),
        sources_(std::move(sources)), limits_(std::move(limits)),
        shared_index_(limits_.shared_mask_frame_index) {
    limits_.archive.reset();
    limits_.shared_mask_frame_index.reset();
  }

  const EyeGeometryOverlayDescriptor &descriptor() const override { return descriptor_; }

  EyeGeometryOverlayResolution resolveCameraFrame(int64_t frame, int width,
                                                   int height) const override {
    return resolveCameraFrameFields(frame, width, height, EyeGeometryFields::All);
  }

  EyeGeometryOverlayResolution resolveCameraFrameFields(
      int64_t frame, int width, int height, EyeGeometryFieldMask requested,
      const std::function<bool()> &cancelled = {}) const override {
    EyeGeometryFieldMask fields = NormalizeEyeGeometryFields(requested);
    EyeGeometryOverlayResolution result;
    result.camera_frame = frame;
    result.loaded_fields = 0;
    if (frame < 0 || static_cast<uint64_t>(frame) >= descriptor_.camera_frame_count) {
      result.status = EyeGeometryOverlayStatus::OutOfRange; return result;
    }
    if (width <= 0 || height <= 0 ||
        static_cast<size_t>(width) != limits_.selection.source_width ||
        static_cast<size_t>(height) != limits_.selection.source_height) {
      result.status = EyeGeometryOverlayStatus::InvalidDimensions; return result;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (const auto &entry : cache_) if (entry.first == frame) {
        if ((entry.second.loaded_fields & fields) == fields) {
          ++access_.cache_hits; return entry.second;
        }
        fields |= entry.second.loaded_fields;
      }
    }
    if (cancelled && cancelled()) {
      result.status = EyeGeometryOverlayStatus::ReadFailed;
      result.error = "Bound eye request cancelled";
      return result;
    }
    // A stale request must release all of its TensorStore futures before a
    // successor begins. This gate bounds aggregate in-flight work even when
    // different scheduler generations call the same repository concurrently.
    std::unique_lock<std::mutex> read_guard(read_mutex_);
    if (cancelled && cancelled()) {
      result.status = EyeGeometryOverlayStatus::ReadFailed;
      result.error = "Bound eye request cancelled";
      return result;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (const auto &entry : cache_) if (entry.first == frame) {
        if ((entry.second.loaded_fields & fields) == fields) {
          ++access_.cache_hits; return entry.second;
        }
        fields |= entry.second.loaded_fields;
      }
    }
    const auto &offsets = shared_index_ ? shared_index_->offsets() : offsets_;
    const size_t first = static_cast<size_t>(offsets[frame]);
    const size_t last = static_cast<size_t>(offsets[frame + 1]);
    const size_t n = last - first;
    if (!n) { result.status = EyeGeometryOverlayStatus::Missing; return result; }
    // Admission counts requested payloads. TensorStore may decode larger inner
    // chunks; those are managed by its bounded context cache.
    constexpr uint64_t per_row_upper_bound = 512;
    if (n > limits_.max_observations_per_frame ||
        n > limits_.max_decoded_frame_bytes / per_row_upper_bound) {
      result.status = EyeGeometryOverlayStatus::ReadFailed;
      result.error = "Bound eye frame exceeds read admission"; return result;
    }
    AccessMetrics counts;
    auto fail = [&](std::string message) {
      std::lock_guard<std::mutex> lock(mutex_);
      addReadMetrics(&access_, counts);
      result.status = EyeGeometryOverlayStatus::ReadFailed;
      result.error = std::move(message);
      result.detections.clear(); return result;
    };
    std::string e;
    std::vector<int64_t> ef, ea, sf, mf, sc, mc;
    std::vector<uint64_t> ek, sk, mk;
    std::vector<float> crops;
    ReadWave identity(&counts, &e, cancelled);
    const bool identity_queued =
        identity.add(sources_.eye_frames, first, last, &ef, "E/support/frame_indices") &&
        identity.add(sources_.eye_acquisition, first, last, &ea, "E/support/source_acquisition_frame_index") &&
        identity.add(sources_.shape_frames, first, last, &sf, "S/source_acquisition_frame_index") &&
        identity.add(sources_.mask_frames, first, last, &mf, "M/source_acquisition_frame_index") &&
        identity.add(sources_.eye_keys, first, last, &ek, "E/support/instance_key") &&
        identity.add(sources_.shape_keys, first, last, &sk, "S/instance_key") &&
        identity.add(sources_.mask_keys, first, last, &mk, "M/instance_key") &&
        identity.add(sources_.shape_crop_rows, first, last, &sc, "S/source_crop_row_ids") &&
        identity.add(sources_.mask_crop_rows, first, last, &mc, "M/source_crop_row_ids") &&
        identity.add(sources_.crop, first, last, &crops, "M/source_crop_xywh");
    if (!identity.flush() || !identity_queued)
      return fail("Bound eye row identity read failed: " + e);
    std::unordered_set<uint64_t> seen;
    for (size_t i = 0; i < n; ++i) {
      const float *c = crops.data() + 4 * i;
      if (ef[i] != frame || ea[i] != frame || sf[i] != frame || mf[i] != frame ||
          ek[i] != sk[i] || ek[i] != mk[i] || !seen.insert(ek[i]).second ||
          sc[i] != mc[i] || sc[i] < 0 ||
          !std::all_of(c, c + 4, [](float x) { return std::isfinite(x); }) ||
          c[0] < 0 || c[1] < 0 || c[2] != descriptor_.coordinate_width ||
          c[3] != descriptor_.coordinate_height ||
          c[0] + c[2] > width || c[1] + c[3] > height)
        return fail("Bound eye row identity or crop disagrees at row " +
                    std::to_string(first + i));
    }
    if (cancelled && cancelled()) return fail("Bound eye request cancelled");
    std::array<std::vector<float>, 2> ellipse;
    std::array<std::vector<bool>, 2> success;
    std::vector<bool> body_valid;
    std::array<std::vector<float>, 3> body;
    std::array<std::vector<float>, 5> angles;
    std::array<std::vector<float>, 2> gaze;
    std::array<std::vector<uint16_t>, 3> qa;
    const bool need_body = (fields & EyeGeometryFields::BodyFrame) != 0;
    ReadWave payload(&counts, &e, cancelled);
    if (!payload.add(sources_.qa, first, last, &qa[0],
                     "E/roi_qa", sources_.valid_frame)) {
      payload.flush();
      return fail("Bound eye frame QA read failed: " + e);
    }
    for (size_t eye = 0; eye < 2; ++eye) {
      const EyeGeometryFieldMask geometry = eye ? EyeGeometryFields::RightGeometry : EyeGeometryFields::LeftGeometry;
      const EyeGeometryFieldMask gaze_field = eye ? EyeGeometryFields::RightGaze : EyeGeometryFields::LeftGaze;
      const EyeGeometryFieldMask signed_field = eye ? EyeGeometryFields::RightSigned : EyeGeometryFields::LeftSigned;
      const EyeGeometryFieldMask angle_field = eye ? EyeGeometryFields::RightAngle : EyeGeometryFields::LeftAngle;
      if (!(fields & geometry)) continue;
      if (!payload.add(sources_.ellipse[eye], first, last, &ellipse[eye],
                     eye ? "S/components/eye_right/ellipse_params" : "S/components/eye_left/ellipse_params") ||
          !payload.add(sources_.ellipse_success[eye], first, last,
                     &success[eye],
                     eye ? "S/components/eye_right/ellipse_success" : "S/components/eye_left/ellipse_success") ||
          !payload.add(sources_.qa, first, last, &qa[1 + eye],
                     "E/roi_qa", sources_.valid_eye[eye]))
      {
        payload.flush();
        return fail("Bound eye geometry read failed: " + e);
      }
      if ((fields & angle_field) &&
          !payload.add(sources_.angles, first, last, &angles[eye],
                     "E/roi_angles", sources_.eye_angle[eye]))
      {
        payload.flush();
        return fail("Bound eye angle read failed: " + e);
      }
      if ((fields & signed_field) &&
          !payload.add(sources_.angles, first, last, &angles[2 + eye],
                     "E/roi_angles", sources_.gaze_angle[eye]))
      {
        payload.flush();
        return fail("Bound eye signed angle read failed: " + e);
      }
      if ((fields & gaze_field) &&
          !payload.add(sources_.vectors, first, last, &gaze[eye],
                     "E/roi_vectors", sources_.gaze_vector[eye]))
      {
        payload.flush();
        return fail("Bound eye gaze read failed: " + e);
      }
    }
    if (need_body) {
      if (!payload.add(sources_.body_valid, first, last, &body_valid,
                       "E/support/body_frame/valid")) {
        payload.flush();
        return fail("Bound eye body-frame validity read failed: " + e);
      }
      for (size_t axis_index = 0; axis_index < 3; ++axis_index)
        if (!payload.add(sources_.body[axis_index], first, last, &body[axis_index],
                       axis_index == 0 ? "E/support/body_frame/origin_xy" :
                       axis_index == 1 ? "E/support/body_frame/forward_axis_xy" :
                                         "E/support/body_frame/left_axis_xy")) {
          payload.flush();
          return fail("Bound eye body-frame read failed: " + e);
        }
    }
    if ((fields & EyeGeometryFields::Vergence) &&
        !payload.add(sources_.angles, first, last, &angles[4],
                     "E/roi_angles", sources_.vergence)) {
      payload.flush();
      return fail("Bound eye vergence read failed: " + e);
    }
    if (!payload.flush()) return fail("Bound eye payload read failed: " + e);
    if (cancelled && cancelled()) return fail("Bound eye request cancelled");
    result.detections.reserve(n);
    for (size_t i = 0; i < n; ++i) {
      EyeGeometryOverlayDetection d;
      d.eye_row = first + i;
      d.instance_key = ek[i]; d.instance_key_valid = true;
      d.camera_frame = frame;
      d.detection_index = static_cast<int64_t>(i);
      d.source_crop_row_id = sc[i];
      d.roi_x = crops[4*i]; d.roi_y = crops[4*i+1];
      d.roi_width = crops[4*i+2]; d.roi_height = crops[4*i+3];
      d.frame_valid = qa[0][i] != 0;
      d.body_frame_valid = need_body && d.frame_valid && body_valid[i] &&
          std::isfinite(body[0][2*i]) && std::isfinite(body[0][2*i+1]) &&
          std::isfinite(body[1][2*i]) && std::isfinite(body[1][2*i+1]) &&
          std::isfinite(body[2][2*i]) && std::isfinite(body[2][2*i+1]);
      if (d.body_frame_valid) {
        d.body_origin = {body[0][2*i], body[0][2*i+1]};
        d.body_forward_axis = {body[1][2*i], body[1][2*i+1]};
        d.body_left_axis = {body[2][2*i], body[2][2*i+1]};
      }
      for (size_t eye = 0; eye < 2; ++eye) {
        const EyeGeometryFieldMask geometry = eye ? EyeGeometryFields::RightGeometry : EyeGeometryFields::LeftGeometry;
        const EyeGeometryFieldMask gaze_field = eye ? EyeGeometryFields::RightGaze : EyeGeometryFields::LeftGaze;
        const EyeGeometryFieldMask signed_field = eye ? EyeGeometryFields::RightSigned : EyeGeometryFields::LeftSigned;
        const EyeGeometryFieldMask angle_field = eye ? EyeGeometryFields::RightAngle : EyeGeometryFields::LeftAngle;
        if (!(fields & geometry)) continue;
        auto &target = d.eyes[eye];
        const float *params = ellipse[eye].data() + 5*i;
        target.valid = d.frame_valid && qa[1+eye][i] && success[eye][i] &&
            std::all_of(params, params + 5, [](float x) { return std::isfinite(x); }) &&
            params[2] >= params[3] && params[3] > 0 &&
            params[4] >= 0 && params[4] < 180;
        if (target.valid) {
          target.major_axis = axis(params, true, d.roi_x, d.roi_y);
          target.minor_axis = axis(params, false, d.roi_x, d.roi_y);
          target.valid = target.major_axis.valid && target.minor_axis.valid;
        }
        if ((fields & gaze_field) && target.valid && d.body_frame_valid &&
            std::isfinite(gaze[eye][2*i]) && std::isfinite(gaze[eye][2*i+1]) &&
            std::hypot(gaze[eye][2*i], gaze[eye][2*i+1]) > 1e-6) {
          target.gaze = {gaze[eye][2*i], gaze[eye][2*i+1]};
          target.gaze_valid = true;
        }
        if ((fields & signed_field) && target.valid && d.body_frame_valid &&
            std::isfinite(angles[2+eye][i])) {
          target.signed_angle_valid = true;
          target.signed_angle_degrees = angles[2+eye][i];
        }
        if ((fields & angle_field) && target.valid && d.body_frame_valid &&
            std::isfinite(angles[eye][i])) {
          target.eye_frame_angle_valid = true;
          target.eye_frame_angle_degrees = angles[eye][i];
        }
      }
      if ((fields & EyeGeometryFields::Vergence) && d.body_frame_valid &&
          d.eyes[0].valid && d.eyes[1].valid &&
          std::isfinite(angles[4][i])) {
        d.vergence_valid = true; d.vergence_degrees = angles[4][i];
      }
      result.detections.push_back(std::move(d));
    }
    result.status = EyeGeometryOverlayStatus::Mapped;
    result.loaded_fields = fields;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      addReadMetrics(&access_, counts);
      const uint64_t bytes = sizeof(EyeGeometryOverlayResolution) +
          result.detections.capacity() * sizeof(EyeGeometryOverlayDetection);
      if (bytes <= limits_.max_cached_decoded_bytes) {
        for (auto it = cache_.begin(); it != cache_.end();) {
          if (it->first != frame) { ++it; continue; }
          cache_bytes_ -= sizeof(EyeGeometryOverlayResolution) +
              it->second.detections.capacity() * sizeof(EyeGeometryOverlayDetection);
          it = cache_.erase(it);
        }
        while (!cache_.empty() &&
               (cache_bytes_ + bytes > limits_.max_cached_decoded_bytes ||
                cache_.size() >= 64)) {
          cache_bytes_ -= sizeof(EyeGeometryOverlayResolution) +
              cache_.front().second.detections.capacity() *
                  sizeof(EyeGeometryOverlayDetection);
          cache_.pop_front();
        }
        cache_.emplace_back(frame, result);
        cache_bytes_ += bytes;
        access_.retained_decoded_cache_bytes = cache_bytes_;
      }
    }
    return result;
  }

  AccessMetrics accessMetrics() const override {
    std::lock_guard<std::mutex> lock(mutex_); return access_;
  }
  RepositoryMemoryMetrics memoryMetrics() const override {
    RepositoryMemoryMetrics m;
    m.retained_index_bytes = offsets_.capacity() * sizeof(int64_t);
    std::lock_guard<std::mutex> lock(mutex_);
    // TensorStore's shared cache is intentionally outside this repository's
    // retained-byte accounting.
    m.decoded_cache_bytes = cache_bytes_; return m;
  }

private:
  EyeGeometryOverlayDescriptor descriptor_;
  std::vector<int64_t> offsets_;
  Sources sources_;
  BoundEyeGeometryOverlayOpenRequest limits_;
  std::shared_ptr<const SharedMaskFrameIndex> shared_index_;
  mutable std::mutex mutex_;
  mutable std::mutex read_mutex_;
  mutable std::deque<std::pair<int64_t, EyeGeometryOverlayResolution>> cache_;
  mutable uint64_t cache_bytes_ = 0;
  mutable AccessMetrics access_;
};

} // namespace

std::unique_ptr<EyeGeometryOverlayRepository>
OpenBoundEyeGeometryOverlayRepository(
    const BoundEyeGeometryOverlayOpenRequest &request, std::string *message,
    BoundEyeGeometryOverlayOpenMetrics *open_metrics) {
  BoundEyeGeometryOverlayOpenMetrics m;
  const auto &s = request.selection;
  if (!request.archive || !request.archive->impl_ ||
      !s.eye.valid || !s.shape.valid || !s.mask.valid || !s.keypoints.valid ||
      s.eye.group != "analysis/eye_angle_runs" ||
      s.shape.group != "analysis/subject_shape_runs" ||
      s.mask.group != "refined_subject_masks_runs" ||
      !runName(s.eye.run_id) || !runName(s.shape.run_id) ||
      !runName(s.mask.run_id) || !runName(s.keypoints.run_id) ||
      s.eye.schema_version != 7 || s.shape.schema_version != 5 ||
      s.frame_count == 0 || s.observation_count == 0 ||
      !IsLowerSha256(s.eye.identity_digest) ||
      !IsLowerSha256(s.shape.identity_digest) ||
      !IsLowerSha256(s.instance_key_digest) ||
      !IsLowerSha256(s.acquisition_frame_digest) ||
      s.shape.bound_source_run_id != s.mask.run_id ||
      request.max_observations_per_frame == 0 ||
      request.max_decoded_frame_bytes == 0 ||
      request.max_cached_decoded_bytes == 0 ||
      request.max_storage_chunk_bytes == 0) {
    error(message, "Exact validated eye/shape/mask selection is required");
    return nullptr;
  }
  const auto &archive = *request.archive->impl_;
  const std::string eb = s.eye.group + "/" + s.eye.run_id;
  const std::string sb = s.shape.group + "/" + s.shape.run_id;
  const std::string mb = s.mask.group + "/" + s.mask.run_id;
  auto ea = internal::ReadArchiveAttributes(archive, eb);
  auto sa = internal::ReadArchiveAttributes(archive, sb);
  if (!ea || !sa) { error(message, "Bound eye/shape metadata is unavailable"); return nullptr; }
  const auto *contract = obj(*ea, "eye_angle_algorithm_contract");
  const auto *sources = obj(*ea, "eye_angle_source_contracts");
  const auto *geometry = sources ? obj(*sources, "eye_geometry") : nullptr;
  const auto *authority = geometry ? obj(*geometry, "source_authority") : nullptr;
  const auto *publication = authority ? obj(*authority, "canonical_publication") : nullptr;
  const auto *descriptors = publication ? obj(*publication, "ellipse_coordinate_descriptors") : nullptr;
  const auto *allowed_arrays = authority ? obj(*authority, "allowed_arrays") : nullptr;
  const auto *keypoints = sources ? obj(*sources, "keypoints") : nullptr;
  const auto *keypoint_authority = keypoints ?
      obj(*keypoints, "canonical_keypoint_authority") : nullptr;
  const auto *ordered = keypoint_authority ?
      obj(*keypoint_authority, "ordered_row_alignment") : nullptr;
  const auto *binding = obj(*sa, "subject_shape_source_binding");
  const auto *row_identity = obj(*sa, "row_identity_contract");
  const std::string row_record_sha256 = str(*sa, "row_identity_contract_sha256");
  if (str(*ea, "palette_run_name") != s.eye.run_id ||
      str(*ea, "lineage_hash") != s.eye.identity_digest ||
      str(*ea, "source_fingerprint") != s.eye.identity_digest ||
      str(*ea, "source_lineage_hash") != s.eye.identity_digest ||
      str(*ea, "staged_input_integrity_receipt_sha256") !=
          s.eye.manifest_payload_digest ||
      str(*ea, "source_subject_shape_run") != s.shape.run_id ||
      str(*ea, "source_eye_geometry_run") != s.shape.run_id ||
      str(*ea, "source_eye_geometry_stage") != s.shape.group ||
      str(*ea, "source_geometry_kind") != "subject_shape_eye_geometry" ||
      str(*ea, "source_refined_subject_masks_run") != s.mask.run_id ||
      str(*ea, "source_base_keypoints_run") != s.keypoints.run_id ||
      str(*ea, "palette_run_completion_status") != "complete" ||
      str(*ea, "source_eye_geometry_authority_mode") !=
          "digest_bound_staged_subset" ||
      str(*ea, "body_frame_coordinate_space") != "roi_pixels" ||
      !contract || !geometry || !authority || !publication || !descriptors ||
      !allowed_arrays || !ordered ||
      !binding || !row_identity ||
      str(*sa, "publication_manifest_sha256") != s.shape.identity_digest ||
      str(*sa, "subject_shape_source_binding_sha256") !=
          s.shape.manifest_payload_digest ||
      CanonicalJsonSha256(*binding) != s.shape.manifest_payload_digest ||
      str(*sa, "row_identity_contract_sha256") !=
          CanonicalJsonSha256(*row_identity) ||
      geometry->value("path", "") != sb ||
      geometry->value("schema_version", 0) != 5 ||
      authority->value("source_subject_shape_run", "") != s.shape.run_id ||
      authority->value("source_subject_shape_run_ref", "") != "/" + sb ||
      authority->value("authority_scope", "") !=
          "eye_geometry_exact_digest_bound_staged_subset_only" ||
      authority->value("row_count", size_t{0}) != s.observation_count ||
      publication->value("manifest_sha256", "") != s.shape.identity_digest ||
      publication->value("row_identity_ref", "") !=
          "/" + sb + "@row_identity_contract" ||
      publication->value("row_identity_sha256", "") != row_record_sha256 ||
      contract->value("schema_id", "") !=
          "analysis.eye_angle_algorithm_contract" ||
      contract->value("schema_version", 0) != 1) {
    error(message, "Bound eye publication/source authority disagrees with selection");
    return nullptr;
  }
  size_t roi_width = 0, roi_height = 0;
  std::array<std::string, 2> ellipse_path;
  try {
    const auto &order = contract->at("ellipse_input").at("parameter_order");
    const auto &normalization = contract->at("ellipse_input").at("parameter_normalization");
    const auto &frame = contract->at("body_frame");
    const auto &array = row_identity->at("key_arrays").at(0);
    const auto &temporal = sa->at("source_row_temporal_authority")
                               .at("source_acquisition_frame_index");
    const auto &roi = binding->at("roi_raster_extent");
    roi_width = roi.at("width_px").get<size_t>();
    roi_height = roi.at("height_px").get<size_t>();
    if (order.get<std::vector<std::string>>() !=
          std::vector<std::string>({"center_x_px", "center_y_px",
                                    "major_axis_length_px", "minor_axis_length_px",
                                    "major_axis_angle_deg"}) ||
        normalization.get<std::string>() !=
          "cv2.fitEllipse axes reordered so major >= minor and major-axis angle normalized to [0, 180) degrees" ||
        frame.value("coordinate_space", "") != "roi_pixels" ||
        !IsLowerSha256(array.value("content_sha256", "")) ||
        array.value("content_sha256", "") !=
            ordered->value("shared_instance_key_content_sha256", "") ||
        temporal.value("content_sha256", "") !=
            ordered->value("shared_frame_index_content_sha256", "") ||
        row_identity->value("unique", false) != true ||
        binding->at("frame_axis").value("source_total_frames", size_t{0}) !=
          s.frame_count ||
        roi_width == 0 || roi_height == 0) {
      error(message, "Bound eye ellipse or row coordinate convention is unsupported");
      return nullptr;
    }
    for (size_t eye = 0; eye < 2; ++eye) {
      const std::string component = eye == 0 ? "eye_left" : "eye_right";
      const std::string relative = "components/" + component + "/ellipse_params";
      ellipse_path[eye] = sb + "/" + relative;
      const auto &entry = descriptors->at(relative);
      const auto &source_array = allowed_arrays->at(relative);
      const auto &success_array = allowed_arrays->at(
          "components/" + component + "/ellipse_success");
      const auto &component_source = contract->at("ellipse_input")
                                         .at("component_sources").at(eye);
      if (entry.value("record_ref", "") != "/" + ellipse_path[eye] +
                                           "@coordinate_descriptor" ||
          source_array.value("array_ref", "") != "/" + ellipse_path[eye] ||
          source_array.value("dtype", "") != "<f4" ||
          source_array.at("shape").get<std::vector<size_t>>() !=
              std::vector<size_t>({s.observation_count, 5}) ||
          !IsLowerSha256(source_array.value("content_sha256", "")) ||
          success_array.value("array_ref", "") != "/" + sb +
              "/components/" + component + "/ellipse_success" ||
          success_array.value("dtype", "") != "|b1" ||
          success_array.at("shape").get<std::vector<size_t>>() !=
              std::vector<size_t>({s.observation_count}) ||
          !IsLowerSha256(success_array.value("content_sha256", "")) ||
          component_source.value("component", "") != component ||
          component_source.value("ellipse_params_path", "") != ellipse_path[eye] ||
          component_source.value("ellipse_success_path", "") !=
              sb + "/components/" + component + "/ellipse_success" ||
          !IsLowerSha256(entry.value("descriptor_sha256", "")) ||
          !coordinateAuthority(archive, ellipse_path[eye], s,
                               entry.at("descriptor_sha256").get<std::string>(),
                               row_record_sha256)) {
        error(message, "Bound eye ellipse coordinate authority is invalid");
        return nullptr;
      }
    }
  } catch (const json::exception &) {
    error(message, "Bound eye scientific contract is malformed"); return nullptr;
  }
  const size_t n = s.observation_count;
  const auto *array_schema = obj(*ea, "eye_angle_array_schema");
  const auto *dimensions = array_schema ? obj(*array_schema, "dimensions") : nullptr;
  if (!dimensions || dimensions->value("n_roi_rows", size_t{0}) != n ||
      dimensions->value("n_frames", size_t{0}) != s.frame_count) {
    error(message, "Bound eye array dimensions disagree with selection"); return nullptr;
  }
  std::vector<std::string> bounded_paths = {
      mb + "/frame_row_offsets", mb + "/source_acquisition_frame_index",
      mb + "/instance_key", mb + "/source_crop_row_ids",
      mb + "/source_crop_xywh", sb + "/source_acquisition_frame_index",
      sb + "/instance_key", sb + "/source_crop_row_ids",
      eb + "/support/frame_indices",
      eb + "/support/source_acquisition_frame_index",
      eb + "/support/instance_key", eb + "/support/body_frame/valid",
      eb + "/support/body_frame/origin_xy",
      eb + "/support/body_frame/forward_axis_xy",
      eb + "/support/body_frame/left_axis_xy",
      eb + "/roi_angles", eb + "/roi_vectors", eb + "/roi_qa",
      eb + "/angle_channel_index/name",
      eb + "/angle_channel_index/roi_available",
      eb + "/vector_channel_index/name",
      eb + "/vector_channel_index/roi_available",
      eb + "/qa_channel_index/name", eb + "/qa_channel_index/roi_available"};
  for (size_t eye = 0; eye < 2; ++eye) {
    bounded_paths.push_back(ellipse_path[eye]);
    bounded_paths.push_back(sb + "/components/" +
        (eye == 0 ? "eye_left" : "eye_right") + "/ellipse_success");
  }
  for (const auto &path : bounded_paths)
    if (!boundedStorageChunk(archive, path, request.max_storage_chunk_bytes)) {
      error(message, "Bound eye storage chunk exceeds decoded limit: " + path);
      return nullptr;
    }
  std::vector<std::string> angle_names, vector_names, qa_names;
  if (!availableNames(archive, eb + "/angle_channel_index",
                      dimensions->value("n_angle_channels", size_t{0}),
                      &m, &angle_names, message) ||
      !availableNames(archive, eb + "/vector_channel_index",
                      dimensions->value("n_vector_channels", size_t{0}),
                      &m, &vector_names, message) ||
      !availableNames(archive, eb + "/qa_channel_index",
                      dimensions->value("n_qa_channels", size_t{0}),
                      &m, &qa_names, message)) {
    error(message, "Bound eye named channels are unavailable"); return nullptr;
  }
  Sources a;
  a.eye_angle = {channel(angle_names, "left_eye_angle_deg"),
                 channel(angle_names, "right_eye_angle_deg")};
  a.gaze_angle = {channel(angle_names, "left_gaze_signed_deg"),
                  channel(angle_names, "right_gaze_signed_deg")};
  a.gaze_vector = {channel(vector_names, "left_gaze_xy"),
                   channel(vector_names, "right_gaze_xy")};
  a.valid_eye = {channel(qa_names, "valid_left"), channel(qa_names, "valid_right")};
  a.vergence = channel(angle_names, "vergence_eye_angle_deg");
  a.valid_frame = channel(qa_names, "valid_frame");
  if (a.eye_angle[0] == SIZE_MAX || a.eye_angle[1] == SIZE_MAX ||
      a.gaze_angle[0] == SIZE_MAX || a.gaze_angle[1] == SIZE_MAX ||
      a.gaze_vector[0] == SIZE_MAX || a.gaze_vector[1] == SIZE_MAX ||
      a.valid_eye[0] == SIZE_MAX || a.valid_eye[1] == SIZE_MAX ||
      a.vergence == SIZE_MAX || a.valid_frame == SIZE_MAX) {
    error(message, "Bound eye required semantic channels are missing"); return nullptr;
  }
#define OPEN_TO(member, type, rank, path) \
  auto member##_opened = open<type, rank>(archive, path, &m, message); \
  if (!member##_opened) return nullptr; \
  a.member = std::move(*member##_opened)
  OPEN_TO(offsets, int64_t, 1, mb + "/frame_row_offsets");
  OPEN_TO(eye_frames, int64_t, 1, eb + "/support/frame_indices");
  OPEN_TO(eye_acquisition, int64_t, 1, eb + "/support/source_acquisition_frame_index");
  OPEN_TO(shape_frames, int64_t, 1, sb + "/source_acquisition_frame_index");
  OPEN_TO(mask_frames, int64_t, 1, mb + "/source_acquisition_frame_index");
  OPEN_TO(eye_keys, uint64_t, 1, eb + "/support/instance_key");
  OPEN_TO(shape_keys, uint64_t, 1, sb + "/instance_key");
  OPEN_TO(mask_keys, uint64_t, 1, mb + "/instance_key");
  OPEN_TO(shape_crop_rows, int64_t, 1, sb + "/source_crop_row_ids");
  OPEN_TO(mask_crop_rows, int64_t, 1, mb + "/source_crop_row_ids");
  OPEN_TO(crop, float, 2, mb + "/source_crop_xywh");
  OPEN_TO(body_valid, bool, 1, eb + "/support/body_frame/valid");
  OPEN_TO(angles, float, 2, eb + "/roi_angles");
  OPEN_TO(vectors, float, 3, eb + "/roi_vectors");
  OPEN_TO(qa, uint16_t, 2, eb + "/roi_qa");
#undef OPEN_TO
  for (size_t eye = 0; eye < 2; ++eye) {
    auto ell = open<float, 2>(archive, ellipse_path[eye], &m, message);
    auto valid = open<bool, 1>(archive,
        sb + "/components/" + (eye == 0 ? "eye_left" : "eye_right") +
        "/ellipse_success", &m, message);
    if (!ell || !valid) return nullptr;
    a.ellipse[eye] = std::move(*ell);
    a.ellipse_success[eye] = std::move(*valid);
  }
  for (size_t i = 0; i < 3; ++i) {
    auto store = open<float, 2>(archive, eb + "/support/body_frame/" +
        (i == 0 ? "origin_xy" : i == 1 ? "forward_axis_xy" : "left_axis_xy"),
        &m, message);
    if (!store) return nullptr;
    a.body[i] = std::move(*store);
  }
  const std::array<size_t, 1> rows{n};
  if (!shape(a.offsets, std::array<size_t, 1>{s.frame_count + 1}) ||
      !shape(a.eye_frames, rows) || !shape(a.eye_acquisition, rows) ||
      !shape(a.shape_frames, rows) || !shape(a.mask_frames, rows) ||
      !shape(a.eye_keys, rows) || !shape(a.shape_keys, rows) ||
      !shape(a.mask_keys, rows) || !shape(a.shape_crop_rows, rows) ||
      !shape(a.mask_crop_rows, rows) || !shape(a.crop, std::array<size_t, 2>{n, 4}) ||
      !shape(a.body_valid, rows) ||
      !shape(a.angles, std::array<size_t, 2>{n, angle_names.size()}) ||
      !shape(a.vectors, std::array<size_t, 3>{n, vector_names.size(), 2}) ||
      !shape(a.qa, std::array<size_t, 2>{n, qa_names.size()})) {
    error(message, "Bound eye array shapes disagree with selection"); return nullptr;
  }
  for (size_t eye = 0; eye < 2; ++eye)
    if (!shape(a.ellipse[eye], std::array<size_t, 2>{n, 5}) ||
        !shape(a.ellipse_success[eye], rows)) {
      error(message, "Bound eye ellipse shape disagrees with selection"); return nullptr;
    }
  for (const auto &body : a.body)
    if (!shape(body, std::array<size_t, 2>{n, 2})) {
      error(message, "Bound eye body-frame shape disagrees with selection"); return nullptr;
    }
  std::vector<int64_t> offsets;
  if (request.shared_mask_frame_index) {
    if (!request.shared_mask_frame_index->matches(request.archive, s) ||
        !request.shared_mask_frame_index->admits(request.max_observations_per_frame)) {
      error(message, "Bound eye shared frame index disagrees with source or observation limit");
      return nullptr;
    }
    m.reused_shared_mask_index = true;
  } else {
    if (!read(a.offsets, 0, s.frame_count + 1, &offsets, message)) return nullptr;
    ++m.offset_read_calls;
    m.retained_offset_bytes = offsets.capacity() * sizeof(int64_t);
  }
  const auto &validated_offsets = request.shared_mask_frame_index ?
      request.shared_mask_frame_index->offsets() : offsets;
  if (validated_offsets.size() != s.frame_count + 1 ||
      validated_offsets.front() != 0 ||
      validated_offsets.back() != static_cast<int64_t>(n) ||
      !std::is_sorted(validated_offsets.begin(), validated_offsets.end())) {
    error(message, "Bound eye frame offsets are invalid"); return nullptr;
  }
  for (size_t frame = 0; frame < s.frame_count; ++frame) {
    const auto count = validated_offsets[frame + 1] - validated_offsets[frame];
    if (count < 0 || static_cast<size_t>(count) > request.max_observations_per_frame) {
      error(message, "Bound eye frame observation limit exceeded"); return nullptr;
    }
    m.maximum_observations_per_frame = std::max(m.maximum_observations_per_frame,
                                                static_cast<size_t>(count));
  }
  EyeGeometryOverlayDescriptor d;
  d.source_group = s.eye.group; d.run_name = s.eye.run_id;
  d.source_subject_shape_run = s.shape.run_id;
  d.source_refined_subject_masks_run = s.mask.run_id;
  d.publication_identity_digest = s.eye.identity_digest;
  d.validated_instance_keys = true;
  d.schema_id = s.eye.schema_id; d.schema_version = 7;
  d.method = str(*ea, "method"); d.method_version = str(*ea, "method_version");
  d.row_axis = str(*ea, "row_axis"); d.layout = str(*ea, "layout");
  d.row_count = n; d.camera_frame_count = s.frame_count;
  d.coordinate_width = roi_width; d.coordinate_height = roi_height;
  if (open_metrics) *open_metrics = m;
  if (message) message->clear();
  return std::make_unique<Repository>(std::move(d), std::move(offsets),
                                      std::move(a), request);
}

} // namespace crimson::zarr
