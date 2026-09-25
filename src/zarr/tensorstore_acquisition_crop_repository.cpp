#include "zarr/tensorstore_acquisition_crop_repository.h"

#include "zarr/archive_context_internal.h"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace crimson::zarr {
using json = nlohmann::json;

namespace {

constexpr std::string_view kInventoryPath =
    "analysis/acquisition_video_streams";
constexpr std::string_view kCropStreamPath =
    "analysis/acquisition_video_streams/streams/crop";

class CsvRecordReader {
 public:
  explicit CsvRecordReader(const std::filesystem::path& path)
      : input_(path, std::ios::binary) {}

  bool isOpen() const { return input_.is_open(); }

  bool next(std::vector<std::string>* fields,
            bool* eof,
            std::string* error) {
    fields->clear();
    *eof = false;
    std::string field;
    bool quoted = false;
    bool quote_closed = false;
    bool saw_data = false;

    while (true) {
      const int next = input_.get();
      if (next == std::char_traits<char>::eof()) {
        if (quoted) {
          *error = "CSV ended inside a quoted field";
          return false;
        }
        if (!saw_data && field.empty() && fields->empty()) {
          *eof = true;
          return true;
        }
        fields->push_back(std::move(field));
        return true;
      }

      saw_data = true;
      const char value = static_cast<char>(next);
      if (quoted) {
        if (value != '"') {
          field.push_back(value);
          continue;
        }
        if (input_.peek() == '"') {
          input_.get();
          field.push_back('"');
          continue;
        }
        quoted = false;
        quote_closed = true;
        continue;
      }

      if (quote_closed && value != ',' && value != '\n' && value != '\r') {
        *error = "CSV has characters after a closing quote";
        return false;
      }
      if (value == '"') {
        if (!field.empty() || quote_closed) {
          *error = "CSV quote appears inside an unquoted field";
          return false;
        }
        quoted = true;
        continue;
      }
      if (value == ',') {
        fields->push_back(std::move(field));
        field.clear();
        quote_closed = false;
        continue;
      }
      if (value == '\n') {
        fields->push_back(std::move(field));
        return true;
      }
      if (value == '\r') {
        if (input_.peek() == '\n') {
          input_.get();
        }
        fields->push_back(std::move(field));
        return true;
      }
      field.push_back(value);
    }
  }

 private:
  std::ifstream input_;
};

bool Fail(std::string* error_message, std::string message) {
  internal::SetArchiveError(error_message, std::move(message));
  return false;
}

bool ReadJsonFile(const std::filesystem::path& path,
                  json* value,
                  std::string* error_message) {
  std::ifstream input(path);
  if (!input) {
    return Fail(error_message, "Could not open JSON file: " + path.string());
  }
  try {
    input >> *value;
  } catch (const json::exception& error) {
    return Fail(error_message,
                "Invalid JSON file " + path.string() + ": " + error.what());
  }
  return true;
}

bool RequiredObject(const json& parent,
                    const char* key,
                    const std::string& context,
                    const json** output,
                    std::string* error_message) {
  auto found = parent.find(key);
  if (found == parent.end() || !found->is_object()) {
    return Fail(error_message, context + " requires object '" + key + "'");
  }
  *output = &*found;
  return true;
}

bool RequiredString(const json& parent,
                    const char* key,
                    const std::string& context,
                    std::string* output,
                    std::string* error_message) {
  auto found = parent.find(key);
  if (found == parent.end() || !found->is_string() ||
      found->get_ref<const std::string&>().empty()) {
    return Fail(error_message,
                context + " requires non-empty string '" + key + "'");
  }
  *output = found->get<std::string>();
  return true;
}

bool RequiredInt64(const json& parent,
                   const char* key,
                   const std::string& context,
                   int64_t* output,
                   std::string* error_message) {
  auto found = parent.find(key);
  if (found == parent.end() || !found->is_number_integer()) {
    return Fail(error_message, context + " requires integer '" + key + "'");
  }
  *output = found->get<int64_t>();
  return true;
}

bool RequiredDouble(const json& parent,
                    const char* key,
                    const std::string& context,
                    double* output,
                    std::string* error_message) {
  auto found = parent.find(key);
  if (found == parent.end() || !found->is_number()) {
    return Fail(error_message, context + " requires number '" + key + "'");
  }
  *output = found->get<double>();
  if (!std::isfinite(*output)) {
    return Fail(error_message, context + " has non-finite '" + key + "'");
  }
  return true;
}

bool ParseInt64(const std::string& text, int64_t* output) {
  if (text.empty()) {
    return false;
  }
  const char* begin = text.data();
  const char* end = begin + text.size();
  auto result = std::from_chars(begin, end, *output);
  return result.ec == std::errc() && result.ptr == end;
}

bool ParseDouble(const std::string& text, double* output) {
  if (text.empty()) {
    return false;
  }
  errno = 0;
  char* end = nullptr;
  *output = std::strtod(text.c_str(), &end);
  return errno != ERANGE && end == text.c_str() + text.size() &&
         std::isfinite(*output);
}

bool ContainsParentTraversal(const std::filesystem::path& path) {
  for (const auto& component : path) {
    if (component == "..") {
      return true;
    }
  }
  return false;
}

bool ResolveRequiredFile(const ArchiveContext& archive,
                         const std::filesystem::path& stored,
                         const std::string& label,
                         std::filesystem::path* resolved,
                         std::string* error_message) {
  if (stored.empty()) {
    return Fail(error_message, "Crop contract has no " + label + " path");
  }
  if (stored.is_relative() && ContainsParentTraversal(stored)) {
    return Fail(error_message,
                "Crop contract " + label + " path escapes recording root: " +
                    stored.string());
  }
  *resolved = archive.resolveStoredPath(stored);
  if (!std::filesystem::is_regular_file(*resolved)) {
    return Fail(error_message,
                "Resolved crop " + label + " file does not exist: " +
                    resolved->string());
  }
  return true;
}

bool ValidateDuplicatedFilePath(const json& files,
                                const char* file_key,
                                const std::filesystem::path& contract_path,
                                std::string* error_message) {
  const json* file = nullptr;
  if (!RequiredObject(files, file_key, "crop files", &file,
                      error_message)) {
    return false;
  }
  std::string file_path;
  if (!RequiredString(*file, "path", "crop files." + std::string(file_key),
                      &file_path, error_message)) {
    return false;
  }
  if (std::filesystem::path(file_path) != contract_path) {
    return Fail(error_message,
                "Crop files." + std::string(file_key) +
                    ".path disagrees with contract path");
  }
  return true;
}

bool ParseDescriptor(const ArchiveContext& archive,
                     const json& root,
                     const json& crop,
                     AcquisitionCropStreamDescriptor* descriptor,
                     int64_t* imported_row_count,
                     std::string* error_message) {
  if (!RequiredString(root, "schema_id", "acquisition inventory",
                      &descriptor->schema_id, error_message) ||
      descriptor->schema_id != "palette.acquisition_video_streams.v1") {
    return Fail(error_message, "Unsupported acquisition inventory schema_id");
  }
  int64_t schema_version = 0;
  if (!RequiredInt64(root, "schema_version", "acquisition inventory",
                     &schema_version, error_message) ||
      schema_version != 1) {
    return Fail(error_message, "Unsupported acquisition inventory version");
  }
  descriptor->schema_version = static_cast<int>(schema_version);
  if (!RequiredString(root, "source_schema_id", "acquisition inventory",
                      &descriptor->source_schema_id, error_message) ||
      descriptor->source_schema_id != "orange_runtime_video_streams_v1") {
    return Fail(error_message, "Unsupported acquisition source schema");
  }
  std::string source_frame_clock;
  std::string inventory_status;
  if (!RequiredString(root, "source_frame_clock", "acquisition inventory",
                      &source_frame_clock, error_message) ||
      source_frame_clock != "recording_frame_id" ||
      !RequiredString(root, "inventory_status", "acquisition inventory",
                      &inventory_status, error_message) ||
      inventory_status != "ok") {
    return Fail(error_message, "Unsupported acquisition inventory state");
  }
  auto available = root.find("crop_stream_available");
  if (available == root.end() || !available->is_boolean() ||
      !available->get<bool>()) {
    return Fail(error_message, "Acquisition crop stream is not available");
  }
  auto availability = crop.find("availability_status");
  if (availability == crop.end() || !availability->is_string() ||
      availability->get<std::string>() != "ok") {
    return Fail(error_message, "Acquisition crop availability is not ok");
  }
  auto required_missing = crop.find("required_missing");
  if (required_missing == crop.end() || !required_missing->is_array() ||
      !required_missing->empty()) {
    return Fail(error_message,
                "Acquisition crop has unresolved required files");
  }

  const json* contract = nullptr;
  const json* files = nullptr;
  if (!RequiredObject(crop, "contract", "crop stream", &contract,
                      error_message) ||
      !RequiredObject(crop, "files", "crop stream", &files,
                      error_message)) {
    return false;
  }
  std::string stream_key;
  std::string role;
  std::string output_kind;
  std::string source;
  std::string declared_role;
  if (!RequiredString(crop, "stream_key", "crop stream", &stream_key,
                      error_message) ||
      stream_key != "crop" ||
      !RequiredString(*contract, "role", "crop contract", &role,
                      error_message) ||
      role != "runtime_derived_acquisition_input" ||
      !RequiredString(*contract, "output_kind", "crop contract",
                      &output_kind, error_message) ||
      output_kind != "crop" ||
      !RequiredString(*contract, "source", "crop contract", &source,
                      error_message) ||
      source != "orange_external_ipc" ||
      !RequiredString(*contract, "orange_declared_role", "crop contract",
                      &declared_role, error_message) ||
      declared_role != "sidecar") {
    return Fail(error_message, "Unsupported acquisition crop stream role");
  }
  descriptor->stream_key = "crop";
  if (!RequiredString(*contract, "stream_id", "crop contract",
                      &descriptor->stream_id, error_message) ||
      !RequiredString(*contract, "camera_id", "crop contract",
                      &descriptor->camera_id, error_message) ||
      !RequiredString(*contract, "frame_clock", "crop contract",
                      &descriptor->frame_clock, error_message) ||
      !RequiredString(*contract, "video_pixel_coordinate_space",
                      "crop contract", &descriptor->video_pixel_coordinate_space,
                      error_message) ||
      !RequiredString(*contract, "source_geometry_coordinate_space",
                      "crop contract",
                      &descriptor->source_geometry_coordinate_space,
                      error_message) ||
      !RequiredString(*contract, "blank_frame_policy", "crop contract",
                      &descriptor->blank_frame_policy, error_message) ||
      !RequiredString(*contract, "selection_policy", "crop contract",
                      &descriptor->selection_policy, error_message) ||
      !RequiredString(*contract, "codec", "crop contract",
                      &descriptor->codec, error_message) ||
      !RequiredString(*contract, "container", "crop contract",
                      &descriptor->container, error_message) ||
      !RequiredString(*contract, "encoded_format", "crop contract",
                      &descriptor->encoded_format, error_message) ||
      !RequiredString(*contract, "pixel_source_format", "crop contract",
                      &descriptor->pixel_source_format, error_message)) {
    return false;
  }
  if (descriptor->frame_clock != "recording_frame_id" ||
      descriptor->video_pixel_coordinate_space != "crop_frame_pixels" ||
      descriptor->source_geometry_coordinate_space != "full_frame_pixels" ||
      descriptor->blank_frame_policy !=
          "encode_black_frame_when_no_detection") {
    return Fail(error_message, "Unsupported acquisition crop frame contract");
  }

  int64_t width = 0;
  int64_t height = 0;
  if (!RequiredInt64(*contract, "width", "crop contract", &width,
                     error_message) ||
      !RequiredInt64(*contract, "height", "crop contract", &height,
                     error_message) ||
      !RequiredInt64(*contract, "frame_count", "crop contract",
                     &descriptor->frame_count, error_message) ||
      !RequiredDouble(*contract, "frame_rate", "crop contract",
                      &descriptor->frame_rate, error_message)) {
    return false;
  }
  if (width <= 0 || width > std::numeric_limits<int>::max() || height <= 0 ||
      height > std::numeric_limits<int>::max() ||
      descriptor->frame_count <= 0 || descriptor->frame_rate <= 0.0) {
    return Fail(error_message, "Acquisition crop dimensions/count/rate are invalid");
  }
  descriptor->output_width = static_cast<int>(width);
  descriptor->output_height = static_cast<int>(height);

  std::string stored;
  if (!RequiredString(*contract, "video", "crop contract", &stored,
                      error_message)) {
    return false;
  }
  descriptor->stored_video_path = stored;
  if (!RequiredString(*contract, "metadata", "crop contract", &stored,
                      error_message)) {
    return false;
  }
  descriptor->stored_metadata_path = stored;
  if (!RequiredString(*contract, "keyframes", "crop contract", &stored,
                      error_message)) {
    return false;
  }
  descriptor->stored_keyframes_path = stored;
  if (!RequiredString(*contract, "summary", "crop contract", &stored,
                      error_message)) {
    return false;
  }
  descriptor->stored_summary_path = stored;

  if (!ValidateDuplicatedFilePath(*files, "video",
                                  descriptor->stored_video_path,
                                  error_message) ||
      !ValidateDuplicatedFilePath(*files, "metadata",
                                  descriptor->stored_metadata_path,
                                  error_message) ||
      !ValidateDuplicatedFilePath(*files, "keyframes",
                                  descriptor->stored_keyframes_path,
                                  error_message) ||
      !ValidateDuplicatedFilePath(*files, "summary",
                                  descriptor->stored_summary_path,
                                  error_message)) {
    return false;
  }

  const json* metadata_file = nullptr;
  if (!RequiredObject(*files, "metadata", "crop files", &metadata_file,
                      error_message) ||
      !RequiredInt64(*metadata_file, "data_row_count",
                     "crop metadata file", imported_row_count,
                     error_message)) {
    return false;
  }
  if (*imported_row_count != descriptor->frame_count) {
    return Fail(error_message,
                "Imported crop row count disagrees with contract frame count");
  }

  auto status = files->find("status");
  if (status != files->end() && status->is_object()) {
    auto path = status->find("path");
    if (path != status->end() && path->is_string()) {
      descriptor->stored_status_path = path->get<std::string>();
      if (descriptor->stored_status_path.is_relative() &&
          ContainsParentTraversal(descriptor->stored_status_path)) {
        return Fail(error_message,
                    "Crop contract status path escapes recording root");
      }
      descriptor->resolved_status_path =
          archive.resolveStoredPath(descriptor->stored_status_path);
    }
  }

  return ResolveRequiredFile(archive, descriptor->stored_video_path, "video",
                             &descriptor->resolved_video_path, error_message) &&
         ResolveRequiredFile(archive, descriptor->stored_metadata_path,
                             "metadata", &descriptor->resolved_metadata_path,
                             error_message) &&
         ResolveRequiredFile(archive, descriptor->stored_keyframes_path,
                             "keyframes", &descriptor->resolved_keyframes_path,
                             error_message) &&
         ResolveRequiredFile(archive, descriptor->stored_summary_path,
                             "summary", &descriptor->resolved_summary_path,
                             error_message);
}

bool ValidateGeometryColumns(const json& crop,
                             std::string* error_message) {
  const json* contract = nullptr;
  if (!RequiredObject(crop, "contract", "crop stream", &contract,
                      error_message)) {
    return false;
  }
  auto columns = contract->find("geometry_columns");
  if (columns == contract->end() || !columns->is_array()) {
    return Fail(error_message, "Crop contract requires geometry_columns");
  }
  const std::vector<std::string> required = {
      "crop_x",       "crop_y",       "crop_w",       "crop_h",
      "detection_x",  "detection_y",  "detection_w",  "detection_h"};
  for (const auto& name : required) {
    bool found = false;
    for (const auto& column : *columns) {
      found = found || (column.is_string() && column.get<std::string>() == name);
    }
    if (!found) {
      return Fail(error_message,
                  "Crop contract geometry_columns is missing '" + name + "'");
    }
  }
  return true;
}

bool ParseCropCsv(const AcquisitionCropStreamDescriptor& descriptor,
                  std::vector<AcquisitionCropFrameRow>* rows,
                  std::string* error_message) {
  CsvRecordReader reader(descriptor.resolved_metadata_path);
  if (!reader.isOpen()) {
    return Fail(error_message,
                "Could not open crop metadata CSV: " +
                    descriptor.resolved_metadata_path.string());
  }

  std::vector<std::string> fields;
  bool eof = false;
  std::string csv_error;
  if (!reader.next(&fields, &eof, &csv_error) || eof) {
    return Fail(error_message,
                csv_error.empty() ? "Crop metadata CSV has no header"
                                  : csv_error);
  }
  if (!fields.empty() && fields.front().size() >= 3 &&
      static_cast<unsigned char>(fields.front()[0]) == 0xEF &&
      static_cast<unsigned char>(fields.front()[1]) == 0xBB &&
      static_cast<unsigned char>(fields.front()[2]) == 0xBF) {
    fields.front().erase(0, 3);
  }

  std::unordered_map<std::string, size_t> column;
  for (size_t index = 0; index < fields.size(); ++index) {
    if (fields[index].empty() || !column.emplace(fields[index], index).second) {
      return Fail(error_message, "Crop metadata CSV has duplicate/empty header");
    }
  }
  const std::vector<std::string> required = {
      "recording_frame_id", "local_frame_id", "camera_frame_id",
      "timestamp",          "timestamp_sys",   "has_detection",
      "blank_frame",       "detection_confidence",
      "crop_x",            "crop_y",           "crop_w",
      "crop_h",            "detection_x",      "detection_y",
      "detection_w",       "detection_h"};
  for (const auto& name : required) {
    if (column.find(name) == column.end()) {
      return Fail(error_message,
                  "Crop metadata CSV is missing column '" + name + "'");
    }
  }

  rows->clear();
  rows->reserve(static_cast<size_t>(descriptor.frame_count));
  while (true) {
    fields.clear();
    csv_error.clear();
    if (!reader.next(&fields, &eof, &csv_error)) {
      return Fail(error_message,
                  "Crop metadata CSV parse error at row " +
                      std::to_string(rows->size()) + ": " + csv_error);
    }
    if (eof) {
      break;
    }
    if (fields.size() != column.size()) {
      return Fail(error_message,
                  "Crop metadata CSV field count mismatch at row " +
                      std::to_string(rows->size()));
    }

    auto integer = [&](const char* name, int64_t* output) {
      return ParseInt64(fields[column.at(name)], output);
    };
    auto number = [&](const char* name, double* output) {
      return ParseDouble(fields[column.at(name)], output);
    };

    AcquisitionCropFrameRow row;
    int64_t has_detection = 0;
    int64_t blank_frame = 0;
    double crop_x = 0.0;
    double crop_y = 0.0;
    double crop_w = 0.0;
    double crop_h = 0.0;
    double detection_x = 0.0;
    double detection_y = 0.0;
    double detection_w = 0.0;
    double detection_h = 0.0;
    const bool parsed =
        integer("recording_frame_id", &row.recording_frame_id) &&
        integer("local_frame_id", &row.local_frame_id) &&
        integer("camera_frame_id", &row.camera_frame_id) &&
        integer("timestamp", &row.timestamp) &&
        integer("timestamp_sys", &row.timestamp_sys) &&
        integer("has_detection", &has_detection) &&
        integer("blank_frame", &blank_frame) &&
        number("detection_confidence", &row.detection_confidence) &&
        number("crop_x", &crop_x) && number("crop_y", &crop_y) &&
        number("crop_w", &crop_w) && number("crop_h", &crop_h) &&
        number("detection_x", &detection_x) &&
        number("detection_y", &detection_y) &&
        number("detection_w", &detection_w) &&
        number("detection_h", &detection_h);
    if (!parsed || (has_detection != 0 && has_detection != 1) ||
        (blank_frame != 0 && blank_frame != 1)) {
      return Fail(error_message,
                  "Crop metadata CSV has invalid value at row " +
                      std::to_string(rows->size()));
    }

    const int64_t expected_recording =
        static_cast<int64_t>(rows->size()) + 1;
    if (row.recording_frame_id != expected_recording) {
      return Fail(error_message,
                  "Crop recording_frame_id is not row+1 at row " +
                      std::to_string(rows->size()));
    }
    row.has_detection = has_detection != 0;
    row.blank_frame = blank_frame != 0;
    if (row.detection_confidence < 0.0 || row.detection_confidence > 1.0 ||
        (row.blank_frame && row.detection_confidence != 0.0)) {
      return Fail(error_message,
                  "Crop detection confidence is invalid at row " +
                      std::to_string(rows->size()));
    }
    if (row.blank_frame == row.has_detection) {
      return Fail(error_message,
                  "Crop blank/detection flags disagree at row " +
                      std::to_string(rows->size()));
    }
    row.full_frame_crop = crop::CropRect{crop_x, crop_y, crop_w, crop_h};
    const crop::CropRect detection{detection_x, detection_y,
                                   detection_w, detection_h};
    if (row.blank_frame) {
      if (crop_x != 0.0 || crop_y != 0.0 || crop_w != 0.0 || crop_h != 0.0 ||
          detection_x != 0.0 || detection_y != 0.0 || detection_w != 0.0 ||
          detection_h != 0.0) {
        return Fail(error_message,
                    "Blank crop row has nonzero geometry at row " +
                        std::to_string(rows->size()));
      }
    } else {
      if (crop_x < 0.0 || crop_y < 0.0 || !row.full_frame_crop.valid() ||
          detection_x < 0.0 || detection_y < 0.0 || !detection.valid() ||
          !row.full_frame_crop.contains(detection)) {
        return Fail(error_message,
                    "Detected crop row has invalid geometry at row " +
                        std::to_string(rows->size()));
      }
      row.full_frame_detection = detection;
    }
    rows->push_back(std::move(row));
  }

  if (static_cast<int64_t>(rows->size()) != descriptor.frame_count) {
    return Fail(error_message,
                "Crop CSV row count disagrees with contract frame count");
  }
  return true;
}

bool ValidateKeyframes(AcquisitionCropStreamDescriptor* descriptor,
                       std::string* error_message) {
  json keyframes;
  if (!ReadJsonFile(descriptor->resolved_keyframes_path, &keyframes,
                    error_message)) {
    return false;
  }
  int64_t total_frames = 0;
  double fps = 0.0;
  std::string codec;
  if (!RequiredInt64(keyframes, "total_frames", "crop keyframes",
                     &total_frames, error_message) ||
      !RequiredDouble(keyframes, "fps", "crop keyframes", &fps,
                      error_message) ||
      !RequiredString(keyframes, "codec", "crop keyframes", &codec,
                      error_message)) {
    return false;
  }
  if (total_frames != descriptor->frame_count ||
      std::abs(fps - descriptor->frame_rate) > 1e-6 ||
      codec != descriptor->codec) {
    return Fail(error_message,
                "Crop keyframe metadata disagrees with stream contract");
  }
  auto frames = keyframes.find("keyframe_frames");
  if (frames == keyframes.end() || !frames->is_array() || frames->empty()) {
    return Fail(error_message, "Crop keyframes has no keyframe_frames");
  }
  int64_t previous = -1;
  for (const auto& frame : *frames) {
    if (!frame.is_number_integer()) {
      return Fail(error_message, "Crop keyframe identity is not an integer");
    }
    const int64_t value = frame.get<int64_t>();
    if (value <= previous || value < 0 || value >= descriptor->frame_count) {
      return Fail(error_message, "Crop keyframe identities are invalid");
    }
    previous = value;
  }
  descriptor->keyframe_count = static_cast<int64_t>(frames->size());
  descriptor->first_keyframe = frames->front().get<int64_t>();
  descriptor->last_keyframe = frames->back().get<int64_t>();
  if (descriptor->first_keyframe != 0) {
    return Fail(error_message, "Crop keyframe list does not begin at frame 0");
  }
  return true;
}

bool ValidateSummaryAndStatus(
    const AcquisitionCropStreamDescriptor& descriptor,
    std::string* error_message) {
  json summary;
  if (!ReadJsonFile(descriptor.resolved_summary_path, &summary,
                    error_message)) {
    return false;
  }
  int64_t received = 0;
  int64_t encoded = 0;
  std::string codec;
  if (!RequiredInt64(summary, "frames_received", "crop summary", &received,
                     error_message) ||
      !RequiredInt64(summary, "frames_encoded", "crop summary", &encoded,
                     error_message) ||
      !RequiredString(summary, "codec", "crop summary", &codec,
                      error_message)) {
    return false;
  }
  if (received != descriptor.frame_count || encoded != descriptor.frame_count ||
      codec != descriptor.codec) {
    return Fail(error_message,
                "Crop summary disagrees with stream contract");
  }

  if (descriptor.resolved_status_path.empty() ||
      !std::filesystem::is_regular_file(descriptor.resolved_status_path)) {
    return true;
  }
  json status;
  if (!ReadJsonFile(descriptor.resolved_status_path, &status, error_message)) {
    return false;
  }
  int64_t status_received = 0;
  int64_t status_encoded = 0;
  int64_t dropped = 0;
  std::string state;
  if (!RequiredString(status, "status", "crop status", &state,
                      error_message) ||
      !RequiredInt64(status, "frames_received", "crop status",
                     &status_received, error_message) ||
      !RequiredInt64(status, "frames_encoded", "crop status",
                     &status_encoded, error_message) ||
      !RequiredInt64(status, "frames_dropped", "crop status", &dropped,
                     error_message)) {
    return false;
  }
  if (state != "completed" || status_received != descriptor.frame_count ||
      status_encoded != descriptor.frame_count || dropped != 0) {
    return Fail(error_message,
                "Crop status does not describe a complete identity stream");
  }
  return true;
}

}  // namespace

std::unique_ptr<AcquisitionCropRepository>
OpenAcquisitionCropRepository(
    const std::shared_ptr<ArchiveContext>& archive,
    std::string* error_message) {
  if (!archive || !archive->impl_) {
    internal::SetArchiveError(error_message, "Archive context is not open");
    return nullptr;
  }
  const auto root = internal::ReadArchiveAttributes(
      *archive->impl_, std::string(kInventoryPath));
  const auto crop = internal::ReadArchiveAttributes(
      *archive->impl_, std::string(kCropStreamPath));
  if (!root || !crop) {
    internal::SetArchiveError(
        error_message,
        "Archive has no acquisition crop inventory attributes");
    return nullptr;
  }
  auto streams = root->find("streams");
  if (streams == root->end() || !streams->is_object() ||
      !streams->contains("crop") || !(*streams)["crop"].is_object()) {
    internal::SetArchiveError(
        error_message,
        "Acquisition inventory has no duplicated crop descriptor");
    return nullptr;
  }
  if ((*streams)["crop"] != *crop) {
    internal::SetArchiveError(
        error_message,
        "Acquisition root and child crop descriptors disagree");
    return nullptr;
  }
  if (!ValidateGeometryColumns(*crop, error_message)) {
    return nullptr;
  }

  AcquisitionCropStreamDescriptor descriptor;
  int64_t imported_row_count = 0;
  if (!ParseDescriptor(*archive, *root, *crop, &descriptor,
                       &imported_row_count, error_message) ||
      !ValidateKeyframes(&descriptor, error_message) ||
      !ValidateSummaryAndStatus(descriptor, error_message)) {
    return nullptr;
  }
  std::vector<AcquisitionCropFrameRow> rows;
  if (!ParseCropCsv(descriptor, &rows, error_message)) {
    return nullptr;
  }
  return MakeAcquisitionCropRepository(std::move(descriptor),
                                       std::move(rows));
}

}  // namespace crimson::zarr
