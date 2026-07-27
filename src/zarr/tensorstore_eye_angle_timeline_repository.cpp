#include "zarr/tensorstore_eye_angle_timeline_repository.h"

#include <tensorstore/box.h>
#include <tensorstore/index_space/index_transform.h>
#include <tensorstore/open.h>
#include <tensorstore/tensorstore.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "zarr/archive_context_internal.h"

namespace crimson::zarr {
namespace ts = tensorstore;
using json = nlohmann::json;

namespace {

std::optional<json> makeArraySpec(const ArchiveContext::Impl& archive,
                                  const std::string& path) {
  return internal::MakeReadOnlyArraySpec(archive, path);
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

template <typename T, ts::DimensionIndex Rank>
auto sliceRows(const ts::TensorStore<T, Rank>& store, ts::Index start,
               ts::Index stop) {
  ts::Box<Rank> domain(store.domain().box());
  domain.origin()[0] = start;
  domain.shape()[0] = stop - start;
  return store | ts::IdentityTransform(domain);
}

std::string stringValue(const json& attributes, const char* key) {
  const auto found = attributes.find(key);
  return found != attributes.end() && found->is_string()
             ? found->get<std::string>()
             : std::string{};
}

int integerValue(const json& attributes, const char* key) {
  const auto found = attributes.find(key);
  return found != attributes.end() && found->is_number_integer()
             ? found->get<int>()
             : 0;
}

std::string methodVersion(const json& attributes) {
  const auto found = attributes.find("method_version");
  if (found == attributes.end()) {
    return {};
  }
  return found->is_string() ? found->get<std::string>() : found->dump();
}

bool validRunName(const std::string& name) {
  return !name.empty() && name != "." && name != ".." &&
         name.find('/') == std::string::npos;
}

std::string latestRun(const ArchiveContext::Impl& archive,
                      const std::string& group) {
  const auto attributes = internal::ReadArchiveAttributes(archive, group);
  if (!attributes) {
    return {};
  }
  constexpr std::array<const char*, 4> keys = {
      "latest_complete", "latest_completed", "latest", "latest_success"};
  for (const char* key : keys) {
    const std::string value = stringValue(*attributes, key);
    if (!value.empty()) {
      return value;
    }
  }
  return {};
}

template <typename Source>
bool readBoolVector(const ArchiveContext::Impl& archive,
                    const std::string& path, std::vector<uint8_t>* output) {
  const auto store = openArray<Source, 1>(archive, path);
  if (!store) {
    return false;
  }
  const auto read = ts::Read(*store).result();
  if (!read.ok() || read->rank() != 1) {
    return false;
  }
  const size_t count = static_cast<size_t>(read->shape()[0]);
  const auto* values = static_cast<const Source*>(read->data());
  output->resize(count);
  for (size_t index = 0; index < count; ++index) {
    (*output)[index] = values[index] ? 1 : 0;
  }
  return true;
}

bool readBools(const ArchiveContext::Impl& archive, const std::string& path,
               std::vector<uint8_t>* output) {
  return readBoolVector<bool>(archive, path, output) ||
         readBoolVector<uint8_t>(archive, path, output) ||
         readBoolVector<uint16_t>(archive, path, output);
}

std::vector<std::string> readNames(const ArchiveContext::Impl& archive,
                                   const std::string& path) {
  std::vector<std::string> names;
  const auto store = openArray<uint8_t, 2>(archive, path);
  if (!store) {
    return names;
  }
  const auto read = ts::Read(*store).result();
  if (!read.ok() || read->rank() != 2) {
    return names;
  }
  const size_t rows = static_cast<size_t>(read->shape()[0]);
  const size_t columns = static_cast<size_t>(read->shape()[1]);
  const auto* values = static_cast<const uint8_t*>(read->data());
  names.reserve(rows);
  for (size_t row = 0; row < rows; ++row) {
    const char* text = reinterpret_cast<const char*>(values + row * columns);
    size_t length = 0;
    while (length < columns && text[length] != '\0') {
      ++length;
    }
    names.emplace_back(text, length);
  }
  return names;
}

std::unordered_map<std::string, size_t> availableFrameChannels(
    const ArchiveContext::Impl& archive, const std::string& base) {
  const auto names = readNames(archive, base + "/name");
  std::vector<uint8_t> available;
  readBools(archive, base + "/frame_available", &available);
  std::unordered_map<std::string, size_t> result;
  for (size_t index = 0; index < names.size(); ++index) {
    if (!names[index].empty() &&
        (available.empty() ||
         (index < available.size() && available[index] != 0))) {
      result[names[index]] = index;
    }
  }
  return result;
}

std::vector<std::string> stringVector(const json& object, const char* key) {
  std::vector<std::string> values;
  const auto found = object.find(key);
  if (found == object.end() || !found->is_array()) {
    return values;
  }
  for (const auto& value : *found) {
    if (value.is_string()) {
      values.push_back(value.get<std::string>());
    }
  }
  return values;
}

bool endsWith(const std::string& value, const std::string& suffix) {
  return value.size() >= suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) ==
             0;
}

std::string unsmoothedName(const std::string& name) {
  constexpr const char* suffix = "_smoothed";
  return endsWith(name, suffix)
             ? name.substr(0,
                           name.size() - std::char_traits<char>::length(suffix))
             : std::string{};
}

std::string humanFieldName(const std::string& source_name,
                           crimson::timeline::EyeAngleTraceRole role) {
  switch (role) {
    case crimson::timeline::EyeAngleTraceRole::Left:
      return "Left";
    case crimson::timeline::EyeAngleTraceRole::Right:
      return "Right";
    case crimson::timeline::EyeAngleTraceRole::Vergence:
      return "Vergence";
    case crimson::timeline::EyeAngleTraceRole::Other:
      break;
  }
  std::string label = source_name;
  std::replace(label.begin(), label.end(), '_', ' ');
  if (!label.empty() && label.front() >= 'a' && label.front() <= 'z') {
    label.front() = static_cast<char>(label.front() - 'a' + 'A');
  }
  return label;
}

std::optional<crimson::timeline::EyeAngleTimelineFieldDescriptor> resolveField(
    const std::string& requested,
    const std::unordered_map<std::string, size_t>& channels,
    const json& field_metadata, const std::string& default_units) {
  std::string source = requested;
  bool fallback = false;
  if (channels.find(source) == channels.end()) {
    source = unsmoothedName(requested);
    fallback = !source.empty();
  }
  if (source.empty() || channels.find(source) == channels.end()) {
    return std::nullopt;
  }
  const auto role = crimson::timeline::eyeAngleTraceRoleForField(source);
  std::string units = default_units.empty() ? "deg" : default_units;
  const auto metadata = field_metadata.find(source);
  if (metadata != field_metadata.end() && metadata->is_object()) {
    const std::string metadata_units = stringValue(*metadata, "units");
    if (!metadata_units.empty()) {
      units = metadata_units;
    }
  }
  crimson::timeline::EyeAngleTimelineFieldDescriptor result;
  result.requested_name = requested;
  result.source_name = source;
  result.display_name = humanFieldName(source, role);
  result.units = units;
  result.role = role;
  result.fallback = fallback;
  return result;
}

std::vector<std::string> representationFields(const json& representation) {
  auto fields = stringVector(representation, "default_plot_fields");
  if (!fields.empty()) {
    return fields;
  }
  const auto primary = stringVector(representation, "primary_roi_fields");
  const auto aggregate = stringVector(representation, "aggregate_roi_fields");
  std::unordered_set<std::string> seen;
  for (const auto& field : primary) {
    if (seen.insert(field).second) {
      fields.push_back(field + "_smoothed");
    }
  }
  for (const auto& field : aggregate) {
    if (seen.insert(field).second) {
      fields.push_back(field + "_smoothed");
    }
  }
  return fields;
}

struct FieldChannel {
  crimson::timeline::EyeAngleTimelineFieldDescriptor descriptor;
  size_t channel = 0;
};

class TensorStoreTimelineRepository final
    : public crimson::timeline::EyeAngleTimelineRepository {
 public:
  TensorStoreTimelineRepository(
      crimson::timeline::EyeAngleTimelineDescriptor descriptor,
      ts::TensorStore<float, 2> frame_angles,
      std::optional<ts::TensorStore<float, 1>> frame_times,
      std::unordered_map<std::string, size_t> channels,
      crimson::data::SmallSeriesPreloadPolicy preload_policy)
      : descriptor_(std::move(descriptor)),
        frame_angles_(std::move(frame_angles)),
        frame_times_(std::move(frame_times)),
        channels_(std::move(channels)),
        frame_column_count_(
            static_cast<size_t>(frame_angles_.domain().shape()[1])) {
    std::vector<size_t> preload_channels;
    std::unordered_set<size_t> unique_channels;
    const auto* default_representation =
        crimson::timeline::findEyeAngleTimelineRepresentation(
            descriptor_, descriptor_.default_representation);
    if (default_representation != nullptr) {
      for (const auto& field : default_representation->fields) {
        const auto channel = channels_.find(field.source_name);
        if (channel != channels_.end() &&
            unique_channels.insert(channel->second).second) {
          preload_channels.push_back(channel->second);
        }
      }
    }
    metrics_.preload_candidate_bytes =
        static_cast<uint64_t>(descriptor_.frame_count) *
        preload_channels.size() * sizeof(float);
    if (frame_times_) {
      metrics_.preload_candidate_bytes +=
          static_cast<uint64_t>(descriptor_.frame_count) * sizeof(float);
    }
    if (!preload_channels.empty() &&
        preload_policy.admits(metrics_.preload_candidate_bytes)) {
      const auto started = std::chrono::steady_clock::now();
      std::unordered_map<size_t, std::vector<float>> angles;
      std::optional<std::vector<float>> times;
      bool ready = readSelectedChannels(preload_channels, &angles);
      if (ready && frame_times_) {
        std::vector<float> retained_times;
        ready = readAllTimes(&retained_times);
        if (ready) {
          times = std::move(retained_times);
        }
      }
      metrics_.preload_ms = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - started)
                                .count();
      if (ready) {
        preloaded_channels_ = std::move(angles);
        preloaded_times_ = std::move(times);
        preloaded_representation_ = descriptor_.default_representation;
        metrics_.frame_series_preloaded = true;
        for (const auto& channel : preloaded_channels_) {
          metrics_.preloaded_retained_bytes +=
              static_cast<uint64_t>(channel.second.capacity()) * sizeof(float);
        }
        if (preloaded_times_) {
          metrics_.preloaded_retained_bytes +=
              static_cast<uint64_t>(preloaded_times_->capacity()) *
              sizeof(float);
        }
      }
    }
  }

  const crimson::timeline::EyeAngleTimelineDescriptor& descriptor()
      const override {
    return descriptor_;
  }

  crimson::timeline::EyeAngleTimelineRepositoryMetrics metrics()
      const override {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    return metrics_;
  }

  crimson::timeline::EyeAngleTimelineWindow resolveWindow(
      const crimson::timeline::EyeAngleTimelineRequest& request)
      const override {
    crimson::timeline::EyeAngleTimelineWindow failure;
    failure.request = request;
    const auto* representation =
        crimson::timeline::findEyeAngleTimelineRepresentation(
            descriptor_, request.representation_key);
    if (representation == nullptr || request.first_frame < 0 ||
        request.last_frame < request.first_frame ||
        request.last_frame >= static_cast<int64_t>(descriptor_.frame_count)) {
      failure.status =
          representation == nullptr
              ? crimson::timeline::EyeAngleTimelineStatus::InvalidRequest
              : crimson::timeline::EyeAngleTimelineStatus::OutOfRange;
      failure.error = "Eye-angle timeline window is invalid or out of range";
      return failure;
    }
    const size_t row_count =
        static_cast<size_t>(request.last_frame - request.first_frame + 1);
    const bool use_preloaded =
        request.representation_key == preloaded_representation_ &&
        !preloaded_channels_.empty();
    {
      std::lock_guard<std::mutex> lock(metrics_mutex_);
      if (use_preloaded) {
        ++metrics_.preloaded_window_resolves;
      } else {
        ++metrics_.paged_window_resolves;
      }
    }
    std::vector<crimson::timeline::EyeAngleTimelineFieldSeries> fields;
    fields.reserve(representation->fields.size());
    if (use_preloaded) {
      for (const auto& field : representation->fields) {
        const auto channel = channels_.find(field.source_name);
        if (channel == channels_.end()) {
          continue;
        }
        const auto retained = preloaded_channels_.find(channel->second);
        if (retained == preloaded_channels_.end() ||
            retained->second.size() != descriptor_.frame_count) {
          continue;
        }
        crimson::timeline::EyeAngleTimelineFieldSeries series;
        series.source_name = field.source_name;
        series.frame_values.resize(row_count);
        for (size_t row = 0; row < row_count; ++row) {
          const size_t source_row =
              static_cast<size_t>(request.first_frame) + row;
          series.frame_values[row] =
              static_cast<double>(retained->second[source_row]);
        }
        fields.push_back(std::move(series));
      }
    } else {
      const auto read = ts::Read(sliceRows(frame_angles_, request.first_frame,
                                           request.last_frame + 1))
                            .result();
      if (!read.ok() || read->rank() != 2 ||
          read->shape()[0] != static_cast<ts::Index>(row_count) ||
          read->byte_strides().size() != 2) {
        failure.status = crimson::timeline::EyeAngleTimelineStatus::ReadFailed;
        failure.error = "Failed to read the eye-angle frame window";
        return failure;
      }
      const auto strides = read->byte_strides();
      const auto* origin = reinterpret_cast<const uint8_t*>(
          read->byte_strided_origin_pointer().get());
      for (const auto& field : representation->fields) {
        const auto channel = channels_.find(field.source_name);
        if (channel == channels_.end() ||
            channel->second >= static_cast<size_t>(read->shape()[1])) {
          continue;
        }
        crimson::timeline::EyeAngleTimelineFieldSeries series;
        series.source_name = field.source_name;
        series.frame_values.resize(row_count);
        for (size_t row = 0; row < row_count; ++row) {
          const auto* value =
              origin + static_cast<ts::Index>(row) * strides[0] +
              static_cast<ts::Index>(channel->second) * strides[1];
          series.frame_values[row] =
              static_cast<double>(*reinterpret_cast<const float*>(value));
        }
        fields.push_back(std::move(series));
      }
    }

    std::vector<double> times;
    if (preloaded_times_) {
      const auto first = preloaded_times_->begin() +
                         static_cast<std::ptrdiff_t>(request.first_frame);
      const auto last = first + static_cast<std::ptrdiff_t>(row_count);
      times.assign(first, last);
    } else if (frame_times_) {
      const auto time_read =
          ts::Read(sliceRows(*frame_times_, request.first_frame,
                             request.last_frame + 1))
              .result();
      if (!time_read.ok() || time_read->rank() != 1 ||
          time_read->shape()[0] != static_cast<ts::Index>(row_count) ||
          time_read->byte_strides().size() != 1) {
        failure.status = crimson::timeline::EyeAngleTimelineStatus::ReadFailed;
        failure.error = "Failed to read stored eye-angle frame times";
        return failure;
      }
      const auto* time_origin = reinterpret_cast<const uint8_t*>(
          time_read->byte_strided_origin_pointer().get());
      times.resize(row_count);
      for (size_t row = 0; row < row_count; ++row) {
        times[row] = static_cast<double>(*reinterpret_cast<const float*>(
            time_origin +
            static_cast<ts::Index>(row) * time_read->byte_strides()[0]));
      }
    }
    return crimson::timeline::buildEyeAngleTimelineWindow(
        descriptor_, request, request.first_frame, times, fields);
  }

 private:
  bool readSelectedChannels(
      const std::vector<size_t>& channels,
      std::unordered_map<size_t, std::vector<float>>* output) const {
    const auto read = ts::Read(frame_angles_).result();
    if (!read.ok() || read->rank() != 2 ||
        read->shape()[0] != static_cast<ts::Index>(descriptor_.frame_count) ||
        read->shape()[1] != static_cast<ts::Index>(frame_column_count_) ||
        read->byte_strides().size() != 2) {
      return false;
    }
    output->clear();
    for (const size_t channel : channels) {
      if (channel >= frame_column_count_) {
        return false;
      }
      output->emplace(channel, std::vector<float>(descriptor_.frame_count));
    }
    const auto* origin = reinterpret_cast<const uint8_t*>(
        read->byte_strided_origin_pointer().get());
    for (size_t row = 0; row < descriptor_.frame_count; ++row) {
      for (const size_t channel : channels) {
        (*output)[channel][row] = *reinterpret_cast<const float*>(
            origin + static_cast<ts::Index>(row) * read->byte_strides()[0] +
            static_cast<ts::Index>(channel) * read->byte_strides()[1]);
      }
    }
    return true;
  }

  bool readAllTimes(std::vector<float>* output) const {
    if (!frame_times_) {
      output->clear();
      return true;
    }
    const auto read = ts::Read(*frame_times_).result();
    if (!read.ok() || read->rank() != 1 ||
        read->shape()[0] != static_cast<ts::Index>(descriptor_.frame_count) ||
        read->byte_strides().size() != 1) {
      return false;
    }
    output->resize(descriptor_.frame_count);
    const auto* origin = reinterpret_cast<const uint8_t*>(
        read->byte_strided_origin_pointer().get());
    for (size_t row = 0; row < descriptor_.frame_count; ++row) {
      (*output)[row] = *reinterpret_cast<const float*>(
          origin + static_cast<ts::Index>(row) * read->byte_strides()[0]);
    }
    return true;
  }

  crimson::timeline::EyeAngleTimelineDescriptor descriptor_;
  ts::TensorStore<float, 2> frame_angles_;
  std::optional<ts::TensorStore<float, 1>> frame_times_;
  std::unordered_map<std::string, size_t> channels_;
  size_t frame_column_count_ = 0;
  std::unordered_map<size_t, std::vector<float>> preloaded_channels_;
  std::optional<std::vector<float>> preloaded_times_;
  std::string preloaded_representation_;
  mutable std::mutex metrics_mutex_;
  mutable crimson::timeline::EyeAngleTimelineRepositoryMetrics metrics_;
};

}  // namespace

std::unique_ptr<crimson::timeline::EyeAngleTimelineRepository>
OpenEyeAngleTimelineRepository(
    const std::shared_ptr<ArchiveContext>& archive,
    const std::string& requested_run, std::string* error_message,
    crimson::data::SmallSeriesPreloadPolicy preload_policy) {
  auto fail = [&](std::string message)
      -> std::unique_ptr<crimson::timeline::EyeAngleTimelineRepository> {
    if (error_message != nullptr) {
      *error_message = std::move(message);
    }
    return nullptr;
  };
  if (!archive || !archive->impl_) {
    return fail("Archive context is unavailable");
  }
  const auto& impl = *archive->impl_;
  const std::string group = "analysis/eye_angle_runs";
  const std::string run =
      requested_run.empty() ? latestRun(impl, group) : requested_run;
  if (!validRunName(run)) {
    return fail("No valid eye-angle timeline run was selected");
  }
  const std::string base = group + "/" + run;
  const auto attributes = internal::ReadArchiveAttributes(impl, base);
  if (!attributes) {
    return fail("Eye-angle timeline metadata is unavailable: " + run);
  }
  if (stringValue(*attributes, "schema_id") != "analysis.eye_angle_runs" ||
      integerValue(*attributes, "schema_version") != 5 ||
      stringValue(*attributes, "layout") != "compact_dense_v2") {
    return fail("Eye-angle timeline requires compact schema 5");
  }
  auto frame_angles = openArray<float, 2>(impl, base + "/frame_angles");
  auto roi_angles = openArray<float, 2>(impl, base + "/roi_angles");
  if (!frame_angles || frame_angles->domain().shape()[0] <= 0 ||
      frame_angles->domain().shape()[1] <= 0) {
    return fail("Eye-angle frame matrix is unavailable or empty");
  }
  const size_t frame_count =
      static_cast<size_t>(frame_angles->domain().shape()[0]);
  const size_t column_count =
      static_cast<size_t>(frame_angles->domain().shape()[1]);
  auto frame_times =
      openArray<float, 1>(impl, base + "/support/frame_time_seconds");
  if (frame_times &&
      frame_times->domain().shape()[0] != static_cast<ts::Index>(frame_count)) {
    return fail("Eye-angle frame-time count does not match frame angles");
  }
  const auto channels =
      availableFrameChannels(impl, base + "/angle_channel_index");
  if (channels.empty()) {
    return fail("No frame-available eye-angle channels were found");
  }
  for (const auto& channel : channels) {
    if (channel.second >= column_count) {
      return fail("Eye-angle frame channel exceeds the matrix shape");
    }
  }

  json variant_schema;
  const auto variant = attributes->find("eye_angle_variant_schema");
  if (variant != attributes->end() && variant->is_object()) {
    variant_schema = *variant;
  } else {
    const auto output = attributes->find("eye_angle_output_schema");
    if (output != attributes->end() && output->is_object()) {
      const auto nested = output->find("variant_schema");
      if (nested != output->end() && nested->is_object()) {
        variant_schema = *nested;
      }
    }
  }
  if (!variant_schema.is_object()) {
    return fail("Eye-angle representation metadata is unavailable");
  }
  const auto representations = variant_schema.find("representations");
  if (representations == variant_schema.end() ||
      !representations->is_object()) {
    return fail("Eye-angle representations are unavailable");
  }
  auto order = stringVector(variant_schema, "representation_order");
  if (order.empty()) {
    for (auto it = representations->begin(); it != representations->end();
         ++it) {
      order.push_back(it.key());
    }
  }
  const auto field_metadata =
      variant_schema.contains("fields") && variant_schema["fields"].is_object()
          ? variant_schema["fields"]
          : json::object();

  crimson::timeline::EyeAngleTimelineDescriptor descriptor;
  descriptor.source_group = group;
  descriptor.run_name = run;
  descriptor.schema_id = stringValue(*attributes, "schema_id");
  descriptor.schema_version = integerValue(*attributes, "schema_version");
  descriptor.method = stringValue(*attributes, "method");
  descriptor.method_version = methodVersion(*attributes);
  descriptor.layout = stringValue(*attributes, "layout");
  descriptor.default_representation =
      stringValue(variant_schema, "default_representation");
  descriptor.roi_row_count =
      roi_angles && roi_angles->domain().shape()[0] > 0
          ? static_cast<size_t>(roi_angles->domain().shape()[0])
          : 0;
  descriptor.frame_count = frame_count;
  for (const auto& key : order) {
    const auto found = representations->find(key);
    if (found == representations->end() || !found->is_object()) {
      continue;
    }
    crimson::timeline::EyeAngleTimelineRepresentation representation;
    representation.key = key;
    representation.display_name = stringValue(*found, "display_name");
    representation.role = stringValue(*found, "role");
    representation.coordinate_frame = stringValue(*found, "coordinate_frame");
    representation.units = stringValue(*found, "units");
    std::unordered_set<std::string> used_sources;
    for (const auto& requested : representationFields(*found)) {
      auto field = resolveField(requested, channels, field_metadata,
                                representation.units);
      if (field && used_sources.insert(field->source_name).second) {
        representation.fields.push_back(std::move(*field));
      }
    }
    descriptor.representations.push_back(std::move(representation));
  }
  const bool has_fields = std::any_of(descriptor.representations.begin(),
                                      descriptor.representations.end(),
                                      [](const auto& representation) {
                                        return !representation.fields.empty();
                                      });
  if (!has_fields) {
    return fail("No representation has frame-available eye-angle fields");
  }
  descriptor.default_representation =
      crimson::timeline::defaultEyeAngleTimelineRepresentation(descriptor);
  if (descriptor.default_representation.empty()) {
    return fail("No default eye-angle timeline representation is available");
  }
  if (error_message != nullptr) {
    error_message->clear();
  }
  return std::make_unique<TensorStoreTimelineRepository>(
      std::move(descriptor), std::move(*frame_angles), std::move(frame_times),
      channels, preload_policy);
}

}  // namespace crimson::zarr
