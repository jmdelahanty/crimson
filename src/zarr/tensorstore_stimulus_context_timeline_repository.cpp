#include "zarr/tensorstore_stimulus_context_timeline_repository.h"

#include <tensorstore/open.h>
#include <tensorstore/tensorstore.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "zarr/archive_context_internal.h"
#include "zarr/tensorstore_stimulus_repository.h"

namespace crimson::zarr {
namespace ts = tensorstore;
using json = nlohmann::json;

namespace {

#pragma pack(push, 1)
struct StimulusEventRowV3 {
  int64_t timestamp_ns_epoch = 0;
  int64_t timestamp_ns_session = 0;
  int32_t event_type_id = 0;
  int32_t current_step_index = 0;
  uint64_t stimulus_frame_num = 0;
  uint64_t camera_frame_id = 0;
  char name_or_context[256];
  int32_t stimulus_mode_id = 0;
  char details_json[1024];
};
#pragma pack(pop)

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

template <typename T>
bool readTypedIntegers(const ArchiveContext::Impl& archive,
                       const std::string& path, std::vector<int64_t>* output) {
  const auto store = openArray<T, 1>(archive, path);
  if (!store) {
    return false;
  }
  const auto result = ts::Read(*store).result();
  if (!result.ok() || result->rank() != 1) {
    return false;
  }
  const size_t count = static_cast<size_t>(result->shape()[0]);
  const auto* values = static_cast<const T*>(result->data());
  output->resize(count);
  for (size_t index = 0; index < count; ++index) {
    if constexpr (std::is_unsigned_v<T>) {
      (*output)[index] =
          values[index] >
                  static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
              ? -1
              : static_cast<int64_t>(values[index]);
    } else {
      (*output)[index] = static_cast<int64_t>(values[index]);
    }
  }
  return true;
}

bool readIntegers(const ArchiveContext::Impl& archive, const std::string& path,
                  std::vector<int64_t>* output) {
  return readTypedIntegers<int64_t>(archive, path, output) ||
         readTypedIntegers<uint64_t>(archive, path, output) ||
         readTypedIntegers<int32_t>(archive, path, output) ||
         readTypedIntegers<uint32_t>(archive, path, output) ||
         readTypedIntegers<int16_t>(archive, path, output) ||
         readTypedIntegers<uint16_t>(archive, path, output) ||
         readTypedIntegers<int8_t>(archive, path, output) ||
         readTypedIntegers<uint8_t>(archive, path, output);
}

std::string fixedString(const char* value, size_t capacity) {
  const auto* end =
      static_cast<const char*>(std::memchr(value, '\0', capacity));
  return std::string(value, end ? static_cast<size_t>(end - value) : capacity);
}

bool readStrings(const ArchiveContext::Impl& archive, const std::string& path,
                 std::vector<std::string>* output) {
  if (const auto store = openArray<std::string, 1>(archive, path)) {
    const auto result = ts::Read(*store).result();
    if (result.ok() && result->rank() == 1) {
      const size_t count = static_cast<size_t>(result->shape()[0]);
      const auto* values = static_cast<const std::string*>(result->data());
      output->assign(values, values + count);
      return true;
    }
  }
  const auto store = openArray<uint8_t, 2>(archive, path);
  if (!store) {
    return false;
  }
  const auto result = ts::Read(*store).result();
  if (!result.ok() || result->rank() != 2) {
    return false;
  }
  const size_t rows = static_cast<size_t>(result->shape()[0]);
  const size_t width = static_cast<size_t>(result->shape()[1]);
  const auto* values = static_cast<const uint8_t*>(result->data());
  output->clear();
  output->reserve(rows);
  for (size_t row = 0; row < rows; ++row) {
    const auto* begin = reinterpret_cast<const char*>(values + row * width);
    output->push_back(fixedString(begin, width));
  }
  return true;
}

std::string stringValue(const json& attributes, const char* key) {
  const auto found = attributes.find(key);
  return found != attributes.end() && found->is_string()
             ? found->get<std::string>()
             : std::string{};
}

int64_t integerValue(const json& attributes, const char* key,
                     int64_t fallback = -1) {
  const auto found = attributes.find(key);
  if (found == attributes.end() || !found->is_number_integer()) {
    return fallback;
  }
  if (found->is_number_unsigned()) {
    const uint64_t value = found->get<uint64_t>();
    return value <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
               ? static_cast<int64_t>(value)
               : fallback;
  }
  return found->get<int64_t>();
}

int32_t int32Value(const json& attributes, const char* key,
                   int32_t fallback = -1) {
  const int64_t value = integerValue(attributes, key, fallback);
  return value >= std::numeric_limits<int32_t>::min() &&
                 value <= std::numeric_limits<int32_t>::max()
             ? static_cast<int32_t>(value)
             : fallback;
}

double doubleValue(const json& attributes, const char* key) {
  const auto found = attributes.find(key);
  return found != attributes.end() && found->is_number()
             ? found->get<double>()
             : std::numeric_limits<double>::quiet_NaN();
}

bool boolValue(const json& attributes, const char* key, bool* value) {
  const auto found = attributes.find(key);
  if (found == attributes.end() || !found->is_boolean()) {
    return false;
  }
  *value = found->get<bool>();
  return true;
}

bool validRunName(const std::string& value) {
  return !value.empty() && value != "." && value != ".." &&
         value.find('/') == std::string::npos;
}

std::string latestRun(const ArchiveContext& archive,
                      const ArchiveContext::Impl& impl) {
  const auto attributes =
      internal::ReadArchiveAttributes(impl, "analysis/stimulus_runs");
  constexpr std::array<const char*, 4> keys = {
      "latest_complete", "latest_completed", "latest_success", "latest"};
  if (attributes) {
    for (const char* key : keys) {
      const std::string run = stringValue(*attributes, key);
      if (validRunName(run)) {
        return run;
      }
    }
  }

  const auto runs_path = archive.rootPath() / "analysis" / "stimulus_runs";
  std::vector<std::string> candidates;
  std::error_code error;
  for (std::filesystem::directory_iterator it(runs_path, error), end;
       !error && it != end; it.increment(error)) {
    if (!it->is_directory()) {
      continue;
    }
    const std::string name = it->path().filename().string();
    if (validRunName(name)) {
      candidates.push_back(name);
    }
  }
  return candidates.empty()
             ? std::string{}
             : *std::max_element(candidates.begin(), candidates.end());
}

int32_t toInt32(int64_t value, int32_t fallback = -1) {
  return value >= std::numeric_limits<int32_t>::min() &&
                 value <= std::numeric_limits<int32_t>::max()
             ? static_cast<int32_t>(value)
             : fallback;
}

bool detailsAreStructured(const std::string& value) {
  if (value.empty()) {
    return false;
  }
  const json parsed = json::parse(value, nullptr, false);
  return !parsed.is_discarded() && parsed.is_structured();
}

std::map<int32_t, std::string> readEventTypes(
    const ArchiveContext::Impl& archive) {
  constexpr std::array<std::pair<const char*, const char*>, 5> candidates = {{
      {"analysis/enums/events/event_type_id", "analysis/enums/events/name"},
      {"analysis/enums/events/event_type_id", "analysis/enums/events/value"},
      {"analysis/enums/events/id", "analysis/enums/events/name"},
      {"analysis/enums/events/id", "analysis/enums/events/value"},
      {"analysis/enums/events/ids", "analysis/enums/events/names"},
  }};
  for (const auto& [id_path, name_path] : candidates) {
    std::vector<int64_t> ids;
    std::vector<std::string> names;
    if (!readIntegers(archive, id_path, &ids) ||
        !readStrings(archive, name_path, &names)) {
      continue;
    }
    std::map<int32_t, std::string> result;
    const size_t count = std::min(ids.size(), names.size());
    for (size_t index = 0; index < count; ++index) {
      const int32_t id = toInt32(ids[index]);
      if (id >= 0) {
        result[id] = names[index];
      }
    }
    if (!result.empty()) {
      return result;
    }
  }
  return {};
}

void normalizeEvent(crimson::timeline::StimulusContextEvent* event,
                    const std::map<int32_t, std::string>& event_types) {
  const auto type = event_types.find(event->event_type_id);
  if (type != event_types.end()) {
    event->event_type_name = type->second;
  }
  event->label = crimson::timeline::buildStimulusEventLabel(
      event->event_type_name, event->event_type_id, event->name_or_context,
      event->details_json, detailsAreStructured(event->details_json));
}

bool readColumnEvents(
    const ArchiveContext::Impl& archive, const std::string& run_base,
    const std::map<int32_t, std::string>& event_types,
    std::vector<crimson::timeline::StimulusContextEvent>* events) {
  const std::string base = run_base + "/events/";
  std::vector<int64_t> stimulus_frames;
  if (!readIntegers(archive, base + "stimulus_frame_num", &stimulus_frames)) {
    return false;
  }
  std::vector<int64_t> camera_frames;
  std::vector<int64_t> type_ids;
  std::vector<int64_t> timestamps;
  std::vector<std::string> names;
  std::vector<std::string> details;
  readIntegers(archive, base + "camera_frame_id", &camera_frames);
  readIntegers(archive, base + "event_type_id", &type_ids);
  readIntegers(archive, base + "timestamp_ns_session", &timestamps);
  readStrings(archive, base + "name_or_context", &names);
  readStrings(archive, base + "details_json", &details);

  events->clear();
  events->reserve(stimulus_frames.size());
  for (size_t index = 0; index < stimulus_frames.size(); ++index) {
    crimson::timeline::StimulusContextEvent event;
    event.source_event_index = index;
    event.stimulus_frame = stimulus_frames[index];
    event.camera_frame =
        index < camera_frames.size() ? camera_frames[index] : -1;
    event.timestamp_ns_session =
        index < timestamps.size() ? timestamps[index] : 0;
    event.event_type_id =
        index < type_ids.size() ? toInt32(type_ids[index]) : -1;
    event.name_or_context = index < names.size() ? names[index] : std::string{};
    event.details_json =
        index < details.size() ? details[index] : std::string{};
    normalizeEvent(&event, event_types);
    events->push_back(std::move(event));
  }
  return true;
}

bool readStructuredEvents(
    const ArchiveContext::Impl& archive, const std::string& run_base,
    const std::map<int32_t, std::string>& event_types,
    std::vector<crimson::timeline::StimulusContextEvent>* events) {
  const auto store =
      openArray<StimulusEventRowV3, 1>(archive, run_base + "/events");
  if (!store) {
    return false;
  }
  const auto result = ts::Read(*store).result();
  if (!result.ok() || result->rank() != 1) {
    return false;
  }
  const size_t count = static_cast<size_t>(result->shape()[0]);
  const auto* rows = static_cast<const StimulusEventRowV3*>(result->data());
  events->clear();
  events->reserve(count);
  for (size_t index = 0; index < count; ++index) {
    crimson::timeline::StimulusContextEvent event;
    event.source_event_index = index;
    event.stimulus_frame =
        rows[index].stimulus_frame_num <=
                static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
            ? static_cast<int64_t>(rows[index].stimulus_frame_num)
            : -1;
    event.camera_frame =
        rows[index].camera_frame_id <=
                static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
            ? static_cast<int64_t>(rows[index].camera_frame_id)
            : -1;
    event.timestamp_ns_session = rows[index].timestamp_ns_session;
    event.event_type_id = rows[index].event_type_id;
    event.name_or_context = fixedString(rows[index].name_or_context,
                                        sizeof(rows[index].name_or_context));
    event.details_json =
        fixedString(rows[index].details_json, sizeof(rows[index].details_json));
    normalizeEvent(&event, event_types);
    events->push_back(std::move(event));
  }
  return true;
}

std::vector<int32_t> stepIndices(const ArchiveContext& archive,
                                 const std::string& run_name) {
  const auto path =
      archive.rootPath() / "analysis/stimulus_runs" / run_name / "steps";
  std::vector<int32_t> result;
  std::error_code error;
  for (std::filesystem::directory_iterator it(path, error), end;
       !error && it != end; it.increment(error)) {
    if (!it->is_directory()) {
      continue;
    }
    const std::string name = it->path().filename().string();
    if (name.rfind("step_", 0) != 0 || name.size() <= 5) {
      continue;
    }
    try {
      size_t parsed = 0;
      const long value = std::stol(name.substr(5), &parsed);
      if (parsed == name.size() - 5 && value >= 0 &&
          value <= std::numeric_limits<int32_t>::max()) {
        result.push_back(static_cast<int32_t>(value));
      }
    } catch (const std::exception&) {
    }
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

std::vector<crimson::timeline::StimulusContextStep> readSteps(
    const ArchiveContext& archive, const ArchiveContext::Impl& impl,
    const std::string& run_name) {
  std::vector<crimson::timeline::StimulusContextStep> result;
  for (const int32_t discovered : stepIndices(archive, run_name)) {
    const std::string base = "analysis/stimulus_runs/" + run_name +
                             "/steps/step_" + std::to_string(discovered);
    const auto attributes = internal::ReadArchiveAttributes(impl, base);
    if (!attributes) {
      continue;
    }
    crimson::timeline::StimulusContextStep step;
    step.step_index = int32Value(*attributes, "step_index", discovered);
    step.step_name = stringValue(*attributes, "step_name");
    step.stimulus_mode_id = int32Value(*attributes, "stimulus_mode_id");
    step.stimulus_mode = stringValue(*attributes, "stimulus_mode");
    step.start_camera_frame = integerValue(*attributes, "start_camera_frame");
    step.end_camera_frame = integerValue(*attributes, "end_camera_frame");
    step.duration_s = doubleValue(*attributes, "duration_s");
    step.raw_protocol_params_json =
        stringValue(*attributes, "raw_protocol_params_json");

    if (const auto moving =
            internal::ReadArchiveAttributes(impl, base + "/moving_grating")) {
      auto& value = step.moving_grating;
      value.present = true;
      value.grating_direction_camera_deg =
          doubleValue(*moving, "grating_direction_camera_deg");
      value.orientation_degrees_authored =
          doubleValue(*moving, "orientation_degrees_authored");
      value.camera_to_projector_offset_deg =
          doubleValue(*moving, "camera_to_projector_offset_deg");
      value.direction_mapping_status =
          stringValue(*moving, "direction_mapping_status");
      value.has_direction_mapping_validated =
          boolValue(*moving, "direction_mapping_validated",
                    &value.direction_mapping_validated);
      value.speed_mm_s = doubleValue(*moving, "speed_mm_s");
      value.temporal_frequency_hz =
          doubleValue(*moving, "temporal_frequency_hz");
    }
    if (const auto concentric = internal::ReadArchiveAttributes(
            impl, base + "/concentric_grating")) {
      auto& value = step.concentric_grating;
      value.present = true;
      value.stimulus_role = stringValue(*concentric, "stimulus_role");
      value.radial_polarity_authored =
          stringValue(*concentric, "radial_polarity_authored");
      value.radial_sign_authored =
          doubleValue(*concentric, "radial_sign_authored");
      value.has_radial_polarity_validated =
          boolValue(*concentric, "radial_polarity_validated",
                    &value.radial_polarity_validated);
      value.center_x_px = doubleValue(*concentric, "center_x_px");
      value.center_y_px = doubleValue(*concentric, "center_y_px");
      value.center_x_mm = doubleValue(*concentric, "center_x_mm");
      value.center_y_mm = doubleValue(*concentric, "center_y_mm");
      value.target_radius_min_mm =
          doubleValue(*concentric, "target_radius_min_mm");
      value.target_radius_max_mm =
          doubleValue(*concentric, "target_radius_max_mm");
      value.speed_mm_s = doubleValue(*concentric, "speed_mm_s");
      value.temporal_frequency_hz =
          doubleValue(*concentric, "temporal_frequency_hz");
    }
    result.push_back(std::move(step));
  }
  return result;
}

}  // namespace

std::unique_ptr<crimson::timeline::StimulusContextTimelineRepository>
OpenStimulusContextTimelineRepository(
    const std::shared_ptr<ArchiveContext>& archive,
    std::size_t frame_count_hint, const std::string& requested_run,
    std::string* error_message) {
  if (!archive || !archive->impl_) {
    internal::SetArchiveError(error_message, "Archive context is not open");
    return nullptr;
  }
  const auto& impl = *archive->impl_;
  const std::string run_name =
      requested_run.empty() ? latestRun(*archive, impl) : requested_run;
  if (!validRunName(run_name)) {
    internal::SetArchiveError(
        error_message,
        "No valid stimulus run was requested or selected from the archive");
    return nullptr;
  }
  const std::string run_base = "analysis/stimulus_runs/" + run_name;
  const auto event_names = readEventTypes(impl);
  std::vector<crimson::timeline::StimulusContextEvent> events;
  if (!readColumnEvents(impl, run_base, event_names, &events)) {
    readStructuredEvents(impl, run_base, event_names, &events);
  }
  auto steps = readSteps(*archive, impl, run_name);
  if (events.empty() && steps.empty()) {
    internal::SetArchiveError(
        error_message, "Stimulus run '" + run_name +
                           "' has no readable events or canonical steps");
    return nullptr;
  }

  std::unique_ptr<StimulusRepository> alignment;
  if (std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.camera_frame < 0 && event.stimulus_frame >= 0;
      })) {
    alignment = OpenStimulusRepository(archive, run_name, nullptr);
  }
  if (alignment) {
    for (auto& event : events) {
      if (event.camera_frame >= 0 || event.stimulus_frame < 0 ||
          event.stimulus_frame > std::numeric_limits<int32_t>::max()) {
        continue;
      }
      const auto camera = alignment->cameraFrameForStimulus(
          static_cast<int32_t>(event.stimulus_frame));
      if (camera) {
        event.camera_frame = *camera;
      }
    }
  }

  crimson::timeline::StimulusContextTimelineDescriptor descriptor;
  descriptor.run_name = run_name;
  descriptor.frame_count = frame_count_hint;
  descriptor.event_types.reserve(event_names.size());
  for (const auto& [id, name] : event_names) {
    descriptor.event_types.push_back({id, name, 0});
  }
  return crimson::timeline::MakeStimulusContextTimelineRepository(
      std::move(descriptor), std::move(events), std::move(steps));
}

}  // namespace crimson::zarr
