#include "zarr/tensorstore_chaser_distance_polar_repository.h"

#include <tensorstore/box.h>
#include <tensorstore/index_space/index_transform.h>
#include <tensorstore/open.h>
#include <tensorstore/tensorstore.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "zarr/archive_context_internal.h"

namespace crimson::zarr {
namespace ts = tensorstore;
using json = nlohmann::json;

namespace {

using FloatMatrix =
    std::variant<ts::TensorStore<float, 2>, ts::TensorStore<double, 2>>;
using ValidMatrix = ts::TensorStore<bool, 2>;

struct Selection {
  std::string name;
  crimson::polar::ChaserDistancePolarSelectionProvenance provenance =
      crimson::polar::ChaserDistancePolarSelectionProvenance::Unspecified;
};

struct DatasetStores {
  std::shared_ptr<ArchiveContext> archive;
  FloatMatrix bearings;
  FloatMatrix distances;
  ValidMatrix valid;
  std::unordered_map<int64_t, size_t> row_by_frame;
  std::vector<int32_t> chaser_indices;
  std::vector<crimson::polar::ChaserDistancePolarColor> colors;
};

bool validName(const std::string& value) {
  return !value.empty() && value != "." && value != ".." &&
         value.find('/') == std::string::npos &&
         value.find('\\') == std::string::npos;
}

std::string stringValue(const json& object, const char* key) {
  const auto found = object.find(key);
  return found != object.end() && found->is_string() ? found->get<std::string>()
                                                     : std::string{};
}

bool arrayMetadataExists(const std::filesystem::path& root,
                         const std::string& path) {
  const auto base = root / path;
  return std::filesystem::is_regular_file(base / "zarr.json") ||
         std::filesystem::is_regular_file(base / ".zarray");
}

bool groupMetadataExists(const std::filesystem::path& root,
                         const std::string& path) {
  const auto base = root / path;
  return std::filesystem::is_directory(base) &&
         (std::filesystem::is_regular_file(base / "zarr.json") ||
          std::filesystem::is_regular_file(base / ".zgroup") ||
          std::filesystem::is_regular_file(base / ".zattrs"));
}

crimson::polar::ChaserDistancePolarSelectionProvenance provenanceForKey(
    std::string_view key) {
  using Provenance = crimson::polar::ChaserDistancePolarSelectionProvenance;
  if (key == "latest_complete") {
    return Provenance::LatestComplete;
  }
  if (key == "latest_completed") {
    return Provenance::LatestCompleted;
  }
  if (key == "latest_success") {
    return Provenance::LatestSuccess;
  }
  if (key == "latest") {
    return Provenance::Latest;
  }
  return Provenance::Unspecified;
}

Selection attributeSelection(const ArchiveContext::Impl& archive,
                             const std::string& group) {
  const auto attributes = internal::ReadArchiveAttributes(archive, group);
  if (!attributes) {
    return {};
  }
  constexpr std::array<std::string_view, 4> keys = {
      "latest_complete", "latest_completed", "latest_success", "latest"};
  for (const auto key : keys) {
    const auto found = attributes->find(std::string(key));
    if (found != attributes->end() && found->is_string()) {
      return {found->get<std::string>(), provenanceForKey(key)};
    }
  }
  return {};
}

Selection fallbackSelection(const std::filesystem::path& root,
                            const std::string& group,
                            const std::vector<std::string>& required_arrays) {
  const auto base = root / group;
  std::vector<std::string> candidates;
  std::error_code error;
  for (std::filesystem::directory_iterator iterator(base, error), end;
       !error && iterator != end; iterator.increment(error)) {
    if (!iterator->is_directory()) {
      continue;
    }
    const std::string name = iterator->path().filename().string();
    if (!validName(name)) {
      continue;
    }
    bool complete = true;
    for (const auto& required : required_arrays) {
      if (!arrayMetadataExists(root, group + "/" + name + "/" + required)) {
        complete = false;
        break;
      }
    }
    if (complete) {
      candidates.push_back(name);
    }
  }
  if (candidates.empty()) {
    return {};
  }
  std::sort(candidates.begin(), candidates.end());
  return {candidates.back(),
          crimson::polar::ChaserDistancePolarSelectionProvenance::
              LexicographicCompatibilityFallback};
}

Selection selectDataset(const ArchiveContext::Impl& archive,
                        const std::string& group, const std::string& requested,
                        const std::vector<std::string>& required_arrays) {
  if (!requested.empty()) {
    return {requested,
            crimson::polar::ChaserDistancePolarSelectionProvenance::Requested};
  }
  auto selected = attributeSelection(archive, group);
  if (!selected.name.empty()) {
    return selected;
  }
  return fallbackSelection(archive.root_path, group, required_arrays);
}

std::optional<json> makeArraySpec(const ArchiveContext::Impl& archive,
                                  const std::string& path) {
  const char* driver =
      std::filesystem::is_regular_file(archive.root_path / path / ".zarray")
          ? "zarr"
          : "zarr3";
  return internal::MakeReadOnlyArraySpec(archive, path, driver);
}

template <typename T, size_t Rank>
std::optional<ts::TensorStore<T, Rank>> openArray(
    const ArchiveContext::Impl& archive, const std::string& path) {
  const auto spec = makeArraySpec(archive, path);
  if (!spec) {
    return std::nullopt;
  }
  auto result = ts::Open<T, Rank>(*spec, ts::OpenMode::open,
                                  ts::ReadWriteMode::read, archive.context)
                    .result();
  return result.ok() ? std::optional<ts::TensorStore<T, Rank>>(*result)
                     : std::nullopt;
}

template <typename Output, typename Source>
Output clampInteger(Source value) {
  static_assert(std::is_integral_v<Output> && std::is_signed_v<Output>);
  static_assert(std::is_integral_v<Source>);
  if constexpr (std::is_signed_v<Source>) {
    if constexpr (sizeof(Source) > sizeof(Output)) {
      if (value < static_cast<Source>(std::numeric_limits<Output>::min())) {
        return std::numeric_limits<Output>::min();
      }
      if (value > static_cast<Source>(std::numeric_limits<Output>::max())) {
        return std::numeric_limits<Output>::max();
      }
    }
  } else if constexpr (sizeof(Source) >= sizeof(Output)) {
    if (value > static_cast<Source>(std::numeric_limits<Output>::max())) {
      return std::numeric_limits<Output>::max();
    }
  }
  return static_cast<Output>(value);
}

std::optional<FloatMatrix> openFloatMatrix(const ArchiveContext::Impl& archive,
                                           const std::string& path) {
  if (auto values = openArray<float, 2>(archive, path)) {
    return FloatMatrix{std::move(*values)};
  }
  if (auto values = openArray<double, 2>(archive, path)) {
    return FloatMatrix{std::move(*values)};
  }
  return std::nullopt;
}

template <typename T, ts::DimensionIndex Rank>
auto sliceRows(const ts::TensorStore<T, Rank>& store, size_t first,
               size_t last) {
  ts::Box<Rank> domain(store.domain().box());
  domain.origin()[0] += static_cast<ts::Index>(first);
  domain.shape()[0] = static_cast<ts::Index>(last - first);
  return store | ts::IdentityTransform(domain);
}

template <typename Store, typename Output>
bool readRankOne(const Store& store, std::vector<Output>* output) {
  const auto read = ts::Read(store).result();
  if (!read.ok() || read->rank() != 1 || read->shape()[0] < 0 ||
      read->byte_strides().size() != 1) {
    return false;
  }
  using Source = typename Store::Element;
  const size_t count = static_cast<size_t>(read->shape()[0]);
  output->resize(count);
  const auto* origin = reinterpret_cast<const uint8_t*>(
      read->byte_strided_origin_pointer().get());
  for (size_t index = 0; index < count; ++index) {
    const auto* value = reinterpret_cast<const Source*>(
        origin + static_cast<ts::Index>(index) * read->byte_strides()[0]);
    (*output)[index] = static_cast<Output>(*value);
  }
  return true;
}

template <typename Source, typename Output>
bool readIntegerRankOne(const ArchiveContext::Impl& archive,
                        const std::string& path, std::vector<Output>* output) {
  const auto store = openArray<Source, 1>(archive, path);
  if (!store) {
    return false;
  }
  std::vector<Source> source;
  if (!readRankOne(*store, &source)) {
    return false;
  }
  output->resize(source.size());
  std::transform(source.begin(), source.end(), output->begin(),
                 [](Source value) { return clampInteger<Output>(value); });
  return true;
}

bool readFrameIds(const ArchiveContext::Impl& archive, const std::string& path,
                  std::vector<int64_t>* output) {
  return readIntegerRankOne<int64_t>(archive, path, output) ||
         readIntegerRankOne<uint64_t>(archive, path, output) ||
         readIntegerRankOne<int32_t>(archive, path, output) ||
         readIntegerRankOne<uint32_t>(archive, path, output) ||
         readIntegerRankOne<int16_t>(archive, path, output) ||
         readIntegerRankOne<uint16_t>(archive, path, output) ||
         readIntegerRankOne<int8_t>(archive, path, output) ||
         readIntegerRankOne<uint8_t>(archive, path, output);
}

bool readChaserIds(const ArchiveContext::Impl& archive, const std::string& path,
                   std::vector<int32_t>* output) {
  return readIntegerRankOne<int32_t>(archive, path, output) ||
         readIntegerRankOne<uint32_t>(archive, path, output) ||
         readIntegerRankOne<int64_t>(archive, path, output) ||
         readIntegerRankOne<uint64_t>(archive, path, output) ||
         readIntegerRankOne<int16_t>(archive, path, output) ||
         readIntegerRankOne<uint16_t>(archive, path, output) ||
         readIntegerRankOne<int8_t>(archive, path, output) ||
         readIntegerRankOne<uint8_t>(archive, path, output);
}

template <typename Store, typename Output>
bool readMatrixRows(const Store& store, size_t first, size_t last,
                    std::vector<Output>* output) {
  if (last < first || last > static_cast<size_t>(store.domain().shape()[0])) {
    return false;
  }
  const auto read = ts::Read(sliceRows(store, first, last)).result();
  if (!read.ok() || read->rank() != 2 || read->shape()[0] < 0 ||
      read->shape()[1] < 0 || read->byte_strides().size() != 2) {
    return false;
  }
  using Source = typename Store::Element;
  const size_t rows = static_cast<size_t>(read->shape()[0]);
  const size_t columns = static_cast<size_t>(read->shape()[1]);
  output->resize(rows * columns);
  const auto* origin = reinterpret_cast<const uint8_t*>(
      read->byte_strided_origin_pointer().get());
  for (size_t row = 0; row < rows; ++row) {
    for (size_t column = 0; column < columns; ++column) {
      const auto* value = reinterpret_cast<const Source*>(
          origin + static_cast<ts::Index>(row) * read->byte_strides()[0] +
          static_cast<ts::Index>(column) * read->byte_strides()[1]);
      (*output)[row * columns + column] = static_cast<Output>(*value);
    }
  }
  return true;
}

bool readFloatRows(const FloatMatrix& store, size_t first, size_t last,
                   std::vector<double>* output) {
  return std::visit(
      [&](const auto& typed) {
        return readMatrixRows(typed, first, last, output);
      },
      store);
}

std::array<size_t, 2> matrixShape(const FloatMatrix& store) {
  return std::visit(
      [](const auto& typed) {
        return std::array<size_t, 2>{
            typed.domain().shape()[0] > 0
                ? static_cast<size_t>(typed.domain().shape()[0])
                : 0,
            typed.domain().shape()[1] > 0
                ? static_cast<size_t>(typed.domain().shape()[1])
                : 0};
      },
      store);
}

std::array<size_t, 2> matrixShape(const ValidMatrix& store) {
  return {store.domain().shape()[0] > 0
              ? static_cast<size_t>(store.domain().shape()[0])
              : 0,
          store.domain().shape()[1] > 0
              ? static_cast<size_t>(store.domain().shape()[1])
              : 0};
}

int hexNibble(char value) {
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  if (value >= 'a' && value <= 'f') {
    return value - 'a' + 10;
  }
  if (value >= 'A' && value <= 'F') {
    return value - 'A' + 10;
  }
  return -1;
}

std::optional<crimson::polar::ChaserDistancePolarRgba> parseHexColor(
    std::string text) {
  if (!text.empty() && text.front() == '#') {
    text.erase(text.begin());
  }
  if (text.size() != 6 && text.size() != 8) {
    return std::nullopt;
  }
  auto byte = [&](size_t offset) -> std::optional<int> {
    const int high = hexNibble(text[offset]);
    const int low = hexNibble(text[offset + 1]);
    return high < 0 || low < 0 ? std::nullopt
                               : std::optional<int>(high * 16 + low);
  };
  const auto red = byte(0);
  const auto green = byte(2);
  const auto blue = byte(4);
  const auto alpha = text.size() == 8 ? byte(6) : std::optional<int>(255);
  if (!red || !green || !blue || !alpha) {
    return std::nullopt;
  }
  return crimson::polar::ChaserDistancePolarRgba{*red / 255.0, *green / 255.0,
                                                 *blue / 255.0, *alpha / 255.0};
}

std::unordered_map<int32_t, crimson::polar::ChaserDistancePolarRgba>
componentColors(const json& attributes) {
  std::unordered_map<int32_t, crimson::polar::ChaserDistancePolarRgba> result;
  const auto summary = attributes.find("summary");
  if (summary == attributes.end() || !summary->is_object()) {
    return result;
  }
  const auto colors = summary->find("chaser_color_hex");
  if (colors == summary->end() || !colors->is_object()) {
    return result;
  }
  for (auto iterator = colors->begin(); iterator != colors->end(); ++iterator) {
    if (!iterator.value().is_string()) {
      continue;
    }
    try {
      size_t parsed = 0;
      const long long index = std::stoll(iterator.key(), &parsed);
      if (parsed != iterator.key().size() || index < 0 ||
          index > std::numeric_limits<int32_t>::max()) {
        continue;
      }
      if (auto color = parseHexColor(iterator.value().get<std::string>())) {
        result[static_cast<int32_t>(index)] = *color;
      }
    } catch (const std::exception&) {
    }
  }
  return result;
}

std::optional<json> objectValue(const json& value) {
  if (value.is_object()) {
    return value;
  }
  if (!value.is_string()) {
    return std::nullopt;
  }
  const json parsed = json::parse(value.get<std::string>(), nullptr, false);
  return parsed.is_object() ? std::optional<json>(parsed) : std::nullopt;
}

std::optional<double> finiteNumber(const json& object, const std::string& key) {
  const auto found = object.find(key);
  if (found == object.end() || !found->is_number()) {
    return std::nullopt;
  }
  const double value = found->get<double>();
  return std::isfinite(value) ? std::optional<double>(value) : std::nullopt;
}

std::optional<crimson::polar::ChaserDistancePolarRgba> protocolColor(
    const json& object) {
  const auto red = finiteNumber(object, "color_r");
  const auto green = finiteNumber(object, "color_g");
  const auto blue = finiteNumber(object, "color_b");
  if (!red || !green || !blue) {
    return std::nullopt;
  }
  const auto alpha = finiteNumber(object, "color_a");
  return crimson::polar::ChaserDistancePolarRgba{
      std::clamp(*red, 0.0, 1.0), std::clamp(*green, 0.0, 1.0),
      std::clamp(*blue, 0.0, 1.0), alpha ? std::clamp(*alpha, 0.0, 1.0) : 1.0};
}

int32_t protocolChaserIndex(const json& object, size_t fallback) {
  for (const char* key : {"chaser_index", "index"}) {
    const auto found = object.find(key);
    if (found != object.end() && found->is_number_integer()) {
      const int64_t value = found->get<int64_t>();
      if (value >= 0 && value <= std::numeric_limits<int32_t>::max()) {
        return static_cast<int32_t>(value);
      }
    }
  }
  return fallback <= static_cast<size_t>(std::numeric_limits<int32_t>::max())
             ? static_cast<int32_t>(fallback)
             : -1;
}

void collectParameterColors(
    const json& parameters,
    std::unordered_map<int32_t, crimson::polar::ChaserDistancePolarRgba>*
        output) {
  const auto chasers = parameters.find("chasers");
  if (chasers == parameters.end() || !chasers->is_array()) {
    return;
  }
  for (size_t index = 0; index < chasers->size(); ++index) {
    const auto& chaser = (*chasers)[index];
    if (!chaser.is_object()) {
      continue;
    }
    const int32_t chaser_index = protocolChaserIndex(chaser, index);
    if (chaser_index < 0 || output->find(chaser_index) != output->end()) {
      continue;
    }
    if (auto color = protocolColor(chaser)) {
      output->emplace(chaser_index, *color);
    }
  }
}

void collectProtocolColors(
    const json& protocol,
    std::unordered_map<int32_t, crimson::polar::ChaserDistancePolarRgba>*
        output) {
  if (!protocol.is_object()) {
    return;
  }
  collectParameterColors(protocol, output);
  const auto parameters = protocol.find("parameters");
  if (parameters != protocol.end()) {
    if (auto object = objectValue(*parameters)) {
      collectParameterColors(*object, output);
    }
  }
  const auto steps = protocol.find("steps");
  if (steps != protocol.end() && steps->is_array()) {
    for (const auto& step : *steps) {
      collectProtocolColors(step, output);
    }
  }
}

std::string stimulusRun(const ArchiveContext::Impl& archive,
                        const std::string& requested) {
  if (!requested.empty()) {
    return validName(requested) ? requested : std::string{};
  }
  const auto attributes =
      internal::ReadArchiveAttributes(archive, "analysis/stimulus_runs");
  if (attributes) {
    constexpr std::array<const char*, 4> keys = {
        "latest", "latest_completed", "latest_complete", "latest_success"};
    for (const char* key : keys) {
      const auto found = attributes->find(key);
      if (found != attributes->end() && found->is_string()) {
        const std::string selected = found->get<std::string>();
        if (!selected.empty()) {
          return validName(selected) ? selected : std::string{};
        }
        break;
      }
    }
  }
  const auto fallback =
      fallbackSelection(archive.root_path, "analysis/stimulus_runs", {});
  return fallback.name;
}

std::unordered_map<int32_t, crimson::polar::ChaserDistancePolarRgba>
stimulusProtocolColors(const ArchiveContext::Impl& archive,
                       const std::string& requested_run) {
  std::unordered_map<int32_t, crimson::polar::ChaserDistancePolarRgba> result;
  const std::string run = stimulusRun(archive, requested_run);
  if (run.empty()) {
    return result;
  }
  const auto attributes =
      internal::ReadArchiveAttributes(archive, "analysis/stimulus_runs/" + run);
  if (!attributes) {
    return result;
  }
  const auto protocol_text = attributes->find("protocol_json");
  if (protocol_text == attributes->end() || !protocol_text->is_string()) {
    return result;
  }
  const json protocol =
      json::parse(protocol_text->get<std::string>(), nullptr, false);
  collectProtocolColors(protocol, &result);
  return result;
}

class TensorStoreRepository final
    : public TensorStoreChaserDistancePolarRepository {
 public:
  TensorStoreRepository(
      crimson::polar::ChaserDistancePolarDescriptor descriptor,
      std::optional<DatasetStores> stores,
      TensorStoreChaserDistancePolarMetrics metrics)
      : descriptor_(std::move(descriptor)),
        stores_(std::move(stores)),
        metrics_(metrics) {}

  const crimson::polar::ChaserDistancePolarDescriptor& descriptor()
      const override {
    return descriptor_;
  }

  crimson::polar::ChaserDistancePolarFrameSample resolveCameraFrame(
      int64_t camera_frame) const override {
    if (!descriptor_.ready() || !stores_) {
      return crimson::polar::makeChaserDistancePolarFrameSample(
          descriptor_, camera_frame, std::nullopt, {});
    }
    const auto found = stores_->row_by_frame.find(camera_frame);
    if (found == stores_->row_by_frame.end()) {
      return crimson::polar::makeChaserDistancePolarFrameSample(
          descriptor_, camera_frame, std::nullopt, {});
    }

    std::vector<double> bearings;
    std::vector<double> distances;
    std::vector<uint8_t> valid;
    const size_t first = found->second;
    const size_t last = first + 1;
    const bool read =
        readFloatRows(stores_->bearings, first, last, &bearings) &&
        readFloatRows(stores_->distances, first, last, &distances) &&
        readMatrixRows(stores_->valid, first, last, &valid);
    {
      std::lock_guard<std::mutex> lock(metrics_mutex_);
      ++metrics_.exact_frame_rows_read;
      metrics_.matrix_read_operations += 3;
      metrics_.maximum_rows_per_matrix_read =
          std::max<size_t>(metrics_.maximum_rows_per_matrix_read, 1);
    }
    if (!read || bearings.size() != stores_->chaser_indices.size() ||
        distances.size() != stores_->chaser_indices.size() ||
        valid.size() != stores_->chaser_indices.size()) {
      return crimson::polar::makeChaserDistancePolarFrameSample(
          descriptor_, camera_frame, camera_frame, {},
          "failed to read the exact chaser-distance polar matrix row");
    }

    std::vector<crimson::polar::ChaserDistancePolarPoint> points;
    points.reserve(stores_->chaser_indices.size());
    for (size_t column = 0; column < stores_->chaser_indices.size(); ++column) {
      crimson::polar::ChaserDistancePolarPoint point;
      point.chaser_index = stores_->chaser_indices[column];
      point.distance_mm = distances[column];
      point.bearing_degrees = bearings[column];
      point.valid = valid[column] != 0;
      point.color = stores_->colors[column];
      points.push_back(std::move(point));
    }
    return crimson::polar::makeChaserDistancePolarFrameSample(
        descriptor_, camera_frame, camera_frame, std::move(points));
  }

  TensorStoreChaserDistancePolarMetrics metrics() const override {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    return metrics_;
  }

 private:
  crimson::polar::ChaserDistancePolarDescriptor descriptor_;
  std::optional<DatasetStores> stores_;
  mutable std::mutex metrics_mutex_;
  mutable TensorStoreChaserDistancePolarMetrics metrics_;
};

std::unique_ptr<TensorStoreChaserDistancePolarRepository> descriptorOnly(
    crimson::polar::ChaserDistancePolarDescriptor descriptor,
    TensorStoreChaserDistancePolarMetrics metrics = {}) {
  return std::make_unique<TensorStoreRepository>(std::move(descriptor),
                                                 std::nullopt, metrics);
}

}  // namespace

std::unique_ptr<TensorStoreChaserDistancePolarRepository>
OpenTensorStoreChaserDistancePolarRepository(
    const std::shared_ptr<ArchiveContext>& archive,
    const TensorStoreChaserDistancePolarOptions& options,
    std::string* error_message) {
  if (!archive || !archive->impl_) {
    internal::SetArchiveError(error_message, "Archive context is unavailable");
    return nullptr;
  }
  if (options.radial_scan_block_rows == 0) {
    internal::SetArchiveError(error_message,
                              "Polar radial scan block size must be positive");
    return nullptr;
  }
  const auto& impl = *archive->impl_;
  const std::string group =
      std::string(crimson::polar::kChaserDistancePolarSourceGroup);
  crimson::polar::ChaserDistancePolarDescriptor descriptor;
  descriptor.provenance.source_group = group;
  auto finish = [&](std::unique_ptr<TensorStoreChaserDistancePolarRepository>
                        repository) {
    if (error_message != nullptr) {
      error_message->clear();
    }
    return repository;
  };
  auto fail = [&](crimson::polar::ChaserDistancePolarAvailability availability,
                  std::string error,
                  TensorStoreChaserDistancePolarMetrics metrics = {}) {
    descriptor.availability = availability;
    descriptor.error = std::move(error);
    return finish(descriptorOnly(descriptor, metrics));
  };

  if (!groupMetadataExists(impl.root_path, group)) {
    return fail(
        crimson::polar::ChaserDistancePolarAvailability::DatasetUnavailable,
        "chaser-distance polar group is unavailable");
  }
  const auto run =
      selectDataset(impl, group, options.requested_run,
                    {"frames/camera_frame_id", "distances/distance_mm"});
  descriptor.provenance.run_name = run.name;
  descriptor.provenance.run_selection = run.provenance;
  if (run.name.empty()) {
    return fail(
        crimson::polar::ChaserDistancePolarAvailability::DatasetUnavailable,
        "no chaser-distance polar run was selected");
  }
  if (!validName(run.name)) {
    return fail(
        crimson::polar::ChaserDistancePolarAvailability::UnsupportedMetadata,
        "selected chaser-distance polar run name is unsafe");
  }

  const std::string run_base = group + "/" + run.name;
  const auto run_attributes = internal::ReadArchiveAttributes(impl, run_base);
  descriptor.coordinate_frame =
      run_attributes ? stringValue(*run_attributes, "coordinate_frame") : "";

  const std::string component_group = run_base + "/egocentric_bearing";
  const auto component = selectDataset(
      impl, component_group, options.requested_component,
      {"per_chaser/bearing_deg", "per_chaser/distance_mm", "per_chaser/valid"});
  descriptor.provenance.component_name = component.name;
  descriptor.provenance.component_selection = component.provenance;
  if (component.name.empty()) {
    return fail(
        crimson::polar::ChaserDistancePolarAvailability::DatasetUnavailable,
        "no egocentric-bearing component was selected");
  }
  if (!validName(component.name)) {
    return fail(
        crimson::polar::ChaserDistancePolarAvailability::UnsupportedMetadata,
        "selected egocentric-bearing component name is unsafe");
  }

  const std::string component_base = component_group + "/" + component.name;
  const auto component_attributes =
      internal::ReadArchiveAttributes(impl, component_base);
  if (component_attributes) {
    descriptor.angle_convention =
        stringValue(*component_attributes, "angle_convention");
    if (descriptor.angle_convention.empty()) {
      const auto parameters = component_attributes->find("parameters");
      if (parameters != component_attributes->end() &&
          parameters->is_object()) {
        descriptor.angle_convention =
            stringValue(*parameters, "angle_convention");
      }
    }
  }

  const std::string component_frames =
      component_base + "/frames/camera_frame_id";
  const std::string run_frames = run_base + "/frames/camera_frame_id";
  const std::string frame_path =
      arrayMetadataExists(impl.root_path, component_frames) ? component_frames
                                                            : run_frames;
  const std::string component_chasers =
      component_base + "/per_chaser/chaser_index";
  const std::string run_chasers = run_base + "/chasers/chaser_index";
  const std::string chaser_path =
      arrayMetadataExists(impl.root_path, component_chasers) ? component_chasers
                                                             : run_chasers;
  const std::string bearing_path = component_base + "/per_chaser/bearing_deg";
  const std::string distance_path = component_base + "/per_chaser/distance_mm";
  const std::string valid_path = component_base + "/per_chaser/valid";
  for (const auto& required :
       {frame_path, chaser_path, bearing_path, distance_path, valid_path}) {
    if (!arrayMetadataExists(impl.root_path, required)) {
      return fail(
          crimson::polar::ChaserDistancePolarAvailability::DatasetUnavailable,
          "required polar array is unavailable: " + required);
    }
  }

  auto bearings = openFloatMatrix(impl, bearing_path);
  auto distances = openFloatMatrix(impl, distance_path);
  auto valid = openArray<bool, 2>(impl, valid_path);
  if (!bearings || !distances || !valid) {
    return fail(
        crimson::polar::ChaserDistancePolarAvailability::UnsupportedMetadata,
        "polar array rank or data type is unsupported");
  }

  std::vector<int64_t> frame_ids;
  std::vector<int32_t> chaser_indices;
  if (!readFrameIds(impl, frame_path, &frame_ids) ||
      !readChaserIds(impl, chaser_path, &chaser_indices)) {
    return fail(crimson::polar::ChaserDistancePolarAvailability::ReadFailed,
                "failed to read polar frame or chaser identity arrays");
  }
  TensorStoreChaserDistancePolarMetrics metrics;
  metrics.frame_index_rows_read = frame_ids.size();
  metrics.chaser_index_rows_read = chaser_indices.size();
  metrics.resident_frame_ids = frame_ids.size();
  metrics.resident_chaser_ids = chaser_indices.size();
  descriptor.row_count = frame_ids.size();
  descriptor.chaser_count = chaser_indices.size();
  descriptor.distance_unit =
      crimson::polar::ChaserDistancePolarDistanceUnit::Millimeters;

  const auto bearing_shape = matrixShape(*bearings);
  const auto distance_shape = matrixShape(*distances);
  const auto valid_shape = matrixShape(*valid);
  const std::array<size_t, 2> expected = {descriptor.row_count,
                                          descriptor.chaser_count};
  if (descriptor.row_count == 0 || descriptor.chaser_count == 0 ||
      bearing_shape != expected || distance_shape != expected ||
      valid_shape != expected) {
    return fail(
        crimson::polar::ChaserDistancePolarAvailability::UnsupportedMetadata,
        "polar array shapes do not match frame and chaser identities", metrics);
  }

  double radial_max = 0.0;
  for (size_t first = 0; first < descriptor.row_count;) {
    const size_t last =
        std::min(descriptor.row_count, first + options.radial_scan_block_rows);
    std::vector<double> distance_values;
    std::vector<uint8_t> valid_values;
    if (!readFloatRows(*distances, first, last, &distance_values) ||
        !readMatrixRows(*valid, first, last, &valid_values) ||
        distance_values.size() != valid_values.size()) {
      return fail(crimson::polar::ChaserDistancePolarAvailability::ReadFailed,
                  "failed during bounded polar radial-maximum scan", metrics);
    }
    metrics.radial_scan_rows += last - first;
    metrics.matrix_read_operations += 2;
    metrics.maximum_rows_per_matrix_read =
        std::max(metrics.maximum_rows_per_matrix_read, last - first);
    for (size_t index = 0; index < distance_values.size(); ++index) {
      if (valid_values[index] != 0 && std::isfinite(distance_values[index]) &&
          distance_values[index] > radial_max) {
        radial_max = distance_values[index];
      }
    }
    first = last;
  }
  descriptor.dataset_global_max_distance_mm = radial_max;
  descriptor.availability =
      crimson::polar::ChaserDistancePolarAvailability::Ready;
  descriptor =
      crimson::polar::normalizeChaserDistancePolarDescriptor(descriptor);
  if (!descriptor.ready()) {
    return finish(descriptorOnly(descriptor, metrics));
  }

  std::unordered_map<int64_t, size_t> row_by_frame;
  row_by_frame.reserve(frame_ids.size());
  for (size_t row = 0; row < frame_ids.size(); ++row) {
    row_by_frame.emplace(frame_ids[row], row);
  }
  const auto protocol_colors =
      stimulusProtocolColors(impl, options.requested_stimulus_run);
  const auto summary_colors =
      component_attributes
          ? componentColors(*component_attributes)
          : std::unordered_map<int32_t,
                               crimson::polar::ChaserDistancePolarRgba>{};
  std::vector<crimson::polar::ChaserDistancePolarColor> colors;
  colors.reserve(chaser_indices.size());
  for (const int32_t chaser_index : chaser_indices) {
    const auto protocol = protocol_colors.find(chaser_index);
    const auto summary = summary_colors.find(chaser_index);
    colors.push_back(crimson::polar::resolveChaserDistancePolarColor(
        chaser_index,
        protocol == protocol_colors.end()
            ? std::nullopt
            : std::optional<crimson::polar::ChaserDistancePolarRgba>(
                  protocol->second),
        summary == summary_colors.end()
            ? std::nullopt
            : std::optional<crimson::polar::ChaserDistancePolarRgba>(
                  summary->second)));
  }

  DatasetStores stores{
      archive,           std::move(*bearings),    std::move(*distances),
      std::move(*valid), std::move(row_by_frame), std::move(chaser_indices),
      std::move(colors)};
  return finish(std::make_unique<TensorStoreRepository>(
      std::move(descriptor), std::move(stores), metrics));
}

}  // namespace crimson::zarr
