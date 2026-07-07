#include "IconsForkAwesome.h"
#include "Logger.h"
#include "camera.h"
#include "filesystem"
#include "global.h"
#include "gui.h"
#include "legacy_labeling_state.h"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"
#include "render.h"
#include "skeleton.h"
#include "utils.h"
#include "debug_flags.h"
#include "yolo_detection.h"
#include <ImGuiFileDialog.h>
#include <algorithm>
#include <cctype>
#include <deque>
#include <cmath>
#include <chrono>
#include <array>
#include <functional>
#include <future>
#include <iomanip>
#include <limits>
#include <sstream>
#include <numeric>
#include <memory>
#include <optional>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <iostream>
#include <ctime>
#include <stdio.h>
#include <stdlib.h>
#include <thread>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include "perf_logging.h"
#include "media_session_loader.h"
#include "playback_session_controller.h"
#include "decode_debug_workflow.h"
#include "manual_detect_payload_preview.h"
#include "chained_crop_image_provider.h"
#include "live_crop_image_provider.h"
#include "refined_keypoint_repository.h"
#include "review_frame_index.h"
#include "zarr_persisted_crop_provider.h"
#include "zarr_loader.h"
#include "gui/file_browser_window.h"
#include "gui/crop_preview_window.h"
#include "gui/diagnostics_window.h"
#include "gui/frame_debug_window.h"
#include "gui/full_frame_rect_edit_overlay.h"
#include "gui/keypoints_window.h"
#include "gui/labeling_tool_window.h"
#include "gui/camera_view_overlay_renderer.h"
#include "gui/refined_keypoint_review_window.h"
#include "gui/refined_keypoint_write_workflow.h"
#include "gui/labeling_tool_workflow.h"
#include "gui/auxiliary_windows.h"
#include "gui/camera_view_manual_keypoint_input.h"
#include "gui/camera_view_presenter.h"
#include "gui/camera_view_frame_context_builder.h"
#include "gui/camera_view_window.h"
#include "gui/stimulus_playback_windows.h"
#include "gui/camera_view_transport_controls.h"
#include "gui_interpolation.h"
#include "gui/analysis_timeline_window.h"
#include "gui/stimulus_event_timeline_window.h"
#include <nlohmann/json.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#if defined(_MSC_VER) && (_MSC_VER >= 1900) &&                                 \
    !defined(IMGUI_DISABLE_WIN32_FUNCTIONS)
#pragma comment(lib, "legacy_stdio_definitions")
#endif

simplelogger::Logger *logger =
    simplelogger::LoggerFactory::CreateConsoleLogger();

#include "ui_path_config.h"
#include "windows_crash_dump.h"
#include "zarr_bbox_edit.h"

std::vector<std::mutex> g_mutexes(MAX_VIEWS);
std::vector<std::condition_variable> g_cvs(MAX_VIEWS);
std::vector<bool> g_ready(MAX_VIEWS);
std::vector<std::vector<cv::Rect>> yolo_boxes(MAX_VIEWS);
std::vector<std::vector<std::string>> yolo_labels(MAX_VIEWS);
std::vector<std::vector<int>> yolo_classid(MAX_VIEWS);
std::vector<unsigned char *> yolo_input_frames_rgba(MAX_VIEWS);
std::unordered_map<std::string, std::atomic<bool>> window_need_decoding;
std::unordered_map<std::string, std::atomic<int>> latest_decoded_frame;
std::unordered_map<std::string, std::shared_ptr<DecoderPerfSample>> decoder_perf_samples;
std::mutex g_seek_info_mutex;
std::mutex g_decoder_perf_mutex;

// Global variables
bool show_interpolation_debug = false;
std::vector<ZarrDetectionLoader::DetectionDataset> detection_dataset_ids;
std::vector<std::string> detection_dataset_labels;
int detection_dataset_choice = 0;
ZarrBBoxEditState g_zarr_bbox_edit_state;

#include "review_frame_state.h"

void refreshDetectionDatasetOptions(ZarrDetectionLoader& loader) {
    detection_dataset_ids.clear();
    detection_dataset_labels.clear();
    detection_dataset_choice = 0;
    auto options = loader.getAvailableDetectionDatasets();
    auto active = loader.getActiveDetectionDataset();
    for (size_t i = 0; i < options.size(); ++i) {
        detection_dataset_ids.push_back(options[i].first);
        detection_dataset_labels.push_back(options[i].second);
        if (options[i].first == active) {
            detection_dataset_choice = static_cast<int>(i);
        }
    }
}

#include "stimulus_playback.h"

static StimulusPlayback stimulus_player;

namespace {

using json = nlohmann::json;

struct PlaybackTraceLogWriter {
    std::ofstream stream;
    std::filesystem::path jsonl_path;
    std::string label = "PlaybackTrace";
    std::chrono::steady_clock::time_point start_steady{};
    uint64_t sequence = 0;
    int samples_since_flush = 0;

    bool open(const std::filesystem::path& output_path,
              const std::string& log_label = "PlaybackTrace") {
        if (output_path.empty()) {
            return false;
        }
        label = log_label.empty() ? "PlaybackTrace" : log_label;
        jsonl_path = output_path;
        std::error_code ec;
        if (jsonl_path.has_parent_path()) {
            std::filesystem::create_directories(jsonl_path.parent_path(), ec);
            if (ec) {
                std::cerr << "[" << label << "] Failed to create parent "
                          << "directory for " << jsonl_path << ": "
                          << ec.message() << std::endl;
                return false;
            }
        }
        stream.open(jsonl_path, std::ios::out | std::ios::trunc);
        if (!stream.is_open()) {
            std::cerr << "[" << label << "] Failed to open " << jsonl_path
                      << " for writing" << std::endl;
            return false;
        }
        start_steady = std::chrono::steady_clock::now();
        std::cout << "[" << label << "] Writing JSONL samples to " << jsonl_path
                  << std::endl;
        return true;
    }

    bool enabled() const { return stream.is_open(); }

    void write(json sample, bool force_flush = false) {
        if (!enabled()) {
            return;
        }
        const auto now_steady = std::chrono::steady_clock::now();
        const auto now_system = std::chrono::system_clock::now();
        sample["format"] = "crimson_playback_trace_v1";
        sample["sequence"] = sequence++;
        sample["elapsed_s"] =
            std::chrono::duration<double>(now_steady - start_steady).count();
        sample["wall_epoch_ms"] =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now_system.time_since_epoch())
                .count();
        stream << sample.dump() << "\n";
        samples_since_flush++;
        if (force_flush || samples_since_flush >= 30) {
            stream.flush();
            samples_since_flush = 0;
        }
    }
};

struct FrameSyncTraceLastState {
    bool initialized = false;
    bool has_presented_frame = false;
    int target_frame = std::numeric_limits<int>::min();
    int presented_frame = std::numeric_limits<int>::min();
    int bbox_query_frame = std::numeric_limits<int>::min();
    int latest_decoded_frame = std::numeric_limits<int>::min();
    int front_frame_before_draw = std::numeric_limits<int>::min();
    int front_frame_after_draw = std::numeric_limits<int>::min();
    int staging_frame_before_draw = std::numeric_limits<int>::min();
    int staging_frame_after_draw = std::numeric_limits<int>::min();
    int zarr_box_count = std::numeric_limits<int>::min();
};

struct ClippedBoundarySmokeConfig {
    bool enabled = false;
    int start_frame = -1;
    int end_frame = -1;
    std::chrono::steady_clock::time_point start_time{};
    bool started = false;
    bool completed = false;
};

struct PlaybackSmokeConfig {
    bool enabled = false;
    int start_frame = -1;
    int end_frame = -1;
    double timeout_s = 20.0;
    std::chrono::steady_clock::time_point start_time{};
    bool started = false;
    bool completed = false;
    int presented_count = 0;
    int last_presented_frame = -1;
    int max_presented_frame = -1;
    int last_presented_slot = -1;
    int last_view_idx = -1;
};

struct ClippedTextureDumpConfig {
    bool enabled = false;
    int parent_frame = -1;
    std::filesystem::path output_path;
    bool dumped = false;
};

struct GlTextureDumpResult {
    bool ok = false;
    int width = 0;
    int height = 0;
    std::filesystem::path raw_path;
    std::filesystem::path flip_y_path;
    std::string error;
};

struct ClippedTraceDeltaStats {
    bool initialized = false;
    int64_t min_delta = 0;
    int64_t max_delta = 0;
    std::unordered_map<int64_t, uint64_t> histogram;

    void add(int64_t delta) {
        if (!initialized) {
            min_delta = delta;
            max_delta = delta;
            initialized = true;
        } else {
            min_delta = std::min(min_delta, delta);
            max_delta = std::max(max_delta, delta);
        }
        histogram[delta]++;
    }

    json summaryJson() const {
        if (!initialized) {
            return nullptr;
        }
        int64_t common_delta = 0;
        uint64_t common_count = 0;
        for (const auto& entry : histogram) {
            if (entry.second > common_count ||
                (entry.second == common_count &&
                 entry.first < common_delta)) {
                common_delta = entry.first;
                common_count = entry.second;
            }
        }
        return json{{"min", min_delta},
                    {"max", max_delta},
                    {"most_common", common_delta},
                    {"most_common_count", common_count}};
    }
};

struct ClippedFrameTraceStats {
    uint64_t frames_traced = 0;
    uint64_t parent_mismatches = 0;
    uint64_t bbox_parent_mismatches = 0;
    uint64_t bbox_local_mismatches = 0;
    uint64_t decoder_local_mismatches = 0;
    uint64_t front_texture_before_mismatches = 0;
    uint64_t front_texture_after_mismatches = 0;
    uint64_t texture_draw_callback_missing = 0;
    uint64_t texture_draw_bound_mismatches = 0;
    ClippedTraceDeltaStats decoder_local_delta;
    ClippedTraceDeltaStats bbox_parent_delta;
    ClippedTraceDeltaStats bbox_local_delta;

    json summaryJson() const {
        return json{
            {"frames_traced", frames_traced},
            {"mismatches",
             {{"parent_matches_resolver", parent_mismatches},
              {"bbox_parent_matches_display", bbox_parent_mismatches},
              {"bbox_local_matches_resolver", bbox_local_mismatches},
              {"decoder_local_matches_resolver", decoder_local_mismatches},
              {"front_texture_matches_resolver_before_draw",
               front_texture_before_mismatches},
              {"front_texture_matches_resolver_after_draw",
               front_texture_after_mismatches},
              {"texture_draw_callback_missing",
               texture_draw_callback_missing},
              {"texture_draw_bound_mismatches",
               texture_draw_bound_mismatches}}},
            {"deltas",
             {{"decoder_presented_local_minus_clip_local",
               decoder_local_delta.summaryJson()},
              {"bbox_query_parent_minus_current_parent",
               bbox_parent_delta.summaryJson()},
              {"bbox_query_local_minus_clip_local",
               bbox_local_delta.summaryJson()}}}};
    }
};

double durationMs(std::chrono::steady_clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

bool parseIntArgument(const char* text, int& out) {
    if (text == nullptr || *text == '\0') {
        return false;
    }
    char* end = nullptr;
    long value = std::strtol(text, &end, 10);
    if (end == text || *end != '\0' ||
        value < std::numeric_limits<int>::min() ||
        value > std::numeric_limits<int>::max()) {
        return false;
    }
    out = static_cast<int>(value);
    return true;
}

std::filesystem::path pathWithStemSuffix(const std::filesystem::path& path,
                                         const std::string& suffix) {
    const std::filesystem::path parent = path.parent_path();
    const std::string stem = path.stem().string();
    const std::string extension = path.extension().string();
    return parent / (stem + suffix + extension);
}

std::filesystem::path pathWithExtension(const std::filesystem::path& path,
                                        const std::string& extension) {
    std::filesystem::path result = path;
    result.replace_extension(extension);
    return result;
}

GlTextureDumpResult dumpGlTextureToPng(GLuint texture_id,
                                       const std::filesystem::path& output_path) {
    GlTextureDumpResult result;
    result.raw_path = output_path;
    result.flip_y_path = pathWithStemSuffix(output_path, "_flip_y");

    if (texture_id == 0) {
        result.error = "texture id is 0";
        return result;
    }

    std::error_code ec;
    if (result.raw_path.has_parent_path()) {
        std::filesystem::create_directories(result.raw_path.parent_path(), ec);
        if (ec) {
            result.error = "failed to create output directory: " + ec.message();
            return result;
        }
    }

    GLint previous_active_texture = 0;
    GLint previous_texture0 = 0;
    GLint previous_pack_alignment = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previous_active_texture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture0);
    glGetIntegerv(GL_PACK_ALIGNMENT, &previous_pack_alignment);

    glBindTexture(GL_TEXTURE_2D, texture_id);
    GLint texture_width = 0;
    GLint texture_height = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH,
                             &texture_width);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT,
                             &texture_height);
    if (texture_width <= 0 || texture_height <= 0) {
        result.error = "texture has invalid dimensions";
        glBindTexture(GL_TEXTURE_2D, previous_texture0);
        glActiveTexture(previous_active_texture);
        return result;
    }

    result.width = texture_width;
    result.height = texture_height;
    std::vector<unsigned char> rgba(
        static_cast<size_t>(texture_width) *
        static_cast<size_t>(texture_height) * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    const GLenum gl_error = glGetError();
    glPixelStorei(GL_PACK_ALIGNMENT, previous_pack_alignment);
    glBindTexture(GL_TEXTURE_2D, previous_texture0);
    glActiveTexture(previous_active_texture);

    if (gl_error != GL_NO_ERROR) {
        std::ostringstream error;
        error << "glGetTexImage failed with GL error 0x" << std::hex
              << gl_error;
        result.error = error.str();
        return result;
    }

    cv::Mat rgba_image(texture_height, texture_width, CV_8UC4, rgba.data());
    cv::Mat bgra_image;
    cv::cvtColor(rgba_image, bgra_image, cv::COLOR_RGBA2BGRA);
    if (!cv::imwrite(result.raw_path.string(), bgra_image)) {
        result.error = "failed to write " + result.raw_path.string();
        return result;
    }

    cv::Mat flip_y_image;
    cv::flip(bgra_image, flip_y_image, 0);
    if (!cv::imwrite(result.flip_y_path.string(), flip_y_image)) {
        result.error = "failed to write " + result.flip_y_path.string();
        return result;
    }

    result.ok = true;
    return result;
}

bool parseFrameRangeArgument(const char* text, int& start, int& end) {
    if (text == nullptr || *text == '\0') {
        return false;
    }
    const std::string value(text);
    const size_t colon = value.find(':');
    if (colon == std::string::npos || colon == 0 ||
        colon + 1 >= value.size()) {
        return false;
    }
    int parsed_start = -1;
    int parsed_end = -1;
    if (!parseIntArgument(value.substr(0, colon).c_str(), parsed_start) ||
        !parseIntArgument(value.substr(colon + 1).c_str(), parsed_end) ||
        parsed_start < 0 || parsed_end < parsed_start) {
        return false;
    }
    start = parsed_start;
    end = parsed_end;
    return true;
}

bool parseDoubleArgument(const char* text, double& out) {
    if (text == nullptr || *text == '\0') {
        return false;
    }
    char* end = nullptr;
    double value = std::strtod(text, &end);
    if (end == text || *end != '\0' || !std::isfinite(value)) {
        return false;
    }
    out = value;
    return true;
}

struct PendingKeypointWriteResult {
    CropKeypointEditorActionType action_type = CropKeypointEditorActionType::None;
    RefinedKeypointSelection selection;
    RefinedKeypointEditResult edit_result;
    bool ok = false;
    std::string error_message;
    std::string success_status;
};

struct PendingKeypointWriteState {
    bool active = false;
    bool reset_crop_editor = false;
    bool reset_full_frame_editor = false;
    std::string archive_path;
    std::future<PendingKeypointWriteResult> future;
};

std::string keypointWriteFailurePrefix(CropKeypointEditorActionType action_type) {
    switch (action_type) {
    case CropKeypointEditorActionType::Save:
        return "Keypoint edit failed: ";
    case CropKeypointEditorActionType::MarkNoKeypoints:
        return "Mark no keypoints failed: ";
    case CropKeypointEditorActionType::MarkDetectionIssue:
        return "Mark detection issue failed: ";
    case CropKeypointEditorActionType::Reset:
    case CropKeypointEditorActionType::None:
    default:
        return "Keypoint write failed: ";
    }
}

std::string keypointWriteReloadFailurePrefix(CropKeypointEditorActionType action_type) {
    switch (action_type) {
    case CropKeypointEditorActionType::Save:
        return "Keypoint edit saved but reload failed: ";
    case CropKeypointEditorActionType::MarkNoKeypoints:
        return "Marked fish_present_no_keypoints but reload failed: ";
    case CropKeypointEditorActionType::MarkDetectionIssue:
        return "Marked detection_issue but reload failed: ";
    case CropKeypointEditorActionType::Reset:
    case CropKeypointEditorActionType::None:
    default:
        return "Keypoint write succeeded but reload failed: ";
    }
}

std::string keypointWriteStartStatus(CropKeypointEditorActionType action_type,
                                     const RefinedKeypointSelection& selection) {
    std::ostringstream status;
    switch (action_type) {
    case CropKeypointEditorActionType::Save:
        status << "Saving keypoint edit";
        break;
    case CropKeypointEditorActionType::MarkNoKeypoints:
        status << "Marking fish_present_no_keypoints";
        break;
    case CropKeypointEditorActionType::MarkDetectionIssue:
        status << "Marking detection_issue";
        break;
    case CropKeypointEditorActionType::Reset:
    case CropKeypointEditorActionType::None:
    default:
        status << "Writing keypoint update";
        break;
    }
    status << ": roi=" << selection.roi_index << " ...";
    return status.str();
}

std::string keypointWriteSuccessStatus(
    CropKeypointEditorActionType action_type,
    const RefinedKeypointSelection& selection,
    const RefinedKeypointEditResult& edit_result) {
    std::ostringstream status;
    if (action_type == CropKeypointEditorActionType::Save) {
        status << (edit_result.changed ? "Keypoint edit saved"
                                       : "Keypoint edit was a no-op");
    } else if (action_type == CropKeypointEditorActionType::MarkNoKeypoints) {
        status << "Marked fish_present_no_keypoints";
    } else if (action_type == CropKeypointEditorActionType::MarkDetectionIssue) {
        status << "Marked detection_issue";
    } else {
        status << "Keypoint write";
    }
    status << ": roi=" << selection.roi_index;
    if (edit_result.summary_updated) {
        status << " summary=updated";
    }
    if (edit_result.stale_eye_mask_runs > 0) {
        status << " stale_eye_masks=" << edit_result.stale_eye_mask_runs;
    }
    return status.str();
}

PendingKeypointWriteResult runKeypointWrite(
    const ZarrDetectionLoader& loader,
    const CropKeypointEditorAction& action,
    const std::optional<RefinedKeypointSelection>& selection) {
    PendingKeypointWriteResult result;
    result.action_type = action.type;
    if (!selection.has_value()) {
        result.error_message = "Keypoint write failed: No keypoint selection.";
        return result;
    }
    result.selection = *selection;

    RefinedKeypointRepository refined_keypoint_repo(loader);
    std::string write_error;
    switch (action.type) {
    case CropKeypointEditorActionType::Save:
        result.ok = refined_keypoint_repo.writeManualCorrection(
            *selection, action.keypoints_roi, write_error, &result.edit_result);
        break;
    case CropKeypointEditorActionType::MarkNoKeypoints:
        result.ok = refined_keypoint_repo.markFishPresentNoKeypoints(
            *selection, write_error, &result.edit_result);
        break;
    case CropKeypointEditorActionType::MarkDetectionIssue:
        result.ok = refined_keypoint_repo.markDetectionIssue(
            *selection, write_error, &result.edit_result);
        break;
    case CropKeypointEditorActionType::Reset:
    case CropKeypointEditorActionType::None:
    default:
        result.error_message = "Keypoint write failed: No keypoint write action.";
        return result;
    }

    if (!result.ok) {
        result.error_message =
            keypointWriteFailurePrefix(action.type) + write_error;
        return result;
    }
    result.success_status =
        keypointWriteSuccessStatus(action.type, *selection, result.edit_result);
    return result;
}

PendingKeypointWriteResult runKeypointWriteFromArchive(
    const std::string& archive_path,
    const CropKeypointEditorAction& action,
    const std::optional<RefinedKeypointSelection>& selection) {
    PendingKeypointWriteResult result;
    result.action_type = action.type;
    if (selection.has_value()) {
        result.selection = *selection;
    }
    if (archive_path.empty()) {
        result.error_message = "Keypoint write failed: No loaded Zarr archive.";
        return result;
    }

    ZarrDetectionLoader worker_loader;
    std::string load_error;
    if (!worker_loader.loadZarrFile(archive_path, load_error)) {
        result.error_message =
            keypointWriteFailurePrefix(action.type) +
            "Worker failed to load active Zarr: " + load_error;
        return result;
    }
    return runKeypointWrite(worker_loader, action, selection);
}

void startKeypointWriteIfRequested(
    PendingKeypointWriteState& pending_write,
    const ZarrDetectionLoader& loader,
    const CropKeypointEditorAction& action,
    const std::optional<RefinedKeypointSelection>& selection,
    bool reset_crop_editor,
    bool reset_full_frame_editor,
    std::string& status_out) {
    if (action.type == CropKeypointEditorActionType::None) {
        return;
    }
    if (!selection.has_value()) {
        status_out = "Keypoint write failed: No keypoint selection.";
        return;
    }
    if (pending_write.active) {
        status_out = "Keypoint write already in progress.";
        return;
    }

    const std::string archive_path = loader.getArchivePath();
    pending_write.active = true;
    pending_write.reset_crop_editor = reset_crop_editor;
    pending_write.reset_full_frame_editor = reset_full_frame_editor;
    pending_write.archive_path = archive_path;
    status_out = keypointWriteStartStatus(action.type, *selection);
    pending_write.future = std::async(
        std::launch::async,
        [archive_path, action, selection]() {
            return runKeypointWriteFromArchive(archive_path, action, selection);
        });
}

void pollPendingKeypointWrite(
    PendingKeypointWriteState& pending_write,
    ZarrDetectionLoader& loader,
    CropKeypointEditorState& crop_editor_state,
    FullFrameKeypointEditState& full_frame_editor_state,
    std::string& status_out,
    const std::function<bool(std::string&)>& reload_active_zarr,
    const std::function<void()>& invalidate_after_write) {
    if (!pending_write.active || !pending_write.future.valid()) {
        return;
    }
    if (pending_write.future.wait_for(std::chrono::milliseconds(0)) !=
        std::future_status::ready) {
        return;
    }

    PendingKeypointWriteResult result = pending_write.future.get();
    const bool reset_crop_editor = pending_write.reset_crop_editor;
    const bool reset_full_frame_editor = pending_write.reset_full_frame_editor;
    const std::string archive_path = pending_write.archive_path;
    pending_write = PendingKeypointWriteState{};

    if (!result.ok) {
        status_out = result.error_message;
        return;
    }

    if (loader.getArchivePath() != archive_path) {
        status_out =
            result.success_status + " (active Zarr changed; skipped reload)";
        return;
    }

    if (reset_crop_editor) {
        resetCropKeypointEditorState(crop_editor_state);
    }
    if (reset_full_frame_editor) {
        resetFullFrameKeypointEditState(full_frame_editor_state);
    }

    std::string cache_error;
    if (!loader.applyRefinedKeypointCacheUpdate(
            result.edit_result.cache_update, &cache_error)) {
        std::string reload_error;
        if (!reload_active_zarr(reload_error)) {
            status_out =
                keypointWriteReloadFailurePrefix(result.action_type) +
                reload_error + " (cache update failed: " + cache_error + ")";
            return;
        }
        if (invalidate_after_write) {
            invalidate_after_write();
        }
        status_out =
            result.success_status +
            " (reloaded; targeted cache update failed: " + cache_error + ")";
        return;
    }

    if (invalidate_after_write) {
        invalidate_after_write();
    }
    status_out = result.success_status;
}

std::optional<int> ParseCudaDeviceIndexString(const std::string& value) {
    if (value.empty()) {
        return std::nullopt;
    }

    try {
        size_t consumed = 0;
        const int parsed = std::stoi(value, &consumed);
        if (consumed != value.size() || parsed < 0) {
            return std::nullopt;
        }
        return parsed;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::filesystem::path> GetCudaDeviceConfigPath() {
    if (const char* explicit_path = std::getenv("CRIMSON_CUDA_DEVICE_CONFIG");
        explicit_path && *explicit_path != '\0') {
        return std::filesystem::path(explicit_path);
    }

#ifdef _WIN32
    if (const char* localappdata = std::getenv("LOCALAPPDATA");
        localappdata && *localappdata != '\0') {
        return std::filesystem::path(localappdata) / "Crimson" / "config" /
               "cuda_device.json";
    }
    if (const char* appdata = std::getenv("APPDATA");
        appdata && *appdata != '\0') {
        return std::filesystem::path(appdata) / "crimson" / "cuda_device.json";
    }
#else
    if (const char* xdg_config_home = std::getenv("XDG_CONFIG_HOME");
        xdg_config_home && *xdg_config_home != '\0') {
        return std::filesystem::path(xdg_config_home) / "crimson" /
               "cuda_device.json";
    }
#endif

    if (const char* home = std::getenv("HOME"); home && *home != '\0') {
        return std::filesystem::path(home) / ".config" / "crimson" /
               "cuda_device.json";
    }
    return std::nullopt;
}

std::optional<int> LoadSavedCudaDeviceIndex(
    const std::filesystem::path& config_path,
    std::string& source_description) {
    if (config_path.empty()) {
        return std::nullopt;
    }

    std::ifstream config_stream(config_path);
    if (!config_stream.is_open()) {
        return std::nullopt;
    }

    try {
        json payload = json::parse(config_stream);
        auto selected_index = payload.find("selected_cuda_device_index");
        if (selected_index == payload.end() || !selected_index->is_number_integer()) {
            return std::nullopt;
        }
        const int parsed = selected_index->get<int>();
        if (parsed < 0) {
            return std::nullopt;
        }
        source_description = config_path.string();
        return parsed;
    } catch (const std::exception& exc) {
        std::cerr << "[CudaDevice] Ignoring unreadable saved CUDA device config "
                  << config_path << ": " << exc.what() << std::endl;
        return std::nullopt;
    }
}

int ResolveCudaDeviceIndex() {
    if (const char* env_device = std::getenv("CRIMSON_CUDA_DEVICE_INDEX");
        env_device && *env_device != '\0') {
        if (auto parsed = ParseCudaDeviceIndexString(std::string(env_device))) {
            std::cout << "[CudaDevice] Using GPU " << *parsed
                      << " from CRIMSON_CUDA_DEVICE_INDEX" << std::endl;
            return *parsed;
        }
        std::cerr << "[CudaDevice] Ignoring invalid CRIMSON_CUDA_DEVICE_INDEX="
                  << env_device << std::endl;
    }

    if (auto config_path = GetCudaDeviceConfigPath()) {
        std::string source_description;
        if (auto saved_index =
                LoadSavedCudaDeviceIndex(*config_path, source_description)) {
            std::cout << "[CudaDevice] Using GPU " << *saved_index
                      << " from " << source_description << std::endl;
            return *saved_index;
        }
    }

    constexpr int default_cuda_device_index = 0;
    std::cout << "[CudaDevice] Using default GPU "
              << default_cuda_device_index << std::endl;
    return default_cuda_device_index;
}



}  // namespace

int main(int argc, char **argv) {
    std::string cli_zarr_override_path;
    std::string cli_recording_path;
    std::string cli_subject_shape_run;
    std::string cli_refined_subject_mask_run;
    std::string cli_refined_subject_mask_storage;
    std::string cli_tail_kinematics_run;
    std::string cli_eye_angle_run;
    std::string cli_stimulus_run;
    std::filesystem::path cli_perf_log_path;
    std::filesystem::path cli_mask_perf_log_path;
    std::filesystem::path cli_playback_trace_log_path;
    std::filesystem::path cli_frame_sync_trace_log_path;
    int cli_swap_interval = 1;
    int cli_mask_perf_sample_every = 10;
    double cli_frame_cap_fps = 0.0;
    bool mask_perf_log_enabled = true;
    bool cli_show_eye_masks = false;
    PlaybackSmokeConfig playback_smoke;
    ClippedBoundarySmokeConfig clipped_boundary_smoke;
    int app_exit_code = 0;
    const std::filesystem::path argv0_path = (argc > 0) ? argv[0] : "";
    InstallWindowsCrashHandler(argv0_path);
    std::error_code cwd_error;
    const std::filesystem::path cwd = std::filesystem::current_path(cwd_error);
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--zarr") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for --zarr" << std::endl;
                return 1;
            }
            cli_zarr_override_path = argv[++i];
            continue;
        }
        if (arg == "--recording") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for --recording" << std::endl;
                return 1;
            }
            cli_recording_path = argv[++i];
            continue;
        }
        if (arg == "--subject-shape-run") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for --subject-shape-run"
                          << std::endl;
                return 1;
            }
            cli_subject_shape_run = argv[++i];
            continue;
        }
        if (arg == "--refined-subject-mask-run") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for --refined-subject-mask-run"
                          << std::endl;
                return 1;
            }
            cli_refined_subject_mask_run = argv[++i];
            continue;
        }
        if (arg == "--refined-subject-mask-storage") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for --refined-subject-mask-storage"
                          << std::endl;
                return 1;
            }
            cli_refined_subject_mask_storage = argv[++i];
            continue;
        }
        if (arg == "--show-subject-masks" || arg == "--show-eye-masks") {
            cli_show_eye_masks = true;
            continue;
        }
        if (arg == "--tail-kinematics-run") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for --tail-kinematics-run"
                          << std::endl;
                return 1;
            }
            cli_tail_kinematics_run = argv[++i];
            continue;
        }
        if (arg == "--eye-angle-run") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for --eye-angle-run"
                          << std::endl;
                return 1;
            }
            cli_eye_angle_run = argv[++i];
            continue;
        }
        if (arg == "--stimulus-run") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for --stimulus-run"
                          << std::endl;
                return 1;
            }
            cli_stimulus_run = argv[++i];
            continue;
        }
        if (arg == "--perf-log") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for --perf-log" << std::endl;
                return 1;
            }
            cli_perf_log_path = argv[++i];
            continue;
        }
        if (arg == "--mask-perf-log") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for --mask-perf-log" << std::endl;
                return 1;
            }
            cli_mask_perf_log_path = argv[++i];
            continue;
        }
        if (arg == "--playback-trace-log") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for --playback-trace-log"
                          << std::endl;
                return 1;
            }
            cli_playback_trace_log_path = argv[++i];
            continue;
        }
        if (arg == "--frame-sync-trace-log") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for --frame-sync-trace-log"
                          << std::endl;
                return 1;
            }
            cli_frame_sync_trace_log_path = argv[++i];
            continue;
        }
        if (arg == "--clipped-boundary-smoke") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for --clipped-boundary-smoke"
                          << std::endl;
                return 1;
            }
            int start_frame = -1;
            int end_frame = -1;
            if (!parseFrameRangeArgument(argv[++i], start_frame, end_frame)) {
                std::cerr << "Invalid --clipped-boundary-smoke value; "
                             "expected START:END with END >= START"
                          << std::endl;
                return 1;
            }
            clipped_boundary_smoke.enabled = true;
            clipped_boundary_smoke.start_frame = start_frame;
            clipped_boundary_smoke.end_frame = end_frame;
            continue;
        }
        if (arg == "--playback-smoke") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for --playback-smoke"
                          << std::endl;
                return 1;
            }
            int start_frame = -1;
            int end_frame = -1;
            if (!parseFrameRangeArgument(argv[++i], start_frame, end_frame)) {
                std::cerr << "Invalid --playback-smoke value; expected "
                             "START:END with END >= START"
                          << std::endl;
                return 1;
            }
            playback_smoke.enabled = true;
            playback_smoke.start_frame = start_frame;
            playback_smoke.end_frame = end_frame;
            continue;
        }
        if (arg == "--playback-smoke-timeout") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for --playback-smoke-timeout"
                          << std::endl;
                return 1;
            }
            double parsed = 0.0;
            if (!parseDoubleArgument(argv[++i], parsed) || parsed <= 0.0) {
                std::cerr << "Invalid --playback-smoke-timeout value; "
                             "expected a positive number of seconds"
                          << std::endl;
                return 1;
            }
            playback_smoke.timeout_s = parsed;
            continue;
        }
        if (arg == "--no-mask-perf-log") {
            mask_perf_log_enabled = false;
            continue;
        }
        if (arg == "--mask-perf-sample-every") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for --mask-perf-sample-every"
                          << std::endl;
                return 1;
            }
            int parsed = 0;
            if (!parseIntArgument(argv[++i], parsed) || parsed < 1) {
                std::cerr << "Invalid --mask-perf-sample-every value; "
                             "expected an integer >= 1"
                          << std::endl;
                return 1;
            }
            cli_mask_perf_sample_every = parsed;
            continue;
        }
        if (arg == "--swap-interval") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for --swap-interval" << std::endl;
                return 1;
            }
            int parsed = 0;
            if (!parseIntArgument(argv[++i], parsed) ||
                (parsed != 0 && parsed != 1)) {
                std::cerr << "Invalid --swap-interval value; expected 0 or 1"
                          << std::endl;
                return 1;
            }
            cli_swap_interval = parsed;
            continue;
        }
        if (arg == "--frame-cap-fps") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for --frame-cap-fps" << std::endl;
                return 1;
            }
            double parsed = 0.0;
            if (!parseDoubleArgument(argv[++i], parsed) || parsed < 0.0) {
                std::cerr << "Invalid --frame-cap-fps value; expected a "
                             "non-negative number"
                          << std::endl;
                return 1;
            }
            cli_frame_cap_fps = parsed;
            continue;
        }
        std::cerr << "Ignoring unknown argument: " << arg << std::endl;
    }

    if (cli_swap_interval == 0 && cli_frame_cap_fps <= 0.0) {
        cli_frame_cap_fps = 60.0;
        std::cerr << "[FramePacing] --swap-interval 0 requested without "
                     "--frame-cap-fps; capping at 60 FPS to avoid an "
                     "uncapped render loop."
                  << std::endl;
    }

    if (playback_smoke.enabled && clipped_boundary_smoke.enabled) {
        std::cerr << "--playback-smoke and --clipped-boundary-smoke cannot be "
                     "used in the same run"
                  << std::endl;
        return 1;
    }

    // Mutual exclusion: --recording takes precedence over --zarr
    if (!cli_recording_path.empty() && !cli_zarr_override_path.empty()) {
        std::cerr << "Warning: both --recording and --zarr specified; "
                  << "using --recording, ignoring --zarr" << std::endl;
        cli_zarr_override_path.clear();
    }

    // Validate --recording path early
    if (!cli_recording_path.empty() && !IsDirectoryNoThrow(cli_recording_path)) {
        std::cerr << "Error: --recording path is not a directory: "
                  << cli_recording_path << std::endl;
        cli_recording_path.clear();
    }

    gx_context *window = new gx_context();
    *window = gx_context{};
    window->swap_interval = cli_swap_interval;
    window->width = 1920;
    window->height = 1080;
    window->render_target_title = (char *)malloc(100);  // window title
    window->glsl_version = (char *)malloc(100);

    const int kCudaDeviceIndex = ResolveCudaDeviceIndex();
    render_initialize_target(window, kCudaDeviceIndex, argv0_path);

    render_scene *scene = new render_scene();

    std::string root_dir;
    std::string skeleton_dir;
    std::vector<std::string> camera_names;
    std::vector<CameraParams> camera_params;
    std::vector<std::thread> decoder_threads;
    std::vector<std::unique_ptr<FFmpegDemuxer>> demuxers;

    // Zarr loading
    ZarrDetectionLoader zarr_loader;
    if (!cli_subject_shape_run.empty()) {
        zarr_loader.setRequestedSubjectShapeRunName(cli_subject_shape_run);
    }
    if (!cli_refined_subject_mask_run.empty()) {
        zarr_loader.setRequestedRefinedSubjectMaskRunName(
            cli_refined_subject_mask_run);
    }
    if (!cli_refined_subject_mask_storage.empty()) {
        zarr_loader.setRequestedRefinedSubjectMaskStorage(
            cli_refined_subject_mask_storage);
    }
    if (!cli_tail_kinematics_run.empty()) {
        zarr_loader.setRequestedTailKinematicsRunName(
            cli_tail_kinematics_run);
    }
    if (!cli_eye_angle_run.empty()) {
        zarr_loader.setRequestedEyeAngleRunName(cli_eye_angle_run);
    }
    if (!cli_stimulus_run.empty()) {
        zarr_loader.setRequestedStimulusRunName(cli_stimulus_run);
    }
    bool zarr_loaded = false;

    DecoderContext *dc_context = new DecoderContext();
    *dc_context = DecoderContext{};
    dc_context->decoding_flag = false;
    dc_context->stop_flag = false;
    dc_context->total_num_frame = int(INT_MAX);
    dc_context->estimated_num_frames = 0;
    dc_context->gpu_index = kCudaDeviceIndex;
    dc_context->seek_interval = 250;

    // gui states, todo: bundle this later
    bool video_loaded = false;
    bool cpu_buffer_toggle = true;
    bool show_keypoint_markers = true;
    bool show_heading_arrows = true;
    bool show_eye_masks = cli_show_eye_masks;
    bool show_subject_body_mask = true;
    bool show_eye_left_mask = true;
    bool show_eye_right_mask = true;
    bool show_swim_bladder_mask = true;
    bool show_eye_direction_beams = true;
    bool show_eye_gaze_rays = true;
    bool show_eye_angle_arcs = true;
    bool show_eye_angle_labels = true;
    bool show_movement_trail = true;
    float movement_trail_seconds = 2.0f;
    bool movement_trail_valid_samples_only = true;
    CameraViewStimulusInsetOptions stimulus_inset_options;
    bool show_stimulus_debug_windows = false;
    CameraViewMaskOverlayMode mask_overlay_mode =
        CameraViewMaskOverlayMode::Review;
    CameraViewSubjectShapeOverlayOptions subject_shape_overlay_options;
    CameraViewTailKinematicsOverlayOptions tail_kinematics_overlay_options;
    int current_frame_num = 0;
    std::vector<std::string> imgs_names;

    constexpr bool kHeadingDebugLoggingEnabled = false;
    constexpr int kHeadingDebugMaxMessages = 400;
    constexpr bool kPlaybackDebugLoggingEnabled = false;
    int heading_debug_message_count = 0;
    int heading_debug_draw_log_count = 0;
    int heading_debug_entry_log_count = 0;
    int heading_debug_last_frame_logged = -1;
    bool heading_debug_logged_toggle_disabled = false;
    bool heading_debug_logged_no_data = false;
    bool heading_debug_logged_interpolated = false;
    auto headingDebugLog = [&](const std::string &message) {
        if (!kHeadingDebugLoggingEnabled) {
            return;
        }
        if (heading_debug_message_count >= kHeadingDebugMaxMessages) {
            if (heading_debug_message_count == kHeadingDebugMaxMessages) {
                std::cout << "[HEADING_DEBUG] Log limit reached, suppressing further messages"
                          << std::endl;
            }
            heading_debug_message_count++;
            return;
        }
        std::cout << "[HEADING_DEBUG] " << message << std::endl;
        heading_debug_message_count++;
    };

    constexpr bool kEyeMaskDebugLoggingEnabled = false;
    constexpr int kEyeMaskDebugMaxMessages = 100;
    int eye_mask_debug_message_count = 0;
    int eye_mask_debug_entry_log_count = 0;
    int eye_mask_debug_draw_log_count = 0;
    int eye_mask_debug_last_frame_logged = -1;
    bool eye_mask_debug_logged_toggle_disabled = false;
    bool eye_mask_debug_logged_no_data = false;
    auto eyeMaskDebugLog = [&](const std::string &message) {
        if (!kEyeMaskDebugLoggingEnabled) {
            return;
        }
        if (eye_mask_debug_message_count >= kEyeMaskDebugMaxMessages) {
            if (eye_mask_debug_message_count == kEyeMaskDebugMaxMessages) {
                std::cout << "[EYE_MASK_DEBUG] Log limit reached, suppressing further messages"
                          << std::endl;
            }
            eye_mask_debug_message_count++;
            return;
        }
        std::cout << "[EYE_MASK_DEBUG] " << message << std::endl;
        eye_mask_debug_message_count++;
    };

    // for labeling
    LegacyLabelingState legacy_labeling_state;

    // others
    UiPathConfig ui_path_config = LoadUiPathConfig(cwd, argv0_path);
    std::string start_folder_name = ui_path_config.default_start_path;
    if (start_folder_name.empty() || !IsDirectoryNoThrow(start_folder_name)) {
        start_folder_name = cwd.string();
    }
    const std::filesystem::path default_buffer_dump_root =
        GetDefaultCrimsonBufferDumpRoot();
    if (!ui_path_config.loaded_from.empty()) {
        std::cout << "[UIPathConfig] Loaded: " << ui_path_config.loaded_from
                  << std::endl;
    } else {
        std::cout << "[UIPathConfig] Using default start path: "
                  << start_folder_name << std::endl;
    }
    ImVec4 clear_color = ImVec4(0.0f, 0.0f, 0.0f, 1.00f);
    ImGuiIO &io = ImGui::GetIO();

    ImPlotStyle &style = ImPlot::GetStyle();
    ImVec4 *colors = style.Colors;
    colors[ImPlotCol_Crosshairs] = ImVec4(0.3f, 0.10f, 0.64f, 1.00f);

    bool yolo_detection = false;
    std::vector<std::thread> yolo_threads;
    yolo_param yolo_setting = yolo_param();
    int label_buffer_size = 100;
    int playback_preview_scale_mode = 0;
    int playback_renderer_mode = 1;
    int stimulus_buffer_size = 12;
    bool stimulus_use_cpu_buffer = false;
#ifdef _WIN32
    bool stimulus_use_software_decode = true;
#else
    bool stimulus_use_software_decode = false;
#endif
    uint64_t stimulus_catchup_seek_generation = 1;
    StimulusPlaybackPresentationState stimulus_playback_presentation_state;
    bool show_help_window = false;
    std::vector<bool> is_view_focused;
    bool input_is_imgs = false;
    bool show_error = false;
    std::string error_message;
    std::unordered_map<std::string, bool> window_was_decoding;
    double inst_speed = 1.0;
    double video_fps = 60.0f;
    float set_playback_speed = 1.0f;
    PlaybackState ps;
    SeekProgress seek_progress;
    std::string frame_sync_debug_line;
    int frame_sync_valid_slots = -1;
    int frame_sync_empty_slots = -1;
    int frame_sync_latest_decoded = -1;
    int frame_sync_recording_remaining = -1;
    int frame_sync_recording_total = -1;
    PerfLogWriter perf_log_writer;
    MaskPerfLogWriter mask_perf_log_writer;
    PlaybackTraceLogWriter playback_trace_log_writer;
    PlaybackTraceLogWriter frame_sync_trace_log_writer;
    PlaybackTraceLogWriter clipped_frame_trace_log_writer;
    ClippedFrameTraceStats clipped_frame_trace_stats;
    ClippedTextureDumpConfig clipped_texture_dump;
    const bool clipped_rebase_before_play =
        crimson_env_flag_enabled("CRIMSON_CLIPPED_REBASE_BEFORE_PLAY");
    constexpr auto kPerfLogSamplePeriod = std::chrono::milliseconds(250);
    constexpr uint64_t kPlaybackWarmupPerfFrames = 120;
    constexpr uint64_t kPlaybackWarmupPerfSampleStride = 2;
    if (!cli_perf_log_path.empty()) {
        (void)perf_log_writer.open(cli_perf_log_path);
    }
    if (mask_perf_log_enabled) {
        const std::filesystem::path mask_perf_log_path =
            cli_mask_perf_log_path.empty()
                ? defaultMaskPerfLogPath(default_buffer_dump_root)
                : cli_mask_perf_log_path;
        (void)mask_perf_log_writer.open(mask_perf_log_path);
        if (cli_mask_perf_sample_every > 1) {
            std::cout << "[MaskPerfLog] Sampling every "
                      << cli_mask_perf_sample_every << " frames" << std::endl;
        }
    }
    if (!cli_playback_trace_log_path.empty()) {
        (void)playback_trace_log_writer.open(cli_playback_trace_log_path);
    }
    const char* frame_sync_trace_path_env =
        std::getenv("CRIMSON_FRAME_SYNC_TRACE_PATH");
    const bool frame_sync_trace_enabled =
        crimson_env_flag_enabled("CRIMSON_FRAME_SYNC_TRACE") ||
        !cli_frame_sync_trace_log_path.empty() ||
        (frame_sync_trace_path_env != nullptr &&
         frame_sync_trace_path_env[0] != '\0');
    if (frame_sync_trace_enabled) {
        std::filesystem::path frame_sync_trace_path =
            default_buffer_dump_root / "frame_sync_trace_latest.jsonl";
        if (frame_sync_trace_path_env != nullptr &&
            frame_sync_trace_path_env[0] != '\0') {
            frame_sync_trace_path = frame_sync_trace_path_env;
        }
        if (!cli_frame_sync_trace_log_path.empty()) {
            frame_sync_trace_path = cli_frame_sync_trace_log_path;
        }
        (void)frame_sync_trace_log_writer.open(frame_sync_trace_path,
                                               "FrameSyncTrace");
    }
    const char* clipped_texture_dump_frame_env =
        std::getenv("CRIMSON_CLIPPED_TEXTURE_DUMP_FRAME");
    if (clipped_texture_dump_frame_env != nullptr &&
        clipped_texture_dump_frame_env[0] != '\0') {
        int dump_frame = -1;
        if (!parseIntArgument(clipped_texture_dump_frame_env, dump_frame) ||
            dump_frame < 0) {
            std::cerr << "[ClippedTextureDump] Invalid "
                      << "CRIMSON_CLIPPED_TEXTURE_DUMP_FRAME='"
                      << clipped_texture_dump_frame_env << "'" << std::endl;
        } else {
            clipped_texture_dump.enabled = true;
            clipped_texture_dump.parent_frame = dump_frame;
            const char* dump_path_env =
                std::getenv("CRIMSON_CLIPPED_TEXTURE_DUMP_PATH");
            if (dump_path_env != nullptr && dump_path_env[0] != '\0') {
                clipped_texture_dump.output_path = dump_path_env;
            } else {
                clipped_texture_dump.output_path =
                    default_buffer_dump_root /
                    ("clipped_bound_texture_parent_" +
                     std::to_string(dump_frame) + ".png");
            }
            if (clipped_texture_dump.output_path.extension().empty()) {
                clipped_texture_dump.output_path.replace_extension(".png");
            }
            std::cout << "[ClippedTextureDump] Will dump parent frame "
                      << clipped_texture_dump.parent_frame << " to "
                      << clipped_texture_dump.output_path << " and "
                      << pathWithStemSuffix(
                             clipped_texture_dump.output_path, "_flip_y")
                      << std::endl;
        }
    }
    const char* clipped_frame_trace_path_env =
        std::getenv("CRIMSON_CLIPPED_FRAME_TRACE_PATH");
    const bool clipped_frame_trace_enabled =
        crimson_env_flag_enabled("CRIMSON_CLIPPED_FRAME_TRACE") ||
        clipped_texture_dump.enabled ||
        (clipped_frame_trace_path_env != nullptr &&
         clipped_frame_trace_path_env[0] != '\0');
    if (clipped_frame_trace_enabled) {
        std::filesystem::path clipped_frame_trace_path =
            default_buffer_dump_root / "clipped_frame_trace_latest.jsonl";
        if (clipped_frame_trace_path_env != nullptr &&
            clipped_frame_trace_path_env[0] != '\0') {
            clipped_frame_trace_path = clipped_frame_trace_path_env;
        }
        (void)clipped_frame_trace_log_writer.open(
            clipped_frame_trace_path, "ClippedFrameTrace");
    }
    std::unordered_map<std::string, FrameSyncTraceLastState>
        frame_sync_trace_last_by_camera;
    uint64_t mask_perf_sample_index = 0;
    int perf_playback_start_frame = -1;
    uint64_t perf_frames_since_playback_start = 0;
    std::string perf_playback_resume_path = resumePathName(ResumePath::None);
    int perf_playback_resume_target_frame = -1;

    window_need_decoding[stimulus_player.window_name].store(false);
    latest_decoded_frame[stimulus_player.window_name].store(-1);
    window_was_decoding[stimulus_player.window_name] = false;
    PaletteClippedMediaState clipped_media_state;
    MediaSessionLoader media_session_loader(
        MediaSessionLoaderContext{
            scene,
            dc_context,
            &zarr_loader,
            &stimulus_player,
            &ps,
            &root_dir,
            &skeleton_dir,
            &camera_names,
            &camera_params,
            &decoder_threads,
            &demuxers,
            &is_view_focused,
            &window_need_decoding,
            &window_was_decoding,
            &clipped_media_state,
            &video_loaded,
            &zarr_loaded,
            &input_is_imgs,
            &show_error,
            &error_message,
            &label_buffer_size,
            &stimulus_buffer_size,
            &stimulus_use_cpu_buffer,
            &stimulus_use_software_decode,
            &video_fps,
            kCudaDeviceIndex,
        });

    auto warmEyeMaskCacheForFrame = [&](const char* reason, int frame) {
        if (!zarr_loaded || !zarr_loader.hasEyeMasks() || frame < 0) {
            return;
        }
        const auto warm_start = std::chrono::steady_clock::now();
        const bool warmed =
            zarr_loader.warmEyeMaskCacheForFrame(static_cast<size_t>(frame));
        if (!warmed) {
            return;
        }
        std::cout << "[SUBJECT_MASK_PREWARM] reason="
                  << (reason != nullptr ? reason : "unknown")
                  << " frame=" << frame
                  << " total_ms="
                  << durationMs(std::chrono::steady_clock::now() -
                                warm_start)
                  << std::endl;
    };

    media_session_loader.bootstrapFromCli(
        cli_zarr_override_path,
        cli_recording_path,
        [&]() { refreshDetectionDatasetOptions(zarr_loader); },
        [&]() { g_zarr_bbox_edit_state.clearAll(); });
    warmEyeMaskCacheForFrame("cli_bootstrap", current_frame_num);

    ReviewFrameFilters review_frame_filters;
    ReviewFrameCache review_frame_cache;
    std::string review_frame_status;
    std::string decode_debug_status;
    std::string bbox_payload_status;
    std::mt19937 debug_rng(
        static_cast<uint32_t>(
            std::chrono::high_resolution_clock::now().time_since_epoch().count()));

    std::optional<ManualDetectPayloadPreview> manual_payload_preview;
    CropPreviewWindowState crop_preview_window_state;
    LabelingToolWindowState labeling_tool_window_state;
    FrameDebugWindowState frame_debug_window_state;
    PendingKeypointWriteState pending_keypoint_write;
    PlaybackSessionController playback_session_controller(
        PlaybackSessionControllerContext{
            scene,
            dc_context,
            &zarr_loader,
            &stimulus_player,
            &ps,
            &seek_progress,
            &current_frame_num,
            &video_fps,
            &camera_names,
            &window_was_decoding,
            &window_need_decoding,
            [&](int parent_frame) {
                return media_session_loader.loadClippedVideoForParentFrame(
                    parent_frame);
            },
        });
    auto resetPlaybackStartPerf = [&]() {
        perf_playback_start_frame = -1;
        perf_frames_since_playback_start = 0;
        perf_playback_resume_path = resumePathName(ResumePath::None);
        perf_playback_resume_target_frame = -1;
    };
    auto markPlaybackStartForPerf = [&]() {
        perf_playback_start_frame =
            std::max(0, ps.to_display_frame_number);
        perf_frames_since_playback_start = 0;
        perf_playback_resume_path = resumePathName(ps.last_resume_path);
        perf_playback_resume_target_frame = ps.last_resume_target_frame;
    };
    auto buildCurrentMaskOverlayOptions = [&]() {
        CameraViewMaskOverlayOptions options;
        options.show_subject_body = show_subject_body_mask;
        options.show_eye_left = show_eye_left_mask;
        options.show_eye_right = show_eye_right_mask;
        options.show_swim_bladder = show_swim_bladder_mask;
        options.show_eye_direction_beams = show_eye_direction_beams;
        options.show_eye_gaze_rays = show_eye_gaze_rays;
        options.show_eye_angle_arcs = show_eye_angle_arcs;
        options.show_eye_angle_labels = show_eye_angle_labels;
        options.mode = mask_overlay_mode;
        if (frame_debug_window_state.subject_mask_edit_session.active()) {
            const auto& target =
                frame_debug_window_state.subject_mask_edit_session.target();
            options.highlighted_roi_index = target.roi_index;
            options.highlighted_component_name = target.component_name;
        }
        return options;
    };
    auto prewarmEyeMaskOverlayTexturesForPlayback =
        [&](const char* reason, int start_frame) {
            if (!zarr_loaded || !zarr_loader.hasEyeMasks() || start_frame < 0) {
                return;
            }
            const bool prewarm_full_overlay = show_eye_masks;
            const bool prewarm_inset =
                frame_debug_window_state
                    .subject_mask_active_roi_inset_options.show_inset;
            if (!prewarm_full_overlay && !prewarm_inset) {
                return;
            }
            const auto prewarm_start = std::chrono::steady_clock::now();
            const CameraViewMaskOverlayOptions mask_options =
                buildCurrentMaskOverlayOptions();
            const std::string smoothing_key =
                zarr_loader.getEyeMaskSourcePath() + "|" +
                zarr_loader.getEyeAngleRunName();
            CameraViewMaskPerfMetrics aggregate;
            constexpr int kTexturePrewarmLookaheadFrames = 4;
            const int max_frame =
                zarr_loader.getTotalFrames() > 0
                    ? static_cast<int>(std::min<size_t>(
                          zarr_loader.getTotalFrames() - 1,
                          static_cast<size_t>(
                              std::numeric_limits<int>::max())))
                    : start_frame;
            int frames_checked = 0;
            int frames_with_masks = 0;
            for (int frame = start_frame;
                 frame <= std::min(max_frame,
                                    start_frame +
                                        kTexturePrewarmLookaheadFrames);
                 ++frame) {
                ++frames_checked;
                (void)zarr_loader.warmEyeMaskCacheForFrame(
                    static_cast<size_t>(frame));
                auto mask_details = zarr_loader.getRawDetections(
                    static_cast<size_t>(frame),
                    /*use_interpolated=*/false,
                    /*include_eye_masks=*/true,
                    /*include_subject_shapes=*/false,
                    /*suppress_subject_mask_smoke_log=*/true);
                if (!mask_details.includes_eye_masks ||
                    mask_details.eye_masks.empty()) {
                    continue;
                }
                ++frames_with_masks;
                auto metrics = prewarmCameraViewEyeMaskOverlayTextures(
                    mask_details,
                    smoothing_key,
                    mask_options,
                    prewarm_full_overlay,
                    prewarm_inset
                        ? &frame_debug_window_state
                               .subject_mask_active_roi_inset_options
                        : nullptr,
                    nullptr);
                accumulateCameraViewMaskPerfMetrics(aggregate, metrics);
            }
            if (frames_with_masks == 0) {
                return;
            }
            std::cout << "[SUBJECT_MASK_TEXTURE_PREWARM] reason="
                      << (reason != nullptr ? reason : "unknown")
                      << " start_frame=" << start_frame
                      << " frames_checked=" << frames_checked
                      << " frames_with_masks=" << frames_with_masks
                      << " uploads=" << aggregate.texture_uploads
                      << " cache_hits=" << aggregate.texture_cache_hits
                      << " cache_misses=" << aggregate.texture_cache_misses
                      << " texture_upload_ms=" << aggregate.texture_upload_ms
                      << " texture_lookup_ms=" << aggregate.texture_lookup_ms
                      << " total_ms="
                      << durationMs(std::chrono::steady_clock::now() -
                                    prewarm_start)
                      << std::endl;
        };
    prewarmEyeMaskOverlayTexturesForPlayback("cli_bootstrap",
                                             current_frame_num);
    auto clippedPlaybackEventStateJson = [&]() -> json {
        auto nullableInt = [](int value) -> json {
            return value >= 0 ? json(value) : json(nullptr);
        };
        json buffer = nullptr;
        const int visible_idx =
            playback_session_controller.getVisibleCameraIndex();
        if (scene != nullptr && visible_idx >= 0 &&
            visible_idx < scene->num_cams && scene->size_of_buffer > 0) {
            const auto& camera = scene->cameras[visible_idx];
            const int normalized_read_head =
                ps.read_head >= 0
                    ? ps.read_head % static_cast<int>(scene->size_of_buffer)
                    : -1;
            int valid_slots = 0;
            int oldest_frame = std::numeric_limits<int>::max();
            int newest_frame = -1;
            int read_head_frame = -1;
            int selected_slot = -1;
            int front_slot = -1;
            int contiguous_span_start = -1;
            int contiguous_span_end = -1;
            std::vector<int> valid_frames;
            valid_frames.reserve(scene->size_of_buffer);
            for (int slot_idx = 0;
                 slot_idx < static_cast<int>(scene->size_of_buffer);
                 ++slot_idx) {
                const auto& slot = camera.display_buffer[slot_idx];
                if (slot.available_to_write || slot.frame_number < 0) {
                    continue;
                }
                valid_slots++;
                valid_frames.push_back(slot.frame_number);
                oldest_frame = std::min(oldest_frame, slot.frame_number);
                newest_frame = std::max(newest_frame, slot.frame_number);
                if (slot_idx == normalized_read_head) {
                    read_head_frame = slot.frame_number;
                }
                if (slot.frame_number == ps.to_display_frame_number) {
                    selected_slot = slot_idx;
                }
                if (slot.frame_number == camera.last_uploaded_frame) {
                    front_slot = slot_idx;
                }
            }
            if (!valid_frames.empty()) {
                std::sort(valid_frames.begin(), valid_frames.end());
                contiguous_span_end = valid_frames.back();
                contiguous_span_start = contiguous_span_end;
                for (int idx = static_cast<int>(valid_frames.size()) - 2;
                     idx >= 0; --idx) {
                    if (valid_frames[idx] + 1 == contiguous_span_start) {
                        contiguous_span_start = valid_frames[idx];
                        continue;
                    }
                    break;
                }
            }
            buffer = {
                {"visible_idx", visible_idx},
                {"read_head", ps.read_head},
                {"normalized_read_head", normalized_read_head},
                {"read_head_frame", nullableInt(read_head_frame)},
                {"selected_slot", selected_slot},
                {"front_slot", front_slot},
                {"valid_slots", valid_slots},
                {"buffer_size", static_cast<int>(scene->size_of_buffer)},
                {"oldest_frame",
                 valid_slots > 0 ? json(oldest_frame) : json(nullptr)},
                {"newest_frame",
                 valid_slots > 0 ? json(newest_frame) : json(nullptr)},
                {"newest_contiguous_span_start",
                 nullableInt(contiguous_span_start)},
                {"newest_contiguous_span_end",
                 nullableInt(contiguous_span_end)},
                {"front_parent_frame",
                 camera.texture_has_valid_frame
                     ? json(camera.last_uploaded_frame)
                     : json(nullptr)},
                {"front_local_frame",
                 camera.texture_has_valid_frame
                     ? json(camera.last_uploaded_local_frame)
                     : json(nullptr)},
                {"staging_valid", camera.playback_staging_valid},
                {"staging_parent_frame",
                 camera.playback_staging_valid
                     ? json(camera.playback_staging_frame)
                     : json(nullptr)},
                {"staging_local_frame",
                 camera.playback_staging_valid
                     ? json(camera.playback_staging_local_frame)
                     : json(nullptr)},
            };
        }
        return json{
            {"current_frame_num", current_frame_num},
            {"video_fps", video_fps},
            {"playback",
             {{"play_video", ps.play_video},
              {"to_display_frame_number", ps.to_display_frame_number},
              {"slider_frame_number", ps.slider_frame_number},
              {"pause_selected", ps.pause_selected},
              {"pause_seeked", ps.pause_seeked},
              {"just_seeked", ps.just_seeked},
              {"slider_just_changed", ps.slider_just_changed},
              {"buffer_browsed_since_pause", ps.buffer_browsed_since_pause},
              {"paused_frame_on_toggle", nullableInt(ps.paused_frame_on_toggle)},
              {"last_resume_path", resumePathName(ps.last_resume_path)},
              {"last_resume_target_frame",
               nullableInt(ps.last_resume_target_frame)},
              {"accumulated_play_time", ps.accumulated_play_time}}},
            {"seek_progress",
             {{"state", seekStateName(seek_progress.state)},
              {"seek_id", seek_progress.seek_id},
              {"requested_camera_frame",
               nullableInt(seek_progress.requested_camera_frame)},
              {"target_camera_frame",
               nullableInt(seek_progress.target_camera_frame)},
              {"target_stimulus_frame",
               nullableInt(seek_progress.target_stimulus_frame)},
              {"accurate", seek_progress.accurate},
              {"skip_stimulus_hard_seek",
               seek_progress.skip_stimulus_hard_seek}}},
            {"buffer", buffer},
        };
    };
    auto writeClippedPlaybackStateEvent =
        [&](const std::string& event_name,
            const json& details,
            bool force_flush) {
            if (!clipped_frame_trace_log_writer.enabled() || !zarr_loaded ||
                !zarr_loader.hasClippedCollection()) {
                return;
            }
            clipped_frame_trace_log_writer.write(
                json{{"event", "clipped_playback_state"},
                     {"playback_event", event_name},
                     {"details", details},
                     {"state", clippedPlaybackEventStateJson()}},
                force_flush);
        };
    auto clippedPlaybackRebaseTargetBeforePlay = [&]() {
        json target = {
            {"target_frame", std::max(0, ps.to_display_frame_number)},
            {"source", "selected_parent_frame"},
            {"visible_idx", nullptr},
            {"front_parent_frame", nullptr},
            {"front_local_frame", nullptr},
            {"staging_parent_frame", nullptr},
            {"read_head_frame", nullptr},
        };
        int target_frame = std::max(0, ps.to_display_frame_number);
        std::string source = "selected_parent_frame";
        const int visible_idx = playback_session_controller.getVisibleCameraIndex();
        target["visible_idx"] =
            visible_idx >= 0 ? json(visible_idx) : json(nullptr);
        if (scene != nullptr && visible_idx >= 0 &&
            visible_idx < scene->num_cams && scene->size_of_buffer > 0) {
            const auto& camera = scene->cameras[visible_idx];
            target["front_parent_frame"] =
                camera.texture_has_valid_frame && camera.last_uploaded_frame >= 0
                    ? json(camera.last_uploaded_frame)
                    : json(nullptr);
            target["front_local_frame"] =
                camera.texture_has_valid_frame &&
                        camera.last_uploaded_local_frame >= 0
                    ? json(camera.last_uploaded_local_frame)
                    : json(nullptr);
            target["staging_parent_frame"] =
                camera.playback_staging_valid &&
                        camera.playback_staging_frame >= 0
                    ? json(camera.playback_staging_frame)
                    : json(nullptr);
            const int read_head_slot =
                ps.read_head >= 0
                    ? ps.read_head % static_cast<int>(scene->size_of_buffer)
                    : -1;
            if (read_head_slot >= 0) {
                const auto& slot = camera.display_buffer[read_head_slot];
                target["read_head_frame"] =
                    !slot.available_to_write && slot.frame_number >= 0
                        ? json(slot.frame_number)
                        : json(nullptr);
            }

            const bool explicit_paused_selection =
                ps.pause_seeked || ps.buffer_browsed_since_pause ||
                ps.slider_just_changed;
            if (!explicit_paused_selection &&
                camera.texture_has_valid_frame &&
                camera.last_uploaded_frame >= 0) {
                target_frame = camera.last_uploaded_frame;
                source = "front_texture_parent_frame";
            }
        }
        target["target_frame"] = target_frame;
        target["source"] = source;
        return target;
    };
    auto applyPlaybackToggleForPerf = [&]() {
        const bool was_playing = ps.play_video;
        if (!was_playing && clipped_rebase_before_play && zarr_loaded &&
            zarr_loader.hasClippedCollection()) {
            const json rebase_target = clippedPlaybackRebaseTargetBeforePlay();
            const int target_frame =
                rebase_target.value("target_frame",
                                    std::max(0, ps.to_display_frame_number));
            if (clipped_frame_trace_log_writer.enabled()) {
                clipped_frame_trace_log_writer.write(
                    json{{"event", "clipped_rebase_before_play"},
                         {"phase", "before_seek"},
                         {"target", rebase_target},
                         {"state", clippedPlaybackEventStateJson()}},
                    /*force_flush=*/true);
            }
            playback_session_controller.seekToFrame(
                target_frame,
                /*prefer_buffer_when_paused=*/false,
                /*force_inaccurate=*/true,
                /*skip_stimulus_hard_seek=*/true);
            if (clipped_frame_trace_log_writer.enabled()) {
                clipped_frame_trace_log_writer.write(
                    json{{"event", "clipped_rebase_before_play"},
                         {"phase", "after_seek"},
                         {"target", rebase_target},
                         {"state", clippedPlaybackEventStateJson()},
                         {"playback",
                          {{"to_display_frame_number",
                            ps.to_display_frame_number},
                           {"slider_frame_number", ps.slider_frame_number},
                           {"read_head", ps.read_head},
                           {"just_seeked", ps.just_seeked},
                           {"pause_seeked", ps.pause_seeked}}}},
                    /*force_flush=*/true);
            }
        }
        writeClippedPlaybackStateEvent(
            "toggle_playback",
            json{{"phase", "before"}, {"was_playing", was_playing}},
            true);
        if (!was_playing) {
            prewarmEyeMaskOverlayTexturesForPlayback(
                "playback_start",
                std::max(0, ps.to_display_frame_number));
        }
        playback_session_controller.applyPlaybackToggle();
        writeClippedPlaybackStateEvent(
            "toggle_playback",
            json{{"phase", "after"},
                 {"was_playing", was_playing},
                 {"is_playing", ps.play_video}},
            true);
        if (!was_playing && ps.play_video) {
            markPlaybackStartForPerf();
        } else if (was_playing && !ps.play_video) {
            resetPlaybackStartPerf();
        }
    };
    auto writePlaybackTraceEvent =
        [&](const std::string& event_name,
            const json& details,
            int presenter_view_idx = -1,
            int presenter_target_frame = -1,
            int presenter_preferred_paused_slot = -1,
            int presenter_presented_slot = -1,
            int presenter_presented_frame = -1,
            int presenter_resolved_frame = -1,
            bool presenter_prewarm_active = false) {
            if (!playback_trace_log_writer.enabled()) {
                return;
            }

            int visible_idx = presenter_view_idx;
            if (visible_idx < 0) {
                visible_idx = playback_session_controller.getVisibleCameraIndex();
            }
            const int target_frame = std::max(
                0, presenter_target_frame >= 0 ? presenter_target_frame
                                                : ps.to_display_frame_number);
            json camera_buffer = json::object();
            if (video_loaded && scene != nullptr && visible_idx >= 0 &&
                visible_idx < scene->num_cams && scene->size_of_buffer > 0) {
                const auto& camera = scene->cameras[visible_idx];
                const int normalized_read_head =
                    ps.read_head >= 0
                        ? ps.read_head % static_cast<int>(scene->size_of_buffer)
                        : -1;
                int valid_slots = 0;
                int oldest_frame = std::numeric_limits<int>::max();
                int newest_frame = -1;
                int exact_target_slot = -1;
                int read_head_frame = -1;
                std::vector<int> sample_frames;
                sample_frames.reserve(std::min<int>(
                    static_cast<int>(scene->size_of_buffer), 12));
                for (int slot_idx = 0;
                     slot_idx < static_cast<int>(scene->size_of_buffer);
                     ++slot_idx) {
                    const auto& slot = camera.display_buffer[slot_idx];
                    if (slot.available_to_write || slot.frame_number < 0) {
                        continue;
                    }
                    valid_slots++;
                    oldest_frame = std::min(oldest_frame, slot.frame_number);
                    newest_frame = std::max(newest_frame, slot.frame_number);
                    if (slot.frame_number == target_frame) {
                        exact_target_slot = slot_idx;
                    }
                    if (slot_idx == normalized_read_head) {
                        read_head_frame = slot.frame_number;
                    }
                    if (sample_frames.size() < 12) {
                        sample_frames.push_back(slot.frame_number);
                    }
                }
                std::sort(sample_frames.begin(), sample_frames.end());
                auto latest_it = latest_decoded_frame.find(
                    visible_idx < static_cast<int>(camera_names.size())
                        ? camera_names[visible_idx]
                        : std::string{});
                const int latest_decoded =
                    latest_it != latest_decoded_frame.end()
                        ? latest_it->second.load()
                        : -1;
                camera_buffer = {
                    {"visible_idx", visible_idx},
                    {"camera_name",
                     visible_idx < static_cast<int>(camera_names.size())
                         ? json(camera_names[visible_idx])
                         : json(nullptr)},
                    {"valid_slots", valid_slots},
                    {"buffer_size", static_cast<int>(scene->size_of_buffer)},
                    {"oldest_frame",
                     valid_slots > 0 ? json(oldest_frame) : json(nullptr)},
                    {"newest_frame",
                     valid_slots > 0 ? json(newest_frame) : json(nullptr)},
                    {"sample_frames", sample_frames},
                    {"exact_target_slot", exact_target_slot},
                    {"contains_target", exact_target_slot >= 0},
                    {"read_head_frame", read_head_frame},
                    {"latest_decoded_frame", latest_decoded},
                    {"texture_has_valid_frame", camera.texture_has_valid_frame},
                    {"last_uploaded_frame", camera.last_uploaded_frame},
                    {"staging_valid", camera.playback_staging_valid},
                    {"staging_frame", camera.playback_staging_frame},
                };
            }

            int window_need_decoding_count = 0;
            for (const auto& entry : window_need_decoding) {
                if (entry.second.load()) {
                    window_need_decoding_count++;
                }
            }
            int stimulus_buffered_frames = 0;
            if (stimulus_player.display_buffer != nullptr &&
                stimulus_player.buffer_size > 0) {
                for (int i = 0; i < stimulus_player.buffer_size; ++i) {
                    auto metadata = frameSlotSnapshotReadable(
                        stimulus_player.display_buffer[i]);
                    if (metadata.has_value() && metadata->frame_number >= 0) {
                        stimulus_buffered_frames++;
                    }
                }
            }
            const auto stimulus_latest_it =
                latest_decoded_frame.find(stimulus_player.window_name);
            const int stimulus_latest_decoded =
                stimulus_latest_it != latest_decoded_frame.end()
                    ? stimulus_latest_it->second.load()
                    : -1;
            bool stimulus_seek_use = false;
            bool stimulus_seek_done = false;
            uint64_t stimulus_seek_id = 0;
            uint64_t stimulus_seek_frame = 0;
            uint64_t stimulus_settled_seek_id = 0;
            {
                std::lock_guard<std::mutex> lock(g_seek_info_mutex);
                stimulus_seek_use = stimulus_player.seek.use_seek;
                stimulus_seek_done = stimulus_player.seek.seek_done;
                stimulus_seek_id = stimulus_player.seek.seek_id;
                stimulus_seek_frame = stimulus_player.seek.seek_frame;
                stimulus_settled_seek_id =
                    stimulus_player.seek.settled_seek_id;
            }

            json sample = {
                {"event", event_name},
                {"details", details},
                {"video_loaded", video_loaded},
                {"video_fps", video_fps},
                {"current_frame_num", current_frame_num},
                {"window_need_decoding_count", window_need_decoding_count},
                {"playback",
                 {{"play_video", ps.play_video},
                  {"to_display_frame_number", ps.to_display_frame_number},
                  {"slider_frame_number", ps.slider_frame_number},
                  {"read_head", ps.read_head},
                  {"pause_seeked", ps.pause_seeked},
                  {"just_seeked", ps.just_seeked},
                  {"slider_just_changed", ps.slider_just_changed},
                  {"buffer_browsed_since_pause",
                   ps.buffer_browsed_since_pause},
                  {"paused_frame_on_toggle", ps.paused_frame_on_toggle},
                  {"current_stimulus_frame", ps.current_stimulus_frame}}},
                {"seek",
                 {{"state", seekStateName(seek_progress.state)},
                  {"seek_id", seek_progress.seek_id},
                  {"requested_camera_frame",
                   seek_progress.requested_camera_frame},
                  {"target_camera_frame", seek_progress.target_camera_frame},
                  {"target_stimulus_frame",
                   seek_progress.target_stimulus_frame},
                  {"accurate", seek_progress.accurate},
                  {"skip_stimulus_hard_seek",
                   seek_progress.skip_stimulus_hard_seek},
                  {"cameras_settled", seek_progress.cameras_settled},
                  {"cameras_total", seek_progress.cameras_total}}},
                {"presenter",
                 {{"view_idx", presenter_view_idx},
                  {"target_frame", presenter_target_frame},
                  {"preferred_paused_slot",
                   presenter_preferred_paused_slot},
                  {"presented_slot", presenter_presented_slot},
                  {"presented_frame", presenter_presented_frame},
                  {"resolved_frame", presenter_resolved_frame},
                  {"prewarm_active", presenter_prewarm_active}}},
                {"camera_buffer", camera_buffer},
                {"stimulus",
                 {{"loaded", stimulus_player.loaded},
                  {"window_name", stimulus_player.window_name},
                  {"current_stimulus_frame", ps.current_stimulus_frame},
                  {"latest_decoded_frame", stimulus_latest_decoded},
                  {"last_displayed_frame",
                   stimulus_player.last_displayed_frame},
                  {"buffered_frames", stimulus_buffered_frames},
                  {"buffer_size", stimulus_player.buffer_size},
                  {"seek_use", stimulus_seek_use},
                  {"seek_done", stimulus_seek_done},
                  {"seek_id", stimulus_seek_id},
                  {"seek_frame", stimulus_seek_frame},
                  {"settled_seek_id", stimulus_settled_seek_id}}},
            };
            playback_trace_log_writer.write(sample, event_name != "frame");
        };

    auto writeFrameSyncTraceEvent =
        [&](const json& details,
            int presenter_view_idx,
            int presenter_target_frame,
            int presenter_preferred_paused_slot,
            int presenter_presented_slot,
            int presenter_presented_frame,
            int presenter_resolved_frame,
            bool presenter_prewarm_active) {
            if (!frame_sync_trace_log_writer.enabled()) {
                return;
            }
            json sample = {
                {"event", "camera_frame_sync"},
                {"details", details},
                {"video_loaded", video_loaded},
                {"video_fps", video_fps},
                {"current_frame_num", current_frame_num},
                {"playback",
                 {{"play_video", ps.play_video},
                  {"to_display_frame_number", ps.to_display_frame_number},
                  {"slider_frame_number", ps.slider_frame_number},
                  {"read_head", ps.read_head},
                  {"pause_seeked", ps.pause_seeked},
                  {"just_seeked", ps.just_seeked},
                  {"slider_just_changed", ps.slider_just_changed}}},
                {"presenter",
                 {{"view_idx", presenter_view_idx},
                  {"target_frame", presenter_target_frame},
                  {"preferred_paused_slot",
                   presenter_preferred_paused_slot},
                  {"presented_slot", presenter_presented_slot},
                  {"presented_frame", presenter_presented_frame},
                  {"resolved_frame", presenter_resolved_frame},
                  {"prewarm_active", presenter_prewarm_active}}},
            };
            frame_sync_trace_log_writer.write(std::move(sample),
                                              /*force_flush=*/true);
        };

    auto clippedStateJson = [&]() -> json {
        if (!zarr_loaded || !zarr_loader.hasClippedCollection()) {
            return nullptr;
        }
        return json{
            {"current_video_path", clipped_media_state.current_video_path},
            {"clip_id", clipped_media_state.clip_id},
            {"camera_serial", clipped_media_state.camera_serial},
            {"selected_run_index", clipped_media_state.selected_run_index},
            {"first_parent_frame", clipped_media_state.first_parent_frame},
            {"last_parent_frame", clipped_media_state.last_parent_frame},
            {"pending_switch_parent_frame",
             clipped_media_state.pending_switch_parent_frame},
            {"switch_in_progress", clipped_media_state.switch_in_progress},
            {"last_presented_parent_frame",
             clipped_media_state.last_presented_parent_frame},
        };
    };

    auto writeClippedHandoffTraceEvent =
        [&](const std::string& event_name, const json& details) {
            if (!frame_sync_trace_log_writer.enabled()) {
                return;
            }
            json sample = {
                {"event", "clipped_handoff"},
                {"handoff_event", event_name},
                {"details", details},
                {"video_loaded", video_loaded},
                {"current_frame_num", current_frame_num},
                {"playback",
                 {{"play_video", ps.play_video},
                  {"to_display_frame_number", ps.to_display_frame_number},
                  {"read_head", ps.read_head}}},
                {"clipped_state", clippedStateJson()},
            };
            frame_sync_trace_log_writer.write(std::move(sample),
                                              /*force_flush=*/true);
        };

    auto clippedSelectedRunForFrame = [&](int parent_frame) -> size_t {
        if (!zarr_loaded || !zarr_loader.hasClippedCollection() ||
            parent_frame < 0) {
            return std::numeric_limits<size_t>::max();
        }
        const auto* row = zarr_loader.resolveClippedFrame(parent_frame);
        return row != nullptr ? row->selected_run_index
                              : std::numeric_limits<size_t>::max();
    };

    auto writeClippedFrameTraceSummary = [&](const char* reason) {
        if (!clipped_frame_trace_log_writer.enabled() ||
            clipped_frame_trace_stats.frames_traced == 0) {
            return;
        }
        clipped_frame_trace_log_writer.write(
            json{{"event", "clipped_frame_trace_summary"},
                 {"reason", reason != nullptr ? reason : "unknown"},
                 {"summary", clipped_frame_trace_stats.summaryJson()}},
            /*force_flush=*/true);
    };

    if (playback_smoke.enabled) {
        if (!video_loaded) {
            std::cerr << "[PlaybackSmoke] requested but no video is loaded"
                      << std::endl;
            return 2;
        }
        if (input_is_imgs) {
            std::cerr << "[PlaybackSmoke] image-sequence input is not "
                         "supported by this smoke"
                      << std::endl;
            return 2;
        }
        if (scene == nullptr || scene->num_cams <= 0 ||
            scene->size_of_buffer <= 0) {
            std::cerr << "[PlaybackSmoke] requested but camera buffers are not "
                         "initialized"
                      << std::endl;
            return 2;
        }
        if (zarr_loaded && zarr_loader.getTotalFrames() > 0) {
            const int max_frame =
                static_cast<int>(std::min<size_t>(
                    zarr_loader.getTotalFrames() - 1,
                    static_cast<size_t>(std::numeric_limits<int>::max())));
            if (playback_smoke.end_frame > max_frame) {
                std::cerr << "[PlaybackSmoke] end frame "
                          << playback_smoke.end_frame
                          << " is outside loaded recording max frame "
                          << max_frame << std::endl;
                return 2;
            }
        }
        playback_smoke.started = true;
        playback_smoke.start_time = std::chrono::steady_clock::now();
        playback_session_controller.seekToFrame(
            playback_smoke.start_frame,
            /*prefer_buffer_when_paused=*/false,
            /*force_inaccurate=*/true,
            /*skip_stimulus_hard_seek=*/true);
        prewarmEyeMaskOverlayTexturesForPlayback("playback_smoke",
                                                 playback_smoke.start_frame);
        if (!ps.play_video) {
            applyPlaybackToggleForPerf();
        }
        writePlaybackTraceEvent(
            "playback_smoke_started",
            {{"start_frame", playback_smoke.start_frame},
             {"end_frame", playback_smoke.end_frame},
             {"timeout_s", playback_smoke.timeout_s}},
            playback_session_controller.getVisibleCameraIndex());
        std::cout << "[PlaybackSmoke] started range="
                  << playback_smoke.start_frame << "-"
                  << playback_smoke.end_frame
                  << " timeout_s=" << playback_smoke.timeout_s << std::endl;
    }

    if (clipped_boundary_smoke.enabled) {
        if (!zarr_loaded || !zarr_loader.hasClippedCollection()) {
            std::cerr << "[ClippedBoundarySmoke] requested but the loaded "
                      << "archive is not a clipped collection" << std::endl;
            return 2;
        }
        const int max_frame =
            static_cast<int>(std::min<size_t>(
                zarr_loader.getTotalFrames() > 0
                    ? zarr_loader.getTotalFrames() - 1
                    : 0,
                static_cast<size_t>(std::numeric_limits<int>::max())));
        if (clipped_boundary_smoke.end_frame > max_frame) {
            std::cerr << "[ClippedBoundarySmoke] end frame "
                      << clipped_boundary_smoke.end_frame
                      << " is outside loaded clipped recording max frame "
                      << max_frame << std::endl;
            return 2;
        }
        const size_t start_run =
            clippedSelectedRunForFrame(clipped_boundary_smoke.start_frame);
        const size_t end_run =
            clippedSelectedRunForFrame(clipped_boundary_smoke.end_frame);
        if (start_run == std::numeric_limits<size_t>::max() ||
            end_run == std::numeric_limits<size_t>::max() ||
            start_run == end_run) {
            std::cerr << "[ClippedBoundarySmoke] range must cross a clipped "
                      << "selected-run boundary: "
                      << clipped_boundary_smoke.start_frame << ":"
                      << clipped_boundary_smoke.end_frame << std::endl;
            return 2;
        }
        clipped_boundary_smoke.started = true;
        clipped_boundary_smoke.start_time = std::chrono::steady_clock::now();
        playback_session_controller.seekToFrame(
            clipped_boundary_smoke.start_frame,
            /*prefer_buffer_when_paused=*/false,
            /*force_inaccurate=*/true,
            /*skip_stimulus_hard_seek=*/true);
        if (!ps.play_video) {
            applyPlaybackToggleForPerf();
        }
        writeClippedHandoffTraceEvent(
            "smoke_started",
            {{"start_frame", clipped_boundary_smoke.start_frame},
             {"end_frame", clipped_boundary_smoke.end_frame},
             {"start_selected_run_index", start_run},
             {"end_selected_run_index", end_run}});
        std::cout << "[ClippedBoundarySmoke] started range="
                  << clipped_boundary_smoke.start_frame << "-"
                  << clipped_boundary_smoke.end_frame << std::endl;
    }

    auto maybeRequestClippedBoundaryHandoff = [&](int presented_parent_frame) {
        if (!ps.play_video || !zarr_loaded ||
            !zarr_loader.hasClippedCollection() ||
            presented_parent_frame < 0) {
            return;
        }

        const size_t presented_run =
            clippedSelectedRunForFrame(presented_parent_frame);
        if (presented_run != std::numeric_limits<size_t>::max()) {
            clipped_media_state.last_presented_parent_frame =
                presented_parent_frame;
        }

        if (clipped_media_state.switch_in_progress &&
            clipped_media_state.pending_switch_parent_frame >= 0 &&
            presented_parent_frame >=
                clipped_media_state.pending_switch_parent_frame &&
            presented_run == clipped_media_state.selected_run_index) {
            std::cout << "[ClippedHandoff] switch_presented parent_frame="
                      << presented_parent_frame
                      << " clip=" << clipped_media_state.clip_id
                      << " selected_run_index="
                      << clipped_media_state.selected_run_index << std::endl;
            writeClippedHandoffTraceEvent(
                "switch_presented",
                {{"presented_parent_frame", presented_parent_frame},
                 {"pending_switch_parent_frame",
                  clipped_media_state.pending_switch_parent_frame},
                 {"clip_id", clipped_media_state.clip_id},
                 {"selected_run_index",
                  clipped_media_state.selected_run_index}});
            clipped_media_state.switch_in_progress = false;
            clipped_media_state.pending_switch_parent_frame = -1;
        }

        if (clipped_media_state.switch_in_progress) {
            return;
        }
        if (presented_parent_frame < clipped_media_state.last_parent_frame) {
            return;
        }

        const size_t total_frames = zarr_loader.getTotalFrames();
        const int64_t next_parent_frame =
            static_cast<int64_t>(presented_parent_frame) + 1;
        if (next_parent_frame < 0 ||
            static_cast<size_t>(next_parent_frame) >= total_frames) {
            return;
        }

        const auto* next_row =
            zarr_loader.resolveClippedFrame(next_parent_frame);
        if (next_row == nullptr ||
            next_row->selected_run_index ==
                clipped_media_state.selected_run_index) {
            return;
        }
        const auto* next_selected =
            zarr_loader.getClippedResolver().selectedRun(
                next_row->selected_run_index);
        const std::string old_clip = clipped_media_state.clip_id;
        const size_t old_selected_run = clipped_media_state.selected_run_index;
        clipped_media_state.switch_in_progress = true;
        clipped_media_state.pending_switch_parent_frame = next_parent_frame;
        std::cout << "[ClippedHandoff] switch_request parent_frame="
                  << next_parent_frame << " old_clip=" << old_clip
                  << " new_clip="
                  << (next_selected != nullptr ? next_selected->clip_id
                                               : std::string("<unknown>"))
                  << " old_selected_run_index=" << old_selected_run
                  << " new_selected_run_index="
                  << next_row->selected_run_index
                  << " local_frame=" << next_row->clip_local_frame_index
                  << std::endl;
        writeClippedHandoffTraceEvent(
            "switch_request",
            {{"parent_frame", next_parent_frame},
             {"old_clip_id", old_clip},
             {"new_clip_id",
              next_selected != nullptr ? json(next_selected->clip_id)
                                       : json(nullptr)},
             {"old_selected_run_index", old_selected_run},
             {"new_selected_run_index", next_row->selected_run_index},
             {"clip_local_frame_index", next_row->clip_local_frame_index}});

        playback_session_controller.seekToFrame(
            static_cast<int>(next_parent_frame),
            /*prefer_buffer_when_paused=*/false,
            /*force_inaccurate=*/true,
            /*skip_stimulus_hard_seek=*/true);

        if (clipped_media_state.selected_run_index ==
            next_row->selected_run_index) {
            writeClippedHandoffTraceEvent(
                "switch_loaded",
                {{"parent_frame", next_parent_frame},
                 {"clip_id", clipped_media_state.clip_id},
                 {"selected_run_index",
                  clipped_media_state.selected_run_index},
                 {"first_parent_frame",
                  clipped_media_state.first_parent_frame},
                 {"last_parent_frame",
                  clipped_media_state.last_parent_frame},
                 {"clip_local_frame_index",
                  next_row->clip_local_frame_index}});
        } else {
            std::cout << "[ClippedHandoff] switch_failed parent_frame="
                      << next_parent_frame << " old_clip=" << old_clip
                      << std::endl;
            writeClippedHandoffTraceEvent(
                "switch_failed",
                {{"parent_frame", next_parent_frame},
                 {"old_clip_id", old_clip},
                 {"new_selected_run_index", next_row->selected_run_index}});
            clipped_media_state.switch_in_progress = false;
            clipped_media_state.pending_switch_parent_frame = -1;
        }
    };

    auto makeDecodeDebugDumpContext = [&]() {
        return DecodeDebugDumpContext{
            video_loaded,
            scene,
            &camera_names,
            &window_need_decoding,
            &ps,
            &stimulus_player,
            default_buffer_dump_root,
            video_fps,
        };
    };
    auto reloadActiveZarrPreserveDataset =
        [&](std::string& reload_error,
            std::optional<ZarrDetectionLoader::DetectionDataset>
                preferred_dataset = std::nullopt) -> bool {
            const auto previous_dataset = zarr_loader.getActiveDetectionDataset();
            const std::string archive_path = zarr_loader.getArchivePath();
            if (archive_path.empty()) {
                reload_error = "No loaded Zarr archive.";
                return false;
            }

            if (!zarr_loader.loadZarrFile(archive_path, reload_error)) {
                zarr_loaded = false;
                return false;
            }

            zarr_loaded = true;
            const auto dataset_to_restore =
                preferred_dataset.value_or(previous_dataset);
            if (zarr_loader.isDatasetAvailable(dataset_to_restore)) {
                (void)zarr_loader.setActiveDetectionDataset(dataset_to_restore);
            }
            refreshDetectionDatasetOptions(zarr_loader);
            invalidateReviewFrameCache(review_frame_cache);
            review_frame_status.clear();
            if (zarr_loader.getTotalFrames() > 0 &&
                current_frame_num >=
                    static_cast<int>(zarr_loader.getTotalFrames())) {
                current_frame_num =
                    static_cast<int>(zarr_loader.getTotalFrames()) - 1;
            }
            warmEyeMaskCacheForFrame("zarr_reload", current_frame_num);
            prewarmEyeMaskOverlayTexturesForPlayback("zarr_reload",
                                                     current_frame_num);
            return true;
        };

    while (!glfwWindowShouldClose(window->render_target)) {
        static FileBrowserWindowState file_browser_window_state;
        const auto frame_loop_start = std::chrono::steady_clock::now();
        pollPendingKeypointWrite(
            pending_keypoint_write,
            zarr_loader,
            crop_preview_window_state.editor_state,
            frame_debug_window_state.keypoint_review_panel.full_frame_edit,
            frame_debug_window_state.keypoint_review_panel.manual_write_status,
            reloadActiveZarrPreserveDataset,
            [&]() {
                invalidateReviewFrameCache(review_frame_cache);
                review_frame_status.clear();
                crop_preview_window_state.last_roi_index =
                    std::numeric_limits<int>::min();
                crop_preview_window_state.last_crop_preview_source_frame = -1;
                crop_preview_window_state.rotated_valid = false;
            });
        double frame_camera_upload_ms = 0.0;
        int frame_camera_upload_count = 0;
        double frame_camera_texture_resize_ms = 0.0;
        double frame_camera_preview_resize_ms = 0.0;
        double frame_camera_display_convert_ms = 0.0;
        double frame_camera_pbo_copy_ms = 0.0;
        double frame_camera_texture_upload_ms = 0.0;
        double frame_camera_playback_front_path_ms = 0.0;
        double frame_camera_playback_stage_total_ms = 0.0;
        double frame_camera_playback_stage_upload_ms = 0.0;
        double frame_camera_playback_prewarm_total_ms = 0.0;
        double frame_camera_playback_prewarm_upload_ms = 0.0;
        int frame_camera_playback_prewarm_count = 0;
        double frame_camera_playback_swap_ms = 0.0;
        double frame_camera_plot_image_ui_ms = 0.0;
        double frame_camera_overlay_ui_ms = 0.0;
        double frame_bbox_get_boxes_ms = 0.0;
        double frame_bbox_edit_resolve_ms = 0.0;
        double frame_bbox_get_raw_detections_ms = 0.0;
        double frame_bbox_load_total_ms = 0.0;
        double frame_bbox_overlay_build_ms = 0.0;
        double frame_bbox_overlay_draw_ms = 0.0;
        int frame_bbox_overlay_item_count = 0;
        int frame_bbox_query_frame = -1;
        int frame_bbox_loaded_count = 0;
        int frame_bbox_display_count = 0;
        double frame_subject_shape_overlay_ms = 0.0;
        double frame_tail_kinematics_overlay_ms = 0.0;
        double frame_camera_scene_ui_ms = 0.0;
        double frame_file_browser_ui_ms = 0.0;
        double frame_frame_debug_ui_ms = 0.0;
        double frame_buffer_window_ui_ms = 0.0;
        double frame_crop_preview_ui_ms = 0.0;
        CropPreviewPerfMetrics frame_crop_preview_perf;
        double frame_stimulus_buffer_window_ui_ms = 0.0;
        double frame_keypoints_window_ui_ms = 0.0;
        double frame_labeling_tool_ui_ms = 0.0;
        double frame_stimulus_window_ui_ms = 0.0;
        double frame_stimulus_timeline_ui_ms = 0.0;
        double frame_movement_timeline_ui_ms = 0.0;
        AnalysisTimelinePerfStats frame_analysis_timeline_perf;
        double frame_help_menu_ui_ms = 0.0;
        double frame_gl_draw_ms = 0.0;
        double frame_swap_ms = 0.0;
        double frame_cap_sleep_ms = 0.0;
        double frame_ui_build_ms = 0.0;
        double frame_imgui_render_ms = 0.0;
        int frame_imgui_draw_cmd_count = 0;
        int frame_imgui_draw_list_count = 0;
        int frame_imgui_total_vtx_count = 0;
        int frame_imgui_total_idx_count = 0;
        double frame_mask_data_load_ms = 0.0;
        CameraViewMaskPerfMetrics frame_mask_overlay_perf;
        int playback_trace_presenter_view_idx = -1;
        int playback_trace_presenter_target_frame = -1;
        int playback_trace_presenter_preferred_paused_slot = -1;
        int playback_trace_presented_slot = -1;
        int playback_trace_presented_frame = -1;
        int playback_trace_presenter_resolved_frame = -1;
        bool playback_trace_prewarm_active = false;
        double perf_camera_viewport_width_px =
            std::numeric_limits<double>::quiet_NaN();
        double perf_camera_viewport_height_px =
            std::numeric_limits<double>::quiet_NaN();
        double perf_camera_view_x_min =
            std::numeric_limits<double>::quiet_NaN();
        double perf_camera_view_x_max =
            std::numeric_limits<double>::quiet_NaN();
        double perf_camera_view_y_min =
            std::numeric_limits<double>::quiet_NaN();
        double perf_camera_view_y_max =
            std::numeric_limits<double>::quiet_NaN();
        double perf_camera_view_visible_fraction =
            std::numeric_limits<double>::quiet_NaN();
        int perf_camera_view_zoomed_in = -1;
        int perf_requested_camera_frame = -1;
        int perf_min_decoded_camera_frame = -1;
        int playback_requested_camera_frame = -1;
        int playback_presenter_target_frame = ps.to_display_frame_number;
        int playback_presenter_target_slot = -1;
        bool playback_target_clamped_to_buffer = false;
        int playback_commit_previous_frame = -1;
        int playback_commit_frame = -1;
        int playback_commit_slot = -1;
        int playback_release_attempts = 0;
        int playback_release_count = 0;
        int playback_release_skip_count = 0;
        bool playback_release_deferred = false;

        // Poll and handle events (inputs, window resize, etc.)
        glfwPollEvents();
        if (playback_smoke.enabled && playback_smoke.started &&
            !playback_smoke.completed &&
            std::chrono::duration<double>(
                std::chrono::steady_clock::now() -
                playback_smoke.start_time)
                    .count() > playback_smoke.timeout_s) {
            int latest_decoded = -1;
            const int visible_idx =
                playback_session_controller.getVisibleCameraIndex();
            if (visible_idx >= 0 &&
                visible_idx < static_cast<int>(camera_names.size())) {
                auto latest_it =
                    latest_decoded_frame.find(camera_names[visible_idx]);
                if (latest_it != latest_decoded_frame.end()) {
                    latest_decoded = latest_it->second.load();
                }
            }
            std::cerr << "[PlaybackSmoke] TIMEOUT target_frame="
                      << playback_smoke.end_frame
                      << " current_frame=" << current_frame_num
                      << " to_display_frame=" << ps.to_display_frame_number
                      << " last_presented="
                      << playback_smoke.last_presented_frame
                      << " max_presented="
                      << playback_smoke.max_presented_frame
                      << " presented_count="
                      << playback_smoke.presented_count
                      << " latest_decoded=" << latest_decoded << std::endl;
            writePlaybackTraceEvent(
                "playback_smoke_timeout",
                {{"start_frame", playback_smoke.start_frame},
                 {"end_frame", playback_smoke.end_frame},
                 {"current_frame", current_frame_num},
                 {"to_display_frame", ps.to_display_frame_number},
                 {"last_presented", playback_smoke.last_presented_frame},
                 {"max_presented", playback_smoke.max_presented_frame},
                 {"presented_count", playback_smoke.presented_count},
                 {"latest_decoded", latest_decoded}},
                visible_idx,
                ps.to_display_frame_number,
                -1,
                playback_smoke.last_presented_slot,
                playback_smoke.last_presented_frame,
                current_frame_num);
            app_exit_code = 3;
            glfwSetWindowShouldClose(window->render_target, GLFW_TRUE);
        }
        if (clipped_boundary_smoke.enabled &&
            clipped_boundary_smoke.started &&
            !clipped_boundary_smoke.completed &&
            std::chrono::steady_clock::now() -
                    clipped_boundary_smoke.start_time >
                std::chrono::seconds(20)) {
            std::cerr << "[ClippedBoundarySmoke] timeout waiting for frame "
                      << clipped_boundary_smoke.end_frame
                      << " current_frame=" << current_frame_num
                      << " state=" << clippedStateJson().dump()
                      << std::endl;
            writeClippedHandoffTraceEvent(
                "smoke_timeout",
                {{"start_frame", clipped_boundary_smoke.start_frame},
                 {"end_frame", clipped_boundary_smoke.end_frame},
                 {"current_frame", current_frame_num}});
            app_exit_code = 3;
            glfwSetWindowShouldClose(window->render_target, GLFW_TRUE);
        }

        // Start the Dear ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        const auto ui_build_start = std::chrono::steady_clock::now();

        playback_session_controller.pollSeekState();

        // --- Update playback time ---
        auto now = std::chrono::steady_clock::now();

        if (ps.play_video) {
            ps.accumulated_play_time +=
                std::chrono::duration<double>(now - ps.last_play_time_start)
                    .count() *
                set_playback_speed;
            ps.last_play_time_start = now;
        }
        double playback_time_now = ps.accumulated_play_time;

        const bool clipped_collection_playback =
            zarr_loaded && zarr_loader.hasClippedCollection();
        auto visibleCameraIndexForPlayback = [&]() -> int {
            int visible_idx = playback_session_controller.getVisibleCameraIndex();
            if (visible_idx < 0 || visible_idx >= scene->num_cams) {
                visible_idx = scene->num_cams > 0 ? 0 : -1;
            }
            return visible_idx;
        };
        auto findExactBufferedFrameSlot =
            [&](int target_frame, int preferred_slot) -> int {
            if (scene->num_cams <= 0 || scene->size_of_buffer <= 0 ||
                target_frame < 0) {
                return -1;
            }
            const int visible_idx = visibleCameraIndexForPlayback();
            if (visible_idx < 0) {
                return -1;
            }
            auto slotMatches = [&](int slot_idx) -> bool {
                if (slot_idx < 0 ||
                    slot_idx >= static_cast<int>(scene->size_of_buffer)) {
                    return false;
                }
                auto metadata = frameSlotSnapshotReadable(
                    scene->cameras[visible_idx].display_buffer[slot_idx]);
                return metadata && metadata->frame_number == target_frame;
            };
            if (slotMatches(preferred_slot)) {
                return preferred_slot;
            }
            for (int slot_idx = 0;
                 slot_idx < static_cast<int>(scene->size_of_buffer);
                 ++slot_idx) {
                if (slotMatches(slot_idx)) {
                    return slot_idx;
                }
            }
            return -1;
        };
        auto findBestBufferedFrameAtOrBefore =
            [&](int target_frame, int min_frame, int& out_slot) -> int {
            out_slot = -1;
            if (scene->num_cams <= 0 || scene->size_of_buffer <= 0 ||
                target_frame < 0) {
                return -1;
            }
            const int visible_idx = visibleCameraIndexForPlayback();
            if (visible_idx < 0) {
                return -1;
            }
            int best_frame = -1;
            for (int slot_idx = 0;
                 slot_idx < static_cast<int>(scene->size_of_buffer);
                 ++slot_idx) {
                auto metadata = frameSlotSnapshotReadable(
                    scene->cameras[visible_idx].display_buffer[slot_idx]);
                if (!metadata || metadata->frame_number > target_frame ||
                    metadata->frame_number <= min_frame ||
                    metadata->frame_number <= best_frame) {
                    continue;
                }
                best_frame = metadata->frame_number;
                out_slot = slot_idx;
            }
            return best_frame;
        };
        auto computePlaybackClockTarget = [&]() -> int {
            int frame_to_show =
                static_cast<int>(std::ceil(playback_time_now * video_fps));
            playback_requested_camera_frame = frame_to_show;
            perf_requested_camera_frame = frame_to_show;

            int min_decoded_frame = INT_MAX;
            bool have_decode_bound = false;
            auto considerDecodeBound = [&](const std::string& stream_name) {
                auto need_it = window_need_decoding.find(stream_name);
                if (need_it == window_need_decoding.end() ||
                    !need_it->second.load()) {
                    return;
                }
                auto latest_it = latest_decoded_frame.find(stream_name);
                if (latest_it == latest_decoded_frame.end()) {
                    return;
                }
                const int decoded = latest_it->second.load();
                if (decoded < 0) {
                    return;
                }
                min_decoded_frame = std::min(min_decoded_frame, decoded);
                have_decode_bound = true;
            };
            for (const auto& cam_name : camera_names) {
                considerDecodeBound(cam_name);
            }
            perf_min_decoded_camera_frame =
                have_decode_bound ? min_decoded_frame : -1;
            const int bounded_frame =
                have_decode_bound ? std::min(frame_to_show, min_decoded_frame)
                                  : ps.to_display_frame_number;
            return std::max(ps.to_display_frame_number, bounded_frame);
        };
        if (!ps.just_seeked && dc_context->decoding_flag && ps.play_video &&
            scene->size_of_buffer > 0) {
            playback_presenter_target_frame = computePlaybackClockTarget();
            const int requested_target = playback_presenter_target_frame;
            if (playback_presenter_target_frame >
                ps.to_display_frame_number) {
                const int preferred_slot =
                    scene->size_of_buffer > 0
                        ? ps.read_head % scene->size_of_buffer
                        : -1;
                playback_presenter_target_slot =
                    findExactBufferedFrameSlot(playback_presenter_target_frame,
                                               preferred_slot);
                if (playback_presenter_target_slot < 0) {
                    int best_slot = -1;
                    const int best_frame = findBestBufferedFrameAtOrBefore(
                        playback_presenter_target_frame,
                        ps.to_display_frame_number, best_slot);
                    if (best_frame > ps.to_display_frame_number &&
                        best_slot >= 0) {
                        playback_presenter_target_frame = best_frame;
                        playback_presenter_target_slot = best_slot;
                    } else {
                        playback_presenter_target_frame =
                            ps.to_display_frame_number;
                    }
                }
            }
            playback_target_clamped_to_buffer =
                requested_target != playback_presenter_target_frame;
            if (clipped_collection_playback &&
                playback_target_clamped_to_buffer) {
                writeClippedPlaybackStateEvent(
                    "playback_target_clamped_to_buffer",
                    json{{"requested_frame", requested_target},
                         {"selected_frame", playback_presenter_target_frame},
                         {"previous_committed_frame",
                          ps.to_display_frame_number},
                         {"target_slot", playback_presenter_target_slot}},
                    false);
            }
        }

        const auto file_browser_ui_start = std::chrono::steady_clock::now();
        if (video_loaded && !legacy_labeling_state.skeleton_chosen) {
            legacy_labeling_state.ensureSkeletonResources();
        }
        const std::string active_skeleton_name =
            legacy_labeling_state.activeSkeletonName();
        const bool has_active_zarr_keypoint_review =
            zarr_loaded && zarr_loader.hasKeypointData();
        FileBrowserWindowContext file_browser_context{
            ui_path_config,
            start_folder_name,
            root_dir,
            skeleton_dir,
            video_loaded,
            !has_active_zarr_keypoint_review,
            legacy_labeling_state.manual_label_mode,
            legacy_labeling_state.skeleton_chosen,
            active_skeleton_name,
            legacy_labeling_state.skeleton_map,
            cpu_buffer_toggle,
            scene->use_cpu_buffer,
            label_buffer_size,
            playback_preview_scale_mode,
            playback_renderer_mode,
            yolo_detection,
            ps.play_video,
            set_playback_speed,
            inst_speed,
            video_fps,
            current_frame_num,
            ps.last_frame_num_playspeed,
            ps.last_wall_time_playspeed,
            stimulus_player.loaded,
            stimulus_buffer_size,
            stimulus_use_cpu_buffer,
            stimulus_use_software_decode,
            stimulus_player.buffer_size,
            stimulus_player.use_cpu_buffer,
            stimulus_player.use_software_decode,
            dc_context->seek_interval,
        };
        FileBrowserWindowResult file_browser_result =
            drawFileBrowserWindow(file_browser_context,
                                  file_browser_window_state);
        if (file_browser_result.skeleton_selection.has_value()) {
            const auto& selection = *file_browser_result.skeleton_selection;
            bool load_calibration = true;
            if (scene->num_cams > 1) {
                for (u32 i = 0; i < scene->num_cams; i++) {
                    std::string cam_file = root_dir + "/calibration/" +
                                           camera_names[i] + ".yaml";

                    if (!std::filesystem::exists(cam_file)) {
                        load_calibration = false;
                        error_message = "Calibration file not found: " + cam_file;
                        show_error = true;
                        break;
                    }
                    if (!camera_load_params_from_yaml(cam_file,
                                                      camera_params[i],
                                                      error_message)) {
                        load_calibration = false;
                        camera_params.clear();
                        camera_params.resize(scene->num_cams);
                        show_error = true;
                        break;
                    }
                }
            }

            if (load_calibration) {
                skeleton_initialize(selection.name,
                                    root_dir,
                                    legacy_labeling_state.skeleton.get(),
                                    selection.primitive);
                legacy_labeling_state.activateManualMode(root_dir);
            }
        }
        switch (file_browser_result.detection_action) {
        case FileBrowserDetectionAction::YOLOv5: {
            std::string yolov5_onnx = root_dir + "/yolo/v5/best.onnx";
            std::string yolov5_labelname = root_dir + "/yolo/v5/label.names";
            read_yolo_labels(yolov5_labelname, &yolo_setting);

            for (int i = 0; i < scene->num_cams; i++) {
                yolo_threads.push_back(std::thread(&yolo_process,
                                                   yolov5_onnx,
                                                   &yolo_setting,
                                                   i));
            }
            yolo_detection = true;
            break;
        }
        case FileBrowserDetectionAction::YOLOv8: {
            std::string engine_file_path =
                root_dir + "/yolo/yolorat_bbox/rat_bbox.engine";
            for (int i = 0; i < scene->num_cams; i++) {
                yolo_threads.push_back(std::thread(&yolo_process_trt,
                                                   engine_file_path,
                                                   i,
                                                   scene->cameras[i].image_width,
                                                   scene->cameras[i].image_height));
            }
            yolo_detection = true;
            break;
        }
        case FileBrowserDetectionAction::YOLOv8Pose: {
            std::string engine_file_path =
                root_dir + "/yolo/yolopose/rat_pose.engine";
            for (int i = 0; i < scene->num_cams; i++) {
                yolo_threads.push_back(std::thread(&yolo_process_v8pose,
                                                   engine_file_path,
                                                   i,
                                                   scene->cameras[i].image_width,
                                                   scene->cameras[i].image_height));
            }
            yolo_detection = true;
            break;
        }
        case FileBrowserDetectionAction::None:
            break;
        }
        if (file_browser_result.accurate_seek_target_frame.has_value()) {
            playback_session_controller.seekToFrame(
                *file_browser_result.accurate_seek_target_frame, false);
        }
        frame_file_browser_ui_ms +=
            durationMs(std::chrono::steady_clock::now() - file_browser_ui_start);

        const bool use_legacy_manual_keypoint_tools =
            legacy_labeling_state.toolsEnabled(
                has_active_zarr_keypoint_review);
        std::optional<RefinedKeypointSelection>
            active_full_frame_keypoint_selection;
        bool keypoint_tab_full_frame_edit_enabled = false;

        if (video_loaded) {
            const auto frame_debug_ui_start = std::chrono::steady_clock::now();
            if (!use_legacy_manual_keypoint_tools) {
                legacy_labeling_state.keypoints_find = false;
            }
            std::vector<LoggedBoundingBox> zarr_boxes;
            bool frame_is_interpolated = false;
            const bool frame_has_bbox_edits =
                g_zarr_bbox_edit_state.isFrameDirty(current_frame_num);
            bool dataset_has_synthetic_boxes = false;
            bool dataset_allows_bbox_edit = false;
            ZarrDetectionLoader::FrameDetections detection_details;
            const ZarrDetectionLoader::FrameDetections* detection_details_ptr =
                nullptr;
            if (zarr_loaded) {
                dataset_has_synthetic_boxes =
                    zarr_loader.activeDatasetHasSyntheticDetections();
                if (zarr_loader.hasInterpolation()) {
                    frame_is_interpolated =
                        zarr_loader.isFrameInterpolated(current_frame_num);
                }
                std::vector<LoggedBoundingBox> loaded_zarr_boxes =
                    zarr_loader.getBoundingBoxesForFrame(current_frame_num);
                zarr_boxes = g_zarr_bbox_edit_state.resolveFrameBoxes(
                    current_frame_num, loaded_zarr_boxes);
                const bool active_dataset_is_raw_detect =
                    zarr_loader.hasDetectionData() &&
                    (zarr_loader.getActiveDetectionDataset() ==
                     ZarrDetectionLoader::DetectionDataset::RawDetect);
                dataset_allows_bbox_edit =
                    zarr_loader.hasDetectionData() &&
                    !active_dataset_is_raw_detect &&
                    !zarr_loader.hasClippedCollection();
                if (!dataset_allows_bbox_edit) {
                    g_zarr_bbox_edit_state.draw_mode = false;
                    g_zarr_bbox_edit_state.cancelDraw();
                    g_zarr_bbox_edit_state.clearSelection();
                }
                const bool subject_shape_needs_contours =
                    subject_shape_overlay_options.show_overlay &&
                    (subject_shape_overlay_options.show_body_contour ||
                     subject_shape_overlay_options.show_swim_bladder_contour ||
                     subject_shape_overlay_options.show_eye_contours);
                const bool include_subject_shapes_in_details =
                    zarr_loader.hasSubjectShapeData() &&
                    (subject_shape_overlay_options.show_overlay ||
                     (zarr_loader.hasTailKinematicsData() &&
                      tail_kinematics_overlay_options.show_overlay) ||
                     (show_eye_masks && show_eye_angle_arcs &&
                      zarr_loader.hasEyeAngleData()) ||
                     frame_debug_window_state.active_tab ==
                         FrameInspectTab::EyeMasks);
                const bool include_eye_masks_in_details =
                    zarr_loader.hasEyeMasks() &&
                    (show_eye_masks || subject_shape_needs_contours ||
                     frame_debug_window_state.active_tab ==
                         FrameInspectTab::EyeMasks);
                const bool need_details =
                    zarr_loader.hasScores() ||
                    zarr_loader.hasHeadingData() ||
                    zarr_loader.hasKeypointData() ||
                    include_eye_masks_in_details ||
                    include_subject_shapes_in_details ||
                    dataset_has_synthetic_boxes;
                if (need_details) {
                    const auto details_load_start =
                        std::chrono::steady_clock::now();
                    detection_details =
                        zarr_loader.getRawDetections(current_frame_num,
                                                     false,
                                                     include_eye_masks_in_details,
                                                     include_subject_shapes_in_details);
                    if (include_eye_masks_in_details) {
                        frame_mask_data_load_ms += durationMs(
                            std::chrono::steady_clock::now() -
                            details_load_start);
                    }
                    detection_details_ptr = &detection_details;
                }
            }

            const FrameDebugWindowContext frame_debug_context{
                current_frame_num,
                ps.to_display_frame_number,
                ps.slider_frame_number,
                frame_sync_valid_slots,
                frame_sync_empty_slots,
                scene->size_of_buffer,
                frame_sync_recording_remaining,
                frame_sync_recording_total,
                frame_sync_latest_decoded,
                frame_sync_debug_line,
                zarr_loaded,
                zarr_loader,
                detection_dataset_labels,
                detection_dataset_choice,
                dataset_has_synthetic_boxes,
                frame_is_interpolated,
                frame_has_bbox_edits,
                dataset_allows_bbox_edit,
                zarr_boxes,
                detection_details_ptr,
                review_frame_filters,
                review_frame_cache.valid,
                review_frame_cache.frames.size(),
                review_frame_status,
                decode_debug_status,
                default_buffer_dump_root.string(),
                g_zarr_bbox_edit_state,
                ps.play_video,
                bbox_payload_status,
                show_keypoint_markers,
                show_heading_arrows,
                show_eye_masks,
                show_subject_body_mask,
                show_eye_left_mask,
                show_eye_right_mask,
                show_swim_bladder_mask,
                show_eye_direction_beams,
                show_eye_gaze_rays,
                show_eye_angle_arcs,
                show_eye_angle_labels,
                mask_overlay_mode,
                subject_shape_overlay_options,
                tail_kinematics_overlay_options,
                show_movement_trail,
                movement_trail_seconds,
                movement_trail_valid_samples_only,
                stimulus_inset_options,
                show_stimulus_debug_windows,
            };
            const FrameDebugWindowResult frame_debug_result =
                drawFrameDebugWindow(frame_debug_context, frame_debug_window_state);
            const DiagnosticsWindowResult diagnostics_result =
                drawDiagnosticsWindow(frame_debug_context);

            show_keypoint_markers = frame_debug_result.show_keypoint_markers;
            show_heading_arrows = frame_debug_result.show_heading_arrows;
            show_eye_masks = frame_debug_result.show_eye_masks;
            show_subject_body_mask =
                frame_debug_result.show_subject_body_mask;
            show_eye_left_mask = frame_debug_result.show_eye_left_mask;
            show_eye_right_mask = frame_debug_result.show_eye_right_mask;
            show_swim_bladder_mask =
                frame_debug_result.show_swim_bladder_mask;
            show_eye_direction_beams =
                frame_debug_result.show_eye_direction_beams;
            show_eye_gaze_rays = frame_debug_result.show_eye_gaze_rays;
            show_eye_angle_arcs = frame_debug_result.show_eye_angle_arcs;
            show_eye_angle_labels = frame_debug_result.show_eye_angle_labels;
            mask_overlay_mode = frame_debug_result.mask_overlay_mode;
            subject_shape_overlay_options =
                frame_debug_result.subject_shape_overlay_options;
            tail_kinematics_overlay_options =
                frame_debug_result.tail_kinematics_overlay_options;
            show_movement_trail = frame_debug_result.show_movement_trail;
            movement_trail_seconds =
                frame_debug_result.movement_trail_seconds;
            movement_trail_valid_samples_only =
                frame_debug_result.movement_trail_valid_samples_only;
            stimulus_inset_options =
                frame_debug_result.stimulus_inset_options;
            show_stimulus_debug_windows =
                frame_debug_result.show_stimulus_debug_windows;
            active_full_frame_keypoint_selection =
                frame_debug_result.selected_keypoint_selection;
            keypoint_tab_full_frame_edit_enabled =
                frame_debug_window_state.active_tab ==
                    FrameInspectTab::Keypoints &&
                frame_debug_window_state.keypoint_review_panel
                    .full_frame_edit.enabled &&
                active_full_frame_keypoint_selection.has_value();

            if (frame_debug_result.requested_detection_dataset_index >= 0 &&
                frame_debug_result.requested_detection_dataset_index <
                    static_cast<int>(detection_dataset_ids.size()) &&
                zarr_loader.setActiveDetectionDataset(
                    detection_dataset_ids[frame_debug_result
                                              .requested_detection_dataset_index])) {
                detection_dataset_choice =
                    frame_debug_result.requested_detection_dataset_index;
                refreshDetectionDatasetOptions(zarr_loader);
                g_zarr_bbox_edit_state.clearAll();
                invalidateReviewFrameCache(review_frame_cache);
                review_frame_status.clear();
                if (zarr_loader.getTotalFrames() > 0 &&
                    current_frame_num >=
                        static_cast<int>(zarr_loader.getTotalFrames())) {
                    current_frame_num =
                        static_cast<int>(zarr_loader.getTotalFrames()) - 1;
                }
            }

            if (frame_debug_result.review_filters_changed) {
                review_frame_filters =
                    frame_debug_result.review_frame_filters;
                invalidateReviewFrameCache(review_frame_cache);
                review_frame_status.clear();
            }
            if (frame_debug_result.request_prev_review_frame) {
                auto jump_result = computeReviewFrameJump(
                    zarr_loaded, zarr_loader, review_frame_filters,
                    review_frame_cache, current_frame_num, false);
                review_frame_status = std::move(jump_result.status);
                if (jump_result.target_frame.has_value()) {
                    playback_session_controller.seekToFrame(
                        *jump_result.target_frame, true);
                }
            }
            if (frame_debug_result.request_next_review_frame) {
                auto jump_result = computeReviewFrameJump(
                    zarr_loaded, zarr_loader, review_frame_filters,
                    review_frame_cache, current_frame_num, true);
                review_frame_status = std::move(jump_result.status);
                if (jump_result.target_frame.has_value()) {
                    playback_session_controller.seekToFrame(
                        *jump_result.target_frame, true);
                }
            }
            if (frame_debug_result.request_prev_subject_shape_qc_frame) {
                auto jump_result = zarr_loader.computeSubjectShapeQcJump(
                    frame_debug_window_state.subject_shape_qc_filters,
                    current_frame_num,
                    false);
                frame_debug_window_state.subject_shape_qc_status =
                    std::move(jump_result.status);
                if (jump_result.target_frame.has_value()) {
                    playback_session_controller.seekToFrame(
                        *jump_result.target_frame, true);
                }
            }
            if (frame_debug_result.request_next_subject_shape_qc_frame) {
                auto jump_result = zarr_loader.computeSubjectShapeQcJump(
                    frame_debug_window_state.subject_shape_qc_filters,
                    current_frame_num,
                    true);
                frame_debug_window_state.subject_shape_qc_status =
                    std::move(jump_result.status);
                if (jump_result.target_frame.has_value()) {
                    playback_session_controller.seekToFrame(
                        *jump_result.target_frame, true);
                }
            }
            if (frame_debug_result.request_prev_tail_kinematics_qc_frame) {
                auto jump_result = zarr_loader.computeTailKinematicsQcJump(
                    frame_debug_window_state.tail_kinematics_qc_filters,
                    current_frame_num,
                    false);
                frame_debug_window_state.tail_kinematics_qc_status =
                    std::move(jump_result.status);
                if (jump_result.target_row.has_value()) {
                    frame_debug_window_state.tail_kinematics_selected_row =
                        static_cast<int>(*jump_result.target_row);
                }
                if (jump_result.target_frame.has_value()) {
                    playback_session_controller.seekToFrame(
                        *jump_result.target_frame, true);
                }
            }
            if (frame_debug_result.request_next_tail_kinematics_qc_frame) {
                auto jump_result = zarr_loader.computeTailKinematicsQcJump(
                    frame_debug_window_state.tail_kinematics_qc_filters,
                    current_frame_num,
                    true);
                frame_debug_window_state.tail_kinematics_qc_status =
                    std::move(jump_result.status);
                if (jump_result.target_row.has_value()) {
                    frame_debug_window_state.tail_kinematics_selected_row =
                        static_cast<int>(*jump_result.target_row);
                }
                if (jump_result.target_frame.has_value()) {
                    playback_session_controller.seekToFrame(
                        *jump_result.target_frame, true);
                }
            }
            if (frame_debug_result.request_seek_tail_kinematics_row) {
                frame_debug_window_state.tail_kinematics_selected_row =
                    static_cast<int>(
                        frame_debug_result.requested_tail_kinematics_row);
                auto frame = zarr_loader.getTailKinematicsFrameForRow(
                    frame_debug_result.requested_tail_kinematics_row);
                if (frame.has_value()) {
                    playback_session_controller.seekToFrame(*frame, true);
                    frame_debug_window_state.tail_kinematics_qc_status =
                        "Selected tail row " +
                        std::to_string(
                            frame_debug_result.requested_tail_kinematics_row) +
                        " mapped to frame " + std::to_string(*frame) + ".";
                } else {
                    frame_debug_window_state.tail_kinematics_qc_status =
                        "Selected tail row has no frame mapping.";
                }
            }
            if (frame_debug_result.request_prev_eye_angle_qc_frame) {
                auto jump_result = zarr_loader.computeEyeAngleQcJump(
                    frame_debug_window_state.eye_angle_qc_filters,
                    current_frame_num,
                    false);
                frame_debug_window_state.eye_angle_qc_status =
                    std::move(jump_result.status);
                if (jump_result.target_row.has_value()) {
                    frame_debug_window_state.eye_angle_selected_row =
                        static_cast<int>(*jump_result.target_row);
                }
                if (jump_result.target_frame.has_value()) {
                    playback_session_controller.seekToFrame(
                        *jump_result.target_frame, true);
                }
            }
            if (frame_debug_result.request_next_eye_angle_qc_frame) {
                auto jump_result = zarr_loader.computeEyeAngleQcJump(
                    frame_debug_window_state.eye_angle_qc_filters,
                    current_frame_num,
                    true);
                frame_debug_window_state.eye_angle_qc_status =
                    std::move(jump_result.status);
                if (jump_result.target_row.has_value()) {
                    frame_debug_window_state.eye_angle_selected_row =
                        static_cast<int>(*jump_result.target_row);
                }
                if (jump_result.target_frame.has_value()) {
                    playback_session_controller.seekToFrame(
                        *jump_result.target_frame, true);
                }
            }
            if (frame_debug_result.request_seek_eye_angle_row) {
                frame_debug_window_state.eye_angle_selected_row =
                    static_cast<int>(
                        frame_debug_result.requested_eye_angle_row);
                auto frame = zarr_loader.getEyeAngleFrameForRow(
                    frame_debug_result.requested_eye_angle_row);
                if (frame.has_value()) {
                    playback_session_controller.seekToFrame(*frame, true);
                    frame_debug_window_state.eye_angle_qc_status =
                        "Selected eye-angle row " +
                        std::to_string(
                            frame_debug_result.requested_eye_angle_row) +
                        " mapped to frame " + std::to_string(*frame) + ".";
                } else {
                    frame_debug_window_state.eye_angle_qc_status =
                        "Selected eye-angle row has no frame mapping.";
                }
            }
            if (diagnostics_result.request_dump_decode_buffers) {
                dumpDecodeBuffersToVideos(makeDecodeDebugDumpContext(),
                                         "manual_dump", decode_debug_status);
            }
            if (diagnostics_result.request_random_seek_dump) {
                randomSeekAndDumpBuffers(
                    RandomSeekDumpContext{
                        makeDecodeDebugDumpContext(),
                        dc_context,
                        &debug_rng,
                        [&](int target_frame, bool prefer_buffer_when_paused) {
                            playback_session_controller.seekToFrame(
                                target_frame, prefer_buffer_when_paused);
                        },
                        [&](bool enabled) {
                            playback_session_controller.setCameraDecodeRequests(
                                enabled);
                        },
                    },
                    decode_debug_status);
            }
            if (frame_debug_result.request_reset_frame_bbox_edits) {
                g_zarr_bbox_edit_state.clearFrameEdits(current_frame_num);
            }
            if (frame_debug_result.request_clear_bbox_selection) {
                g_zarr_bbox_edit_state.clearSelection();
            }
            if (frame_debug_result.request_build_manual_payload_preview) {
                manual_payload_preview = buildManualDetectPayloadPreview(
                    zarr_loaded, zarr_loader, g_zarr_bbox_edit_state,
                    scene->num_cams > 0
                        ? static_cast<int>(scene->cameras[0].image_width)
                        : 0,
                    scene->num_cams > 0
                        ? static_cast<int>(scene->cameras[0].image_height)
                        : 0);
                if (!manual_payload_preview->valid) {
                    bbox_payload_status =
                        "Manual payload preview failed: " +
                        manual_payload_preview->error;
                } else {
                    bbox_payload_status = summarizeManualDetectPayloadPreview(
                        *manual_payload_preview,
                        g_zarr_bbox_edit_state.dirtyFrameCount());
                }
            }
            if (frame_debug_result.request_write_manual_payload) {
                if (zarr_loaded && zarr_loader.hasClippedCollection()) {
                    manual_payload_preview.reset();
                    bbox_payload_status =
                        "Manual write disabled for clipped finalized collections.";
                } else {
                    manual_payload_preview = buildManualDetectPayloadPreview(
                        zarr_loaded, zarr_loader, g_zarr_bbox_edit_state,
                        scene->num_cams > 0
                            ? static_cast<int>(scene->cameras[0].image_width)
                            : 0,
                        scene->num_cams > 0
                            ? static_cast<int>(scene->cameras[0].image_height)
                            : 0);
                    if (!manual_payload_preview->valid) {
                        bbox_payload_status =
                            "Manual write failed: payload preview invalid: " +
                            manual_payload_preview->error;
                    } else {
                        std::string source_variant = "interpolated";
                        if (zarr_loader.getActiveDetectionDataset() ==
                            ZarrDetectionLoader::DetectionDataset::RefinedFiltered) {
                            source_variant = "filtered";
                        }

                        std::string write_error;
                        std::string resolved_refined_run;
                        ManualWriteReviewOptions review_opts;
                        const auto review_metadata = resolveReviewMetadataValues(
                            frame_debug_window_state.manual_write_review);
                        review_opts.intended_use =
                            review_metadata.intended_use;
                        review_opts.state = review_metadata.review_state;
                        review_opts.method = review_metadata.method;
                        review_opts.reviewer = review_metadata.reviewer;
                        review_opts.notes = review_metadata.notes;
                        const bool write_ok =
                            zarr_loader.writeManualRefinedDetections(
                                manual_payload_preview->frame_indices,
                                manual_payload_preview->bbox_norm_coords,
                                manual_payload_preview->scores,
                                manual_payload_preview->class_ids,
                                manual_payload_preview->frame_counts,
                                manual_payload_preview->detection_source,
                                manual_payload_preview->reason,
                                "manual",
                                source_variant,
                                write_error,
                                &resolved_refined_run,
                                review_opts);
                        if (!write_ok) {
                            bbox_payload_status =
                                "Manual write failed: " + write_error;
                        } else {
                            const size_t written_detections =
                                manual_payload_preview->total_detections;
                            std::string reload_error;
                            const std::string archive_path =
                                zarr_loader.getArchivePath();
                            if (!archive_path.empty() &&
                                zarr_loader.loadZarrFile(archive_path,
                                                         reload_error)) {
                                zarr_loaded = true;
                                if (zarr_loader.isDatasetAvailable(
                                        ZarrDetectionLoader::DetectionDataset::
                                            RefinedRoot)) {
                                    (void)zarr_loader.setActiveDetectionDataset(
                                        ZarrDetectionLoader::DetectionDataset::
                                            RefinedRoot);
                                }
                                refreshDetectionDatasetOptions(zarr_loader);
                                g_zarr_bbox_edit_state.clearAll();
                                manual_payload_preview.reset();
                                invalidateReviewFrameCache(review_frame_cache);
                                review_frame_status.clear();
                                if (zarr_loader.getTotalFrames() > 0 &&
                                    current_frame_num >= static_cast<int>(
                                                             zarr_loader
                                                                 .getTotalFrames())) {
                                    current_frame_num =
                                        static_cast<int>(
                                            zarr_loader.getTotalFrames()) -
                                        1;
                                }
                                warmEyeMaskCacheForFrame("manual_write_reload",
                                                         current_frame_num);
                                prewarmEyeMaskOverlayTexturesForPlayback(
                                    "manual_write_reload",
                                    current_frame_num);
                                std::ostringstream payload_msg;
                                payload_msg
                                    << "Manual write complete: run="
                                    << (resolved_refined_run.empty() ? "<latest>"
                                                                    : resolved_refined_run)
                                    << " surface=instances"
                                    << " resolved_group=refined"
                                    << " detections=" << written_detections;
                                bbox_payload_status = payload_msg.str();
                            } else {
                                zarr_loaded = false;
                                g_zarr_bbox_edit_state.clearAll();
                                bbox_payload_status =
                                    "Manual write succeeded but reload failed: " +
                                    reload_error;
                            }
                        }
                    }
                }
            }
            if (frame_debug_result.request_keypoint_review_write) {
                RefinedKeypointRepository refined_keypoint_repo(zarr_loader);
                const RefinedKeypointReviewWriteWorkflowResult
                    review_write_result = applyRefinedKeypointReviewWrite(
                        refined_keypoint_repo,
                        RefinedKeypointReviewPanelResult{
                            frame_debug_result.selected_keypoint_selection,
                            CropKeypointEditorAction{},
                            frame_debug_result.request_keypoint_review_write,
                            frame_debug_result.keypoint_review_options,
                        },
                        frame_debug_window_state.keypoint_review_panel
                            .review_write_status,
                        reloadActiveZarrPreserveDataset);
                if (review_write_result.should_clear_zarr_loaded) {
                    zarr_loaded = false;
                }
            }
            if (frame_debug_result.keypoint_edit_action.type !=
                CropKeypointEditorActionType::None) {
                startKeypointWriteIfRequested(
                    pending_keypoint_write,
                    zarr_loader,
                    frame_debug_result.keypoint_edit_action,
                    frame_debug_result.selected_keypoint_selection,
                    true,
                    true,
                    frame_debug_window_state.keypoint_review_panel
                        .manual_write_status);
            }
            frame_frame_debug_ui_ms +=
                durationMs(std::chrono::steady_clock::now() - frame_debug_ui_start);
        }

        // file explorer display
        if (ImGuiFileDialog::Instance()->Display("ChooseMedia")) {
            if (ImGuiFileDialog::Instance()->IsOk()) { // action if OK
                auto selected_files =
                    ImGuiFileDialog::Instance()->GetSelection();
                root_dir = ImGuiFileDialog::Instance()->GetCurrentPath();
                skeleton_dir = root_dir;

                // Reset any previously loaded stimulus video
                destroyStimulusPlayback(stimulus_player);
                window_need_decoding[stimulus_player.window_name].store(false);
                window_was_decoding[stimulus_player.window_name] = false;

                // Try to load Zarr detection file
                std::string zarr_error;
                if (loadZarrDetectionFromDirectory(root_dir, zarr_loader, zarr_error)) {
                    zarr_loaded = true;
                    refreshDetectionDatasetOptions(zarr_loader);
                    g_zarr_bbox_edit_state.clearAll();
                    warmEyeMaskCacheForFrame("media_dialog_zarr",
                                             current_frame_num);
                    prewarmEyeMaskOverlayTexturesForPlayback(
                        "media_dialog_zarr",
                        current_frame_num);
                } else {
                    zarr_loaded = false;
                    g_zarr_bbox_edit_state.clearAll();
                    std::cout << "No Zarr detection file found (optional): " << zarr_error << std::endl;
                    detection_dataset_ids.clear();
                    detection_dataset_labels.clear();
                    detection_dataset_choice = 0;
                }

                // check if it is mp4, if it is mp4 files
                auto first_selection =
                    *selected_files.begin(); // Dereferencing iterator
                if (string_ends_with(first_selection.first, ".mp4")) {
                    for (const auto &elem : selected_files) {
                        std::string cam_string_full = elem.first;
                        std::size_t last_slash = cam_string_full.find_last_of("/\\");
                        if (last_slash != std::string::npos) {
                            cam_string_full = cam_string_full.substr(last_slash + 1);
                        }
                        std::size_t cam_string_mp4_position =
                            cam_string_full.find(".mp4");
                        std::string cam_string =
                            cam_string_full.substr(0, cam_string_mp4_position);
                        camera_names.push_back(cam_string);
                        std::cout << "camera names: " << cam_string
                                  << std::endl;
                        window_need_decoding[cam_string].store(true);
                        window_was_decoding[cam_string] = true;
                        std::map<std::string, std::string> m;
                        demuxers.push_back(
                            std::make_unique<FFmpegDemuxer>(elem.second.c_str(), m));
                    }
                    std::map<std::string, std::string> m;
                    FFmpegDemuxer dummy_dmuxer(
                        selected_files.begin()->second.c_str(), m);
                    dc_context->seek_interval =
                        (int)dummy_dmuxer
                            .FindKeyFrameInterval(); // get the seek interval
                    video_fps = dummy_dmuxer.GetFramerate();
                    scene->num_cams = selected_files.size();
                    scene->cameras.resize(scene->num_cams);
                    for (u32 j = 0; j < scene->num_cams; j++) {
                        scene->cameras[j].image_width = demuxers[j]->GetWidth();
                        scene->cameras[j].image_height = demuxers[j]->GetHeight();
                    }
                    render_allocate_scene_memory(scene, label_buffer_size);
                    // multiple threads for decoding for selected videos
                    for (int i = 0; i < scene->num_cams; i++) {
                        decoder_threads.push_back(std::thread(
                            &decoder_process, dc_context, demuxers[i].get(),
                            camera_names[i], scene->cameras[i].display_buffer,
                            scene->size_of_buffer, &scene->cameras[i].seek_context,
                            scene->use_cpu_buffer));
                        is_view_focused.push_back(false);
                    }
                    video_loaded = true;
                } else {
                    input_is_imgs = true;
                    for (const auto &elem : selected_files) {
                        std::size_t cam_string_position = elem.first.find("_");
                        std::string cam_name =
                            elem.first.substr(0, cam_string_position);
                        std::string file_name =
                            elem.first.substr(cam_string_position + 1);

                        if (std::find(camera_names.begin(), camera_names.end(),
                                      cam_name) == camera_names.end()) {
                            camera_names.push_back(cam_name);
                        }

                        if (std::find(imgs_names.begin(), imgs_names.end(),
                                      file_name) == imgs_names.end()) {
                            imgs_names.push_back(file_name);
                        }
                    }

                    dc_context->seek_interval = 1;
                    scene->num_cams = camera_names.size();
                    scene->cameras.resize(scene->num_cams);
                    for (u32 j = 0; j < scene->num_cams; j++) {
                        std::string file_name = root_dir + "/" +
                                                camera_names[j] + "_" +
                                                imgs_names[0];
                        cv::Mat image = cv::imread(file_name, cv::IMREAD_COLOR);
                        scene->cameras[j].image_width = image.cols;
                        scene->cameras[j].image_height = image.rows;
                    }
                    if (imgs_names.size() < label_buffer_size) {
                        label_buffer_size = imgs_names.size();
                    }
                    render_allocate_scene_memory(scene, label_buffer_size);
                    for (int i = 0; i < scene->num_cams; i++) {
                        decoder_threads.push_back(std::thread(
                            &image_loader, dc_context, imgs_names,
                            scene->cameras[i].display_buffer, scene->size_of_buffer,
                            &scene->cameras[i].seek_context, scene->use_cpu_buffer,
                            camera_names[i], root_dir));
                        is_view_focused.push_back(false);
                    }
                    video_loaded = true;
                }

                media_session_loader.loadCameraCalibrationsForCurrentMedia();
                if (video_loaded && !input_is_imgs) {
                    int initial_frame = std::max(0, ps.to_display_frame_number);
                    double seek_fps = (video_fps > 0.0) ? video_fps : 30.0;
                    seek_all_cameras(scene, initial_frame, seek_fps, ps, true,
                                     &zarr_loader, &stimulus_player);
                }
            }
            // close
            ImGuiFileDialog::Instance()->Close();
        }

        if (ImGuiFileDialog::Instance()->Display("ChooseZarrArchive")) {
            if (ImGuiFileDialog::Instance()->IsOk()) {
                auto selection = ImGuiFileDialog::Instance()->GetSelection();
                std::string selected_zarr_path;
                if (!selection.empty()) {
                    selected_zarr_path = selection.begin()->second;
                } else {
                    selected_zarr_path = ImGuiFileDialog::Instance()->GetCurrentPath();
                }

                std::string zarr_error;
                if (loadZarrDetectionFromPath(selected_zarr_path, zarr_loader, zarr_error)) {
                    zarr_loaded = true;
                    refreshDetectionDatasetOptions(zarr_loader);
                    g_zarr_bbox_edit_state.clearAll();
                    invalidateReviewFrameCache(review_frame_cache);
                    review_frame_status.clear();
                    warmEyeMaskCacheForFrame("zarr_dialog", current_frame_num);
                    prewarmEyeMaskOverlayTexturesForPlayback("zarr_dialog",
                                                             current_frame_num);
                    std::cout << "Loaded Zarr archive override: "
                              << zarr_loader.getArchivePath() << std::endl;
                    media_session_loader.tryAutoLoadAffiliatedVideoFromZarr(
                        "Load Zarr Archive");
                    media_session_loader.tryAutoLoadStimulusVideo(
                        "file-dialog");
                } else {
                    zarr_loaded = false;
                    g_zarr_bbox_edit_state.clearAll();
                    invalidateReviewFrameCache(review_frame_cache);
                    review_frame_status.clear();
                    std::cout << "Failed to load Zarr archive override: " << zarr_error << std::endl;
                    detection_dataset_ids.clear();
                    detection_dataset_labels.clear();
                    detection_dataset_choice = 0;
                }
            }
            ImGuiFileDialog::Instance()->Close();
        }

        if (ImGuiFileDialog::Instance()->Display("ChooseStimulus")) {
            if (ImGuiFileDialog::Instance()->IsOk()) {
                auto selection = ImGuiFileDialog::Instance()->GetSelection();
                if (!selection.empty()) {
                    std::string stimulus_path = selection.begin()->second;
                    int selected_stimulus_buffer_size =
                        std::max(1, stimulus_buffer_size);
                    if (!initializeStimulusPlayback(stimulus_player, stimulus_path,
                                                    selected_stimulus_buffer_size,
                                                    stimulus_use_cpu_buffer,
                                                    stimulus_use_software_decode,
                                                    kCudaDeviceIndex)) {
                        show_error = true;
                        error_message = "Failed to load stimulus video: " + stimulus_path;
                    } else {
                        window_was_decoding[stimulus_player.window_name] = false;
                        window_need_decoding[stimulus_player.window_name].store(false);
                        if (zarr_loaded) {
                            scheduleStimulusSeek(stimulus_player, &zarr_loader,
                                                 ps.to_display_frame_number,
                                                 !ps.play_video);
                        }
                    }
                }
            }
            ImGuiFileDialog::Instance()->Close();
        }

        if (ImGuiFileDialog::Instance()->Display("ChooseSkeleton")) {
            if (ImGuiFileDialog::Instance()->IsOk()) { // action if OK
                auto skeleton_file =
                    ImGuiFileDialog::Instance()->GetSelection();

                if (!skeleton_file.empty()) {

                    bool load_calibration = true;
                    if (scene->num_cams > 1) {
                        for (u32 i = 0; i < scene->num_cams; i++) {
                            std::string cam_file = root_dir + "/calibration/" +
                                                   camera_names[i] + ".yaml";
                            if (!camera_load_params_from_yaml(cam_file, camera_params[i], error_message)) {
                                load_calibration = false;
                                camera_params.clear();
                                camera_params.resize(scene->num_cams);
                                show_error = true;
                                break;
                            }
                        }
                    }

                        if (load_calibration) {
                        skeleton_dir =
                            ImGuiFileDialog::Instance()->GetCurrentPath();
                        skeleton_initialize("", skeleton_file.begin()->second,
                                            legacy_labeling_state.skeleton.get(),
                                            SP_LOAD);
                        legacy_labeling_state.activateManualMode(root_dir);
                    }
                }
            }
            // close
            ImGuiFileDialog::Instance()->Close();
        }

        static int select_corr_head = 0;
        if (video_loaded && (!ps.play_video)) {
            int visible_idx = 0;
            if (!ps.pause_seeked) {
                for (int i = 0; i < scene->num_cams; i++) {
                    if (window_was_decoding[camera_names[i]]) {
                        visible_idx = i;
                        break;
                    }
                }
            }

            struct PausedBufferListItem {
                int slot = -1;
                int frame = -1;
            };
            std::vector<PausedBufferListItem> paused_buffer_items;
            paused_buffer_items.reserve(scene->size_of_buffer);
            for (int i = 0; i < scene->size_of_buffer; ++i) {
                const auto& slot = scene->cameras[visible_idx].display_buffer[i];
                if (slot.available_to_write || slot.frame_number < 0) {
                    continue;
                }
                paused_buffer_items.push_back({i, slot.frame_number});
            }
            std::sort(paused_buffer_items.begin(), paused_buffer_items.end(),
                      [](const PausedBufferListItem& a,
                         const PausedBufferListItem& b) {
                          if (a.frame == b.frame) {
                              return a.slot < b.slot;
                          }
                          return a.frame < b.frame;
                      });

            struct PausedBufferSpan {
                int start_index = -1;
                int end_index = -1;
            };
            std::vector<PausedBufferSpan> paused_buffer_spans;
            paused_buffer_spans.reserve(paused_buffer_items.size());
            for (int i = 0; i < static_cast<int>(paused_buffer_items.size()); ++i) {
                if (paused_buffer_spans.empty() ||
                    paused_buffer_items[i].frame !=
                        paused_buffer_items[paused_buffer_spans.back().end_index]
                            .frame + 1) {
                    paused_buffer_spans.push_back({i, i});
                } else {
                    paused_buffer_spans.back().end_index = i;
                }
            }

            auto getPreferredPausedSlot = [&]() -> int {
                int exact_slot = -1;
                for (int i = 0; i < scene->size_of_buffer; ++i) {
                    const auto& slot = scene->cameras[visible_idx].display_buffer[i];
                    if (!slot.available_to_write &&
                        slot.frame_number == ps.to_display_frame_number) {
                        exact_slot = i;
                        break;
                    }
                }
                if (exact_slot >= 0) {
                    return exact_slot;
                }
                return -1;
            };

            ImGui::SetNextWindowSize(ImVec2(500, 440), ImGuiCond_FirstUseEver);
            const auto buffer_window_ui_start = std::chrono::steady_clock::now();
            if (ImGui::Begin("Frames in the buffer")) {
                ImGui::Text("Valid frames: %zu / %u",
                            paused_buffer_items.size(), scene->size_of_buffer);
                if (ps.paused_frame_on_toggle >= 0) {
                    ImGui::Text("Pause origin frame: %d, resume mode: %s",
                                ps.paused_frame_on_toggle,
                                ps.buffer_browsed_since_pause
                                    ? "buffered resume / camera re-anchor"
                                    : "smooth resume from pause frame");
                }
                if (ps.last_resume_path != ResumePath::None) {
                    ImGui::Text("Last resume: %s (target %d)",
                                resumePathName(ps.last_resume_path),
                                ps.last_resume_target_frame);
                }
                if (!paused_buffer_items.empty()) {
                    const int oldest_buffered_frame =
                        paused_buffer_items.front().frame;
                    const int newest_buffered_frame =
                        paused_buffer_items.back().frame;
                    int largest_gap = 0;
                    for (int span_idx = 1;
                         span_idx < static_cast<int>(paused_buffer_spans.size());
                         ++span_idx) {
                        const auto& previous_last_item =
                            paused_buffer_items[paused_buffer_spans[span_idx - 1]
                                                    .end_index];
                        const auto& current_first_item =
                            paused_buffer_items[paused_buffer_spans[span_idx]
                                                    .start_index];
                        largest_gap = std::max(
                            largest_gap,
                            current_first_item.frame - previous_last_item.frame - 1);
                    }
                    ImGui::Text("Selected/displayed frame: %d",
                                ps.to_display_frame_number);
                    ImGui::Text("Buffered spans: %zu, oldest: %d, newest: %d, largest gap: %d",
                                paused_buffer_spans.size(),
                                oldest_buffered_frame,
                                newest_buffered_frame,
                                largest_gap);
                    ImGui::Text("Newest buffered frame: %d",
                                newest_buffered_frame);
                }
                int selected_item = -1;
                int best_distance = std::numeric_limits<int>::max();
                int best_frame = std::numeric_limits<int>::min();
                for (int i = 0; i < static_cast<int>(paused_buffer_items.size()); ++i) {
                    const auto& item = paused_buffer_items[i];
                    if (item.frame == ps.to_display_frame_number) {
                        selected_item = i;
                        best_distance = 0;
                        best_frame = item.frame;
                        break;
                    }
                    const int distance = std::abs(item.frame - ps.to_display_frame_number);
                    if (distance < best_distance ||
                        (distance == best_distance && item.frame > best_frame)) {
                        best_distance = distance;
                        best_frame = item.frame;
                        selected_item = i;
                    }
                }
                if (paused_buffer_items.empty()) {
                    ImGui::TextDisabled("No decoded frames currently buffered.");
                } else {
                    const int newest_buffered_frame =
                        paused_buffer_items.back().frame;
                    for (int span_idx = 0;
                         span_idx < static_cast<int>(paused_buffer_spans.size());
                         ++span_idx) {
                        const auto& span = paused_buffer_spans[span_idx];
                        const auto& first_item =
                            paused_buffer_items[span.start_index];
                        const auto& last_item =
                            paused_buffer_items[span.end_index];

                        if (span_idx > 0) {
                            const auto& previous_last_item =
                                paused_buffer_items[paused_buffer_spans[span_idx - 1]
                                                        .end_index];
                            const int missing_frames =
                                first_item.frame - previous_last_item.frame - 1;
                            if (missing_frames > 0) {
                                const int missing_start =
                                    previous_last_item.frame + 1;
                                const int missing_end =
                                    first_item.frame - 1;
                                ImGui::Separator();
                                ImGui::TextDisabled(
                                    "Gap: %d missing frames (%d..%d)",
                                    missing_frames, missing_start,
                                    missing_end);
                            }
                        }

                        char span_label[192];
                        snprintf(span_label, sizeof(span_label),
                                 "Span %d-%d (%d frames, slots %d-%d, selected %+d..%+d, newest %+d..%+d)",
                                 first_item.frame, last_item.frame,
                                 span.end_index - span.start_index + 1,
                                 first_item.slot, last_item.slot,
                                 first_item.frame - ps.to_display_frame_number,
                                 last_item.frame - ps.to_display_frame_number,
                                 first_item.frame - newest_buffered_frame,
                                 last_item.frame - newest_buffered_frame);
                        ImGui::TextDisabled("%s", span_label);

                        for (int i = span.start_index; i <= span.end_index; ++i) {
                            const auto& item = paused_buffer_items[i];
                            char label[128];
                            const int selected_delta =
                                item.frame - ps.to_display_frame_number;
                            const int newest_delta =
                                item.frame - newest_buffered_frame;
                            snprintf(label, sizeof(label),
                                     "Frame %d (slot %d, selected %+d, newest %+d)",
                                     item.frame, item.slot, selected_delta,
                                     newest_delta);
                            ImGui::PushID(i);
                            if (ImGui::Selectable(label, selected_item == i)) {
                                const int previous_frame =
                                    ps.to_display_frame_number;
                                selected_item = i;
                                ps.to_display_frame_number = item.frame;
                                ps.slider_frame_number = item.frame;
                                ps.pause_seeked = true;
                                ps.buffer_browsed_since_pause =
                                    (ps.paused_frame_on_toggle >= 0 &&
                                     item.frame != ps.paused_frame_on_toggle);
                                writeClippedPlaybackStateEvent(
                                    "paused_buffer_select",
                                    json{{"previous_frame", previous_frame},
                                         {"target_frame", item.frame},
                                         {"slot", item.slot}},
                                    true);
                            }
                            ImGui::PopID();
                        }
                    }
                }

                if (ImGui::IsKeyPressed(ImGuiKey_Comma, true)) {
                    if (selected_item > 0 &&
                        selected_item <= static_cast<int>(paused_buffer_items.size()) - 1) {
                        const int previous_frame = ps.to_display_frame_number;
                        --selected_item;
                        ps.to_display_frame_number =
                            paused_buffer_items[selected_item].frame;
                        ps.slider_frame_number = ps.to_display_frame_number;
                        ps.pause_seeked = true;
                        ps.buffer_browsed_since_pause =
                            (ps.paused_frame_on_toggle >= 0 &&
                             ps.to_display_frame_number !=
                                 ps.paused_frame_on_toggle);
                        writePlaybackTraceEvent(
                            "buffer_key_step",
                            json{{"key", "comma"},
                                 {"direction", -1},
                                 {"previous_frame", previous_frame},
                                 {"target_frame",
                                  ps.to_display_frame_number}});
                        writeClippedPlaybackStateEvent(
                            "paused_buffer_key_step",
                            json{{"key", "comma"},
                                 {"direction", -1},
                                 {"previous_frame", previous_frame},
                                 {"target_frame", ps.to_display_frame_number}},
                            true);
                    }
                };

                if (ImGui::IsKeyPressed(ImGuiKey_Period, true)) {
                    if (selected_item >= 0 &&
                        selected_item < static_cast<int>(paused_buffer_items.size()) - 1) {
                        const int previous_frame = ps.to_display_frame_number;
                        ++selected_item;
                        ps.to_display_frame_number =
                            paused_buffer_items[selected_item].frame;
                        ps.slider_frame_number = ps.to_display_frame_number;
                        ps.pause_seeked = true;
                        ps.buffer_browsed_since_pause =
                            (ps.paused_frame_on_toggle >= 0 &&
                             ps.to_display_frame_number !=
                                 ps.paused_frame_on_toggle);
                        writePlaybackTraceEvent(
                            "buffer_key_step",
                            json{{"key", "period"},
                                 {"direction", 1},
                                 {"previous_frame", previous_frame},
                                 {"target_frame",
                                  ps.to_display_frame_number}});
                        writeClippedPlaybackStateEvent(
                            "paused_buffer_key_step",
                            json{{"key", "period"},
                                 {"direction", 1},
                                 {"previous_frame", previous_frame},
                                 {"target_frame", ps.to_display_frame_number}},
                            true);
                    }
                };
            }
            ImGui::End();
            frame_buffer_window_ui_ms +=
                durationMs(std::chrono::steady_clock::now() - buffer_window_ui_start);
            select_corr_head = getPreferredPausedSlot();
            if (select_corr_head >= 0) {
                ps.read_head = select_corr_head;
                current_frame_num =
                    scene->cameras[visible_idx].display_buffer[select_corr_head]
                        .frame_number;
                // Keep paused seek target stable unless the user explicitly
                // selects/seeks a different frame.
            } else {
                current_frame_num = std::max(0, ps.to_display_frame_number);
            }
        }

        // Render a video frame
        if (video_loaded) {
            if (ps.play_video && scene->num_cams > 0 && scene->size_of_buffer > 0) {
                int live_frame =
                    scene->cameras[0].display_buffer[ps.read_head % scene->size_of_buffer].frame_number;
                if (live_frame >= 0) {
                    current_frame_num = live_frame;
                } else {
                    current_frame_num = ps.to_display_frame_number;
                }
                maybeRequestClippedBoundaryHandoff(current_frame_num);
            }
            const bool freeze_stimulus_during_paused_browse =
                !ps.play_video && ps.pause_seeked && ps.buffer_browsed_since_pause;
            const bool freeze_stimulus_during_seek =
                seek_progress.state == SeekState::WaitingCameras ||
                seek_progress.state == SeekState::WaitingStimulus;
            if (zarr_loaded && zarr_loader.hasStimulusAlignment()) {
                if (!freeze_stimulus_during_paused_browse &&
                    !freeze_stimulus_during_seek) {
                    int stim_source_frame = ps.play_video ? current_frame_num
                                                          : ps.to_display_frame_number;
                    if (auto stim_frame = zarr_loader.getStimulusFrameForCameraFrame(stim_source_frame)) {
                        ps.current_stimulus_frame = *stim_frame;
                    } else {
                        ps.current_stimulus_frame = -1;
                    }
                }
            } else {
                ps.current_stimulus_frame = -1;
            }
            if (stimulus_player.loaded) {
                const auto stimulus_presentation_result =
                    updateStimulusPlaybackPresentation(
                        StimulusPlaybackPresentationContext{
                            stimulus_player,
                            zarr_loaded ? &zarr_loader : nullptr,
                            ps,
                            seek_progress,
                            current_frame_num,
                            video_fps,
                            latest_decoded_frame[stimulus_player.window_name]
                                .load(),
                            window_need_decoding[stimulus_player.window_name]
                                .load(),
                            &stimulus_catchup_seek_generation,
                        },
                        stimulus_playback_presentation_state);
                window_need_decoding[stimulus_player.window_name].store(
                    stimulus_presentation_result.decoder_requested);
            }
            const int paused_visible_idx =
                ps.play_video ? -1
                              : playback_session_controller
                                    .getVisibleCameraIndex();
            for (int j = 0; j < scene->num_cams; j++) {
                const std::string &win_name = camera_names[j];

                // layout
                ImGui::SetNextWindowSize(ImVec2(500, 400),
                                         ImGuiCond_FirstUseEver);
                ImVec2 window_pos;

                if (scene->num_cams < 8) {
                    if (j % 2 == 0) {
                        window_pos.y = 200.0;
                        window_pos.x = (j / 2.0) * 500;
                    } else {
                        window_pos.y = 600.0;
                        window_pos.x = (j - 1) / 2.0 * 500;
                    }
                } else {
                    int row = j % 4;
                    float base_x = (row == 0) ? j : (j - 1);
                    float x_group = (base_x / 4.0f) * 500;
                    switch (row) {
                    case 0:
                        window_pos = {x_group, 200.0f};
                        break;
                    case 1:
                        window_pos = {x_group, 600.0f};
                        break;
                    case 2:
                        window_pos = {x_group, 1000.0f};
                        break;
                    case 3:
                        window_pos = {x_group, 1400.0f};
                        break;
                    }
                }

                ImGui::SetNextWindowPos(window_pos, ImGuiCond_FirstUseEver);
                bool is_visible = ImGui::Begin(win_name.c_str());

                if (!window_was_decoding[win_name] && is_visible &&
                    ps.play_video) {
                    // seek if visibility has changed
                    playback_session_controller.seekToFrame(current_frame_num,
                                                            true);
                }

                if (!window_was_decoding[win_name] && is_visible &&
                    !ps.play_video && !ps.pause_seeked) {
                    // seek if visibility has changed
                    playback_session_controller.seekToFrame(current_frame_num,
                                                            true);
                    for (auto &[key, value] : window_need_decoding) {
                        value.store(true);
                    }
                }

                if (ps.play_video) {
                    window_need_decoding[win_name].store(is_visible);
                };

                if (is_visible) {
                    const bool prewarm_playback_textures =
                        !ps.play_video && video_loaded &&
                        !yolo_detection &&
                        playbackLightweightRendererIsActive(
                            true, playback_renderer_mode);
                    const bool playback_upload_mode_active =
                        ps.play_video || prewarm_playback_textures;
                    const CameraViewPresenterContext camera_view_presenter_context{
                        scene,
                        j,
                        current_frame_num,
                        ps.play_video ? playback_presenter_target_frame
                                      : ps.to_display_frame_number,
                        ps.read_head,
                        select_corr_head,
                        ps.play_video,
                        ps.pause_seeked,
                        yolo_detection,
                        playbackLightweightRendererIsActive(
                            playback_upload_mode_active,
                            playback_renderer_mode),
                        playbackPreviewIsActive(
                            playback_upload_mode_active, yolo_detection,
                            playback_preview_scale_mode),
                        prewarm_playback_textures,
                        playbackPreviewScaleFactor(
                            playback_preview_scale_mode),
                        playback_preview_scale_mode,
                    };
                    const CameraViewPresenterResult camera_view_presenter_result =
                        presentCameraViewFrame(camera_view_presenter_context);

                    unsigned char *presented_rgba_cuda_buffer =
                        camera_view_presenter_result.presented_rgba_cuda_buffer;
                    bool swap_playback_surface_after_draw =
                        camera_view_presenter_result.swap_playback_surface_after_draw;
                    int presented_slot =
                        camera_view_presenter_result.presented_slot;
                    int presented_frame =
                        camera_view_presenter_result.presented_frame;
                    current_frame_num =
                        camera_view_presenter_result.resolved_current_frame_num;
                    bool playback_surface_swapped_before_draw = false;
                    if (swap_playback_surface_after_draw) {
                        auto& camera = scene->cameras[j];
                        const auto swap_start = std::chrono::steady_clock::now();
                        std::swap(camera.image_texture,
                                  camera.playback_staging_texture);
                        std::swap(camera.pbo_cuda,
                                  camera.playback_staging_pbo);
                        std::swap(camera.applied_preview_sampling_mode,
                                  camera.playback_staging_preview_sampling_mode);
                        const int previous_front_frame =
                            camera.last_uploaded_frame;
                        const int previous_front_local_frame =
                            camera.last_uploaded_local_frame;
                        const int64_t previous_front_pts =
                            camera.last_uploaded_pts;
                        const bool previous_front_valid =
                            camera.texture_has_valid_frame;
                        camera.last_uploaded_frame =
                            camera.playback_staging_frame;
                        camera.last_uploaded_local_frame =
                            camera.playback_staging_local_frame;
                        camera.last_uploaded_pts =
                            camera.playback_staging_pts;
                        camera.texture_has_valid_frame =
                            camera.playback_staging_valid;
                        camera.playback_staging_frame =
                            previous_front_frame;
                        camera.playback_staging_local_frame =
                            previous_front_local_frame;
                        camera.playback_staging_pts =
                            previous_front_pts;
                        camera.playback_staging_valid =
                            previous_front_valid;
                        frame_camera_playback_swap_ms += durationMs(
                            std::chrono::steady_clock::now() - swap_start);
                        playback_surface_swapped_before_draw = true;
                        swap_playback_surface_after_draw = false;
                        presented_frame = camera.last_uploaded_frame;
                        if (presented_frame >= 0) {
                            current_frame_num = presented_frame;
                        }
                        presented_rgba_cuda_buffer =
                            camera.pbo_cuda.cuda_buffer;
                    }
                    playback_trace_presenter_view_idx = j;
                    playback_trace_presenter_target_frame =
                        camera_view_presenter_context.target_display_frame;
                    playback_trace_presenter_preferred_paused_slot =
                        camera_view_presenter_context.preferred_paused_slot;
                    playback_trace_presented_slot = presented_slot;
                    playback_trace_presented_frame = presented_frame;
                    playback_trace_presenter_resolved_frame =
                        camera_view_presenter_result.resolved_current_frame_num;
                    playback_trace_prewarm_active = prewarm_playback_textures;

                    frame_camera_upload_ms +=
                        camera_view_presenter_result.perf.upload_ms;
                    frame_camera_upload_count +=
                        camera_view_presenter_result.perf.upload_count;
                    frame_camera_texture_resize_ms +=
                        camera_view_presenter_result.perf.texture_resize_ms;
                    frame_camera_preview_resize_ms +=
                        camera_view_presenter_result.perf.preview_resize_ms;
                    frame_camera_display_convert_ms +=
                        camera_view_presenter_result.perf.display_convert_ms;
                    frame_camera_pbo_copy_ms +=
                        camera_view_presenter_result.perf.pbo_copy_ms;
                    frame_camera_texture_upload_ms +=
                        camera_view_presenter_result.perf.texture_upload_ms;
                    frame_camera_playback_front_path_ms +=
                        camera_view_presenter_result.perf.playback_front_path_ms;
                    frame_camera_playback_stage_total_ms +=
                        camera_view_presenter_result.perf.playback_stage_total_ms;
                    frame_camera_playback_stage_upload_ms +=
                        camera_view_presenter_result.perf.playback_stage_upload_ms;
                    frame_camera_playback_prewarm_total_ms +=
                        camera_view_presenter_result
                            .perf.playback_prewarm_total_ms;
                    frame_camera_playback_prewarm_upload_ms +=
                        camera_view_presenter_result
                            .perf.playback_prewarm_upload_ms;
                    frame_camera_playback_prewarm_count +=
                        camera_view_presenter_result
                            .perf.playback_prewarm_count;

                    // sync yolo detection
                    if (yolo_detection) {
                        std::unique_lock<std::mutex> lck(g_mutexes[j]);
                        // std::cout << "main_thread: acquire lock" <<
                        // std::endl;
                        yolo_input_frames_rgba[j] = presented_rgba_cuda_buffer;
                        g_ready[j] = true;
                        g_cvs[j].notify_one();
                    }

                    ZarrDetectionLoader::FrameDetections detection_details;
                    const bool has_presented_camera_frame =
                        presented_frame >= 0;
                    int zarr_bbox_query_frame =
                        has_presented_camera_frame ? presented_frame
                                                   : current_frame_num;
                    if (zarr_loaded && zarr_loader.hasClippedCollection() &&
                        clipped_media_state.switch_in_progress &&
                        clipped_media_state.pending_switch_parent_frame >= 0 &&
                        clipped_media_state.last_presented_parent_frame >= 0) {
                        const bool presented_new_clip_frame =
                            has_presented_camera_frame &&
                            presented_frame >=
                                clipped_media_state
                                    .pending_switch_parent_frame &&
                            clippedSelectedRunForFrame(presented_frame) ==
                                clipped_media_state.selected_run_index;
                        if (!presented_new_clip_frame) {
                            zarr_bbox_query_frame = static_cast<int>(
                                clipped_media_state
                                    .last_presented_parent_frame);
                        }
                    }
                    const bool camera_subject_shape_needs_contours =
                        subject_shape_overlay_options.show_overlay &&
                        (subject_shape_overlay_options.show_body_contour ||
                         subject_shape_overlay_options
                             .show_swim_bladder_contour ||
                         subject_shape_overlay_options.show_eye_contours);
                    const bool camera_details_include_subject_shapes =
                        zarr_loaded &&
                        has_presented_camera_frame &&
                        zarr_loader.hasSubjectShapeData() &&
                        (subject_shape_overlay_options.show_overlay ||
                         (zarr_loader.hasTailKinematicsData() &&
                          tail_kinematics_overlay_options.show_overlay) ||
                         (show_eye_masks && show_eye_angle_arcs &&
                          zarr_loader.hasEyeAngleData()));
                    const bool camera_details_include_eye_masks =
                        zarr_loaded &&
                        has_presented_camera_frame &&
                        (show_eye_masks || camera_subject_shape_needs_contours) &&
                        zarr_loader.hasEyeMasks();
                    const bool is_zarr_interpolated =
                        zarr_loaded && has_presented_camera_frame &&
                        zarr_loader.hasInterpolation() &&
                        zarr_loader.isFrameInterpolated(zarr_bbox_query_frame);
                    const bool active_dataset_is_raw_detect =
                        zarr_loaded && zarr_loader.hasDetectionData() &&
                        (zarr_loader.getActiveDetectionDataset() ==
                         ZarrDetectionLoader::DetectionDataset::RawDetect);
                    const bool dataset_allows_bbox_edit =
                        zarr_loaded && has_presented_camera_frame &&
                        zarr_loader.hasDetectionData() && !active_dataset_is_raw_detect &&
                        !zarr_loader.hasClippedCollection();
                    if (zarr_loaded && !dataset_allows_bbox_edit) {
                        g_zarr_bbox_edit_state.draw_mode = false;
                        g_zarr_bbox_edit_state.cancelDraw();
                        g_zarr_bbox_edit_state.clearSelection();
                    }
                    const bool can_modify_boxes =
                        dataset_allows_bbox_edit &&
                        g_zarr_bbox_edit_state.enabled &&
                        (g_zarr_bbox_edit_state.allow_edit_while_playing ||
                         !ps.play_video);
                    std::vector<LoggedBoundingBox> loaded_zarr_boxes;
                    std::vector<LoggedBoundingBox> zarr_boxes;
                    if (zarr_loaded && has_presented_camera_frame) {
                        frame_bbox_query_frame = zarr_bbox_query_frame;
                        const auto bbox_load_total_start =
                            std::chrono::steady_clock::now();
                        const auto bbox_get_boxes_start =
                            std::chrono::steady_clock::now();
                        loaded_zarr_boxes =
                            zarr_loader.getBoundingBoxesForFrame(
                                zarr_bbox_query_frame);
                        frame_bbox_get_boxes_ms += durationMs(
                            std::chrono::steady_clock::now() -
                            bbox_get_boxes_start);
                        frame_bbox_loaded_count =
                            static_cast<int>(loaded_zarr_boxes.size());
                        const auto bbox_edit_resolve_start =
                            std::chrono::steady_clock::now();
                        zarr_boxes = g_zarr_bbox_edit_state.resolveFrameBoxes(
                            zarr_bbox_query_frame, loaded_zarr_boxes);
                        frame_bbox_edit_resolve_ms += durationMs(
                            std::chrono::steady_clock::now() -
                            bbox_edit_resolve_start);
                        frame_bbox_display_count =
                            static_cast<int>(zarr_boxes.size());
                        const auto detection_load_start =
                            std::chrono::steady_clock::now();
                        detection_details = zarr_loader.getRawDetections(
                            zarr_bbox_query_frame,
                            false,
                            camera_details_include_eye_masks,
                            camera_details_include_subject_shapes);
                        const double raw_detections_ms = durationMs(
                            std::chrono::steady_clock::now() -
                            detection_load_start);
                        frame_bbox_get_raw_detections_ms +=
                            raw_detections_ms;
                        if (camera_details_include_eye_masks) {
                            frame_mask_data_load_ms += raw_detections_ms;
                        }
                        frame_bbox_load_total_ms += durationMs(
                            std::chrono::steady_clock::now() -
                            bbox_load_total_start);
                    }

                    auto deleteSelectedBoxOnCurrentFrame = [&]() -> bool {
                        if (!can_modify_boxes) {
                            return false;
                        }
                        if (g_zarr_bbox_edit_state.selected_frame != current_frame_num ||
                            g_zarr_bbox_edit_state.selected_box < 0) {
                            return false;
                        }
                        auto& editable_boxes =
                            g_zarr_bbox_edit_state.ensureFrameOverride(
                                current_frame_num, loaded_zarr_boxes,
                                &detection_details);
                        auto& added_flags =
                            g_zarr_bbox_edit_state.ensureAddedFlags(
                                current_frame_num, editable_boxes.size());
                        auto& manual_flags =
                            g_zarr_bbox_edit_state.ensureManualFlags(
                                current_frame_num, editable_boxes.size());
                        g_zarr_bbox_edit_state.ensureSourceMetadata(
                            current_frame_num, editable_boxes.size());
                        auto& source_indices =
                            g_zarr_bbox_edit_state
                                .frame_source_indices[current_frame_num];
                        auto& source_detection_source =
                            g_zarr_bbox_edit_state
                                .frame_source_detection_source[current_frame_num];
                        auto& source_reason =
                            g_zarr_bbox_edit_state
                                .frame_source_reason[current_frame_num];
                        const int selected_idx =
                            g_zarr_bbox_edit_state.selected_box;
                        if (selected_idx < 0 ||
                            selected_idx >=
                                static_cast<int>(editable_boxes.size())) {
                            g_zarr_bbox_edit_state.clearSelection();
                            return false;
                        }
                        editable_boxes.erase(editable_boxes.begin() +
                                             selected_idx);
                        if (selected_idx <
                            static_cast<int>(added_flags.size())) {
                            added_flags.erase(added_flags.begin() +
                                              selected_idx);
                        } else {
                            added_flags.assign(editable_boxes.size(), 0);
                        }
                        if (selected_idx <
                            static_cast<int>(manual_flags.size())) {
                            manual_flags.erase(manual_flags.begin() +
                                               selected_idx);
                        } else {
                            manual_flags.assign(editable_boxes.size(), 0);
                        }
                        if (selected_idx <
                            static_cast<int>(source_indices.size())) {
                            source_indices.erase(source_indices.begin() +
                                                 selected_idx);
                        } else {
                            source_indices.assign(editable_boxes.size(), -1);
                        }
                        if (selected_idx < static_cast<int>(
                                               source_detection_source.size())) {
                            source_detection_source.erase(
                                source_detection_source.begin() + selected_idx);
                        } else {
                            source_detection_source.assign(editable_boxes.size(),
                                                          0);
                        }
                        if (selected_idx <
                            static_cast<int>(source_reason.size())) {
                            source_reason.erase(source_reason.begin() +
                                                selected_idx);
                        } else {
                            source_reason.assign(editable_boxes.size(),
                                                 std::string{});
                        }
                        g_zarr_bbox_edit_state.dirty_frames.insert(
                            current_frame_num);
                        g_zarr_bbox_edit_state.drag_active = false;
                        g_zarr_bbox_edit_state.drag_mouse_button = -1;
                        if (editable_boxes.empty()) {
                            g_zarr_bbox_edit_state.clearSelection();
                        } else {
                            g_zarr_bbox_edit_state.selected_frame =
                                current_frame_num;
                            g_zarr_bbox_edit_state.selected_box = std::min(
                                selected_idx,
                                static_cast<int>(editable_boxes.size() - 1));
                        }
                        zarr_boxes = editable_boxes;
                        return true;
                    };

                    FullFrameRectEditStateView full_frame_edit_state;
                    full_frame_edit_state.selected_frame =
                        g_zarr_bbox_edit_state.selected_frame;
                    full_frame_edit_state.selected_box =
                        g_zarr_bbox_edit_state.selected_box;
                    full_frame_edit_state.drag_active =
                        g_zarr_bbox_edit_state.drag_active;
                    full_frame_edit_state.drag_mouse_button =
                        g_zarr_bbox_edit_state.drag_mouse_button;
                    full_frame_edit_state.drag_offset_x =
                        g_zarr_bbox_edit_state.drag_offset_x;
                    full_frame_edit_state.drag_offset_y =
                        g_zarr_bbox_edit_state.drag_offset_y;
                    full_frame_edit_state.draw_mode =
                        g_zarr_bbox_edit_state.draw_mode;
                    full_frame_edit_state.draw_active =
                        g_zarr_bbox_edit_state.draw_active;
                    full_frame_edit_state.draw_frame =
                        g_zarr_bbox_edit_state.draw_frame;
                    full_frame_edit_state.draw_anchor_x =
                        g_zarr_bbox_edit_state.draw_anchor_x;
                    full_frame_edit_state.draw_anchor_y =
                        g_zarr_bbox_edit_state.draw_anchor_y;
                    full_frame_edit_state.draw_current_x =
                        g_zarr_bbox_edit_state.draw_current_x;
                    full_frame_edit_state.draw_current_y =
                        g_zarr_bbox_edit_state.draw_current_y;

                    std::vector<ZarrDetectionLoader::ChaserBoundingBox>
                        chaser_bboxes;
                    std::vector<ZarrDetectionLoader::ChaserState> chaser_states;
                    if (zarr_loaded) {
                        chaser_bboxes =
                            zarr_loader.getChaserBoundingBoxesForFrame(
                                current_frame_num);
                        chaser_states =
                            zarr_loader.getChaserInterpolatedStatesForCameraFrame(
                                current_frame_num);
                        if (chaser_states.empty() &&
                            ps.current_stimulus_frame >= 0 &&
                            zarr_loader.hasStimulusFrameMapping()) {
                            chaser_states =
                                zarr_loader.getChaserStatesForStimulusFrame(
                                    ps.current_stimulus_frame);
                        }
                        if (chaser_states.empty()) {
                            chaser_states =
                                zarr_loader.getChaserStatesForFrame(
                                    current_frame_num);
                        }
#if defined(CRIMSON_CHASER_DEBUG_LOGS)
                        static bool debug_printed = false;
                        static int frames_with_data = 0;
                        if (!chaser_bboxes.empty() || !chaser_states.empty()) {
                            frames_with_data++;
                            if (!debug_printed) {
                                std::cout << "\n=== CHASER DATA DEBUG ==="
                                          << std::endl;
                                std::cout
                                    << "Camera frame " << current_frame_num
                                    << ": Found " << chaser_bboxes.size()
                                    << " chaser bboxes, "
                                    << chaser_states.size()
                                    << " chaser states" << std::endl;
                                if (!chaser_bboxes.empty()) {
                                    std::cout
                                        << "  First bbox: fish_id="
                                        << chaser_bboxes[0].fish_id
                                        << ", x=" << chaser_bboxes[0].x_px
                                        << ", y=" << chaser_bboxes[0].y_px
                                        << ", w="
                                        << chaser_bboxes[0].width_px
                                        << ", h="
                                        << chaser_bboxes[0].height_px
                                        << std::endl;
                                }
                                if (!chaser_states.empty()) {
                                    std::cout
                                        << "  First state: stimulus_frame="
                                        << chaser_states[0].stimulus_frame_num
                                        << ", camera_frame="
                                        << chaser_states[0].camera_frame_id
                                        << std::endl;
                                    std::cout
                                        << "    chaser=("
                                        << chaser_states[0].chaser_pos_x
                                        << ","
                                        << chaser_states[0].chaser_pos_y
                                        << ") target=("
                                        << chaser_states[0].target_pos_x
                                        << ","
                                        << chaser_states[0].target_pos_y
                                        << ")" << std::endl;
                                    std::cout
                                        << "  Camera params: has_homography="
                                        << camera_params[j].has_valid_homography
                                        << ", offsetX="
                                        << camera_params[j].stimulus_offset_x
                                        << ", offsetY="
                                        << camera_params[j].stimulus_offset_y
                                        << std::endl;
                                }
                                debug_printed = true;
                            }
                        }
                        static int last_frame_checked = -1;
                        if (current_frame_num > last_frame_checked + 1000) {
                            std::cout << "Frames " << (last_frame_checked + 1)
                                      << "-" << current_frame_num << ": "
                                      << frames_with_data
                                      << " frames had chaser data"
                                      << std::endl;
                            frames_with_data = 0;
                            last_frame_checked = current_frame_num;
                        }
#endif
                    }

                    const bool heading_overlay_enabled = show_heading_arrows;
                    const bool heading_data_available =
                        zarr_loaded && zarr_loader.hasHeadingData();
                    const bool eye_mask_overlay_enabled = show_eye_masks;
                    const bool eye_mask_data_available =
                        zarr_loaded && zarr_loader.hasEyeMasks();
                    const bool can_draw_headings =
                        heading_overlay_enabled && heading_data_available;
                    const bool can_draw_eye_masks =
                        eye_mask_overlay_enabled && eye_mask_data_available;

                    if (kHeadingDebugLoggingEnabled) {
                        if (!heading_overlay_enabled) {
                            if (!heading_debug_logged_toggle_disabled) {
                                headingDebugLog("Heading overlay disabled via UI toggle; skipping arrow drawing.");
                                heading_debug_logged_toggle_disabled = true;
                            }
                        } else {
                            if (heading_debug_logged_toggle_disabled) {
                                headingDebugLog("Heading overlay toggle enabled; attempting to draw arrows.");
                                heading_debug_logged_toggle_disabled = false;
                            }
                            if (!heading_data_available) {
                                if (!heading_debug_logged_no_data) {
                                    headingDebugLog("Zarr loader reports no heading data; arrows will not be drawn.");
                                    heading_debug_logged_no_data = true;
                                }
                            } else if (heading_debug_logged_no_data) {
                                headingDebugLog("Heading data detected; resuming arrow attempts.");
                                heading_debug_logged_no_data = false;
                            }
                            if (zarr_loaded &&
                                zarr_loader.activeDatasetHasSyntheticDetections()) {
                                if (!heading_debug_logged_interpolated) {
                                    headingDebugLog("Dataset contains synthetic detections; headings render only for real boxes.");
                                    heading_debug_logged_interpolated = true;
                                }
                            } else if (heading_debug_logged_interpolated) {
                                headingDebugLog("Dataset now fully real; headings may render for all boxes.");
                                heading_debug_logged_interpolated = false;
                            }
                        }
                    }

                    if (kEyeMaskDebugLoggingEnabled) {
                        if (!show_eye_masks) {
                            if (!eye_mask_debug_logged_toggle_disabled) {
                                eyeMaskDebugLog("Eye mask overlay disabled via UI toggle; skipping mask drawing.");
                                eye_mask_debug_logged_toggle_disabled = true;
                            }
                        } else if (eye_mask_debug_logged_toggle_disabled) {
                            eyeMaskDebugLog("Eye mask overlay toggle enabled; attempting to draw masks.");
                            eye_mask_debug_logged_toggle_disabled = false;
                        }
                        if (!(zarr_loaded && zarr_loader.hasEyeMasks())) {
                            if (!eye_mask_debug_logged_no_data) {
                                eyeMaskDebugLog("Zarr loader reports no eye mask data.");
                                eye_mask_debug_logged_no_data = true;
                            }
                        } else if (eye_mask_debug_logged_no_data) {
                            eyeMaskDebugLog("Eye mask data detected; masks may render.");
                            eye_mask_debug_logged_no_data = false;
                        }
                    }

                    int latest_decoded = -1;
                    const auto latest_it = latest_decoded_frame.find(win_name);
                    if (latest_it != latest_decoded_frame.end()) {
                        latest_decoded = latest_it->second.load();
                    }
                    int total_recording_frames = -1;
                    if (dc_context->estimated_num_frames > 0) {
                        total_recording_frames = dc_context->estimated_num_frames;
                    }
                    if (dc_context->total_num_frame > 0 &&
                        dc_context->total_num_frame !=
                            std::numeric_limits<int>::max()) {
                        total_recording_frames = std::max(
                            total_recording_frames,
                            dc_context->total_num_frame);
                    }

                    CameraViewFrameContextInput camera_context_input;
                    camera_context_input.scene = scene;
                    camera_context_input.view_idx = j;
                    camera_context_input.camera_name = win_name;
                    camera_context_input.current_frame_num = current_frame_num;
                    camera_context_input.presented_slot = presented_slot;
                    camera_context_input.presented_frame = presented_frame;
                    camera_context_input.swap_playback_surface_after_draw =
                        swap_playback_surface_after_draw;
                    camera_context_input.play_video = ps.play_video;
                    camera_context_input.lightweight_playback_renderer_active =
                        playbackLightweightRendererIsActive(
                            ps.play_video, playback_renderer_mode);
                    camera_context_input.use_legacy_manual_keypoint_tools =
                        use_legacy_manual_keypoint_tools;
                    camera_context_input.legacy_labeling_state =
                        &legacy_labeling_state;
                    camera_context_input.zarr_loaded = zarr_loaded;
                    camera_context_input.zarr_loader = &zarr_loader;
                    camera_context_input.dataset_allows_bbox_edit =
                        dataset_allows_bbox_edit;
                    camera_context_input.bbox_edit_enabled =
                        g_zarr_bbox_edit_state.enabled;
                    camera_context_input.bbox_allow_edit_while_playing =
                        g_zarr_bbox_edit_state.allow_edit_while_playing;
                    camera_context_input.bbox_edit_state =
                        &g_zarr_bbox_edit_state;
                    camera_context_input.full_frame_edit_state =
                        full_frame_edit_state;
                    camera_context_input.zarr_boxes = &zarr_boxes;
                    camera_context_input.detection_details =
                        zarr_loaded && has_presented_camera_frame
                            ? &detection_details
                            : nullptr;
                    camera_context_input.frame_is_interpolated =
                        is_zarr_interpolated;
                    camera_context_input.latest_decoded_frame = latest_decoded;
                    camera_context_input.total_recording_frames =
                        total_recording_frames;
                    camera_context_input.has_yolo_detections = yolo_detection;
                    camera_context_input.yolo_boxes =
                        yolo_detection ? &yolo_boxes.at(j) : nullptr;
                    camera_context_input.yolo_labels =
                        yolo_detection ? &yolo_labels.at(j) : nullptr;
                    camera_context_input.yolo_class_ids =
                        yolo_detection ? &yolo_classid.at(j) : nullptr;
                    camera_context_input.show_keypoint_markers =
                        show_keypoint_markers;
                    camera_context_input.keypoint_tab_full_frame_edit_enabled =
                        keypoint_tab_full_frame_edit_enabled;
                    camera_context_input.visible_camera_index =
                        playback_session_controller.getVisibleCameraIndex();
                    camera_context_input.active_full_frame_keypoint_selection =
                        &active_full_frame_keypoint_selection;
                    camera_context_input.frame_debug_state =
                        &frame_debug_window_state;
                    camera_context_input.subject_mask_edit_session =
                        &frame_debug_window_state.subject_mask_edit_session;
                    camera_context_input.subject_mask_brush_state =
                        &frame_debug_window_state.subject_mask_brush;
                    camera_context_input.can_draw_headings = can_draw_headings;
                    camera_context_input.can_draw_eye_masks =
                        can_draw_eye_masks;
                    camera_context_input.show_subject_body_mask =
                        show_subject_body_mask;
                    camera_context_input.show_eye_left_mask =
                        show_eye_left_mask;
                    camera_context_input.show_eye_right_mask =
                        show_eye_right_mask;
                    camera_context_input.show_swim_bladder_mask =
                        show_swim_bladder_mask;
                    camera_context_input.show_eye_direction_beams =
                        show_eye_direction_beams;
                    camera_context_input.show_eye_gaze_rays =
                        show_eye_gaze_rays;
                    camera_context_input.show_eye_angle_arcs =
                        show_eye_angle_arcs;
                    camera_context_input.show_eye_angle_labels =
                        show_eye_angle_labels;
                    camera_context_input.mask_overlay_mode = mask_overlay_mode;
                    camera_context_input.subject_shape_overlay_options =
                        subject_shape_overlay_options;
                    camera_context_input.tail_kinematics_overlay_options =
                        tail_kinematics_overlay_options;
                    camera_context_input.show_movement_trail =
                        show_movement_trail;
                    camera_context_input.movement_trail_seconds =
                        movement_trail_seconds;
                    camera_context_input.movement_trail_valid_samples_only =
                        movement_trail_valid_samples_only;
                    camera_context_input.stimulus_inset_options =
                        stimulus_inset_options;
                    camera_context_input.stimulus_player =
                        stimulus_player.loaded ? &stimulus_player : nullptr;
                    camera_context_input.target_stimulus_frame =
                        ps.current_stimulus_frame;
                    camera_context_input.chaser_bboxes = &chaser_bboxes;
                    camera_context_input.chaser_states = &chaser_states;
                    camera_context_input.camera_params = &camera_params[j];
                    camera_context_input.transport_controls =
                        CameraViewTransportControlsContext{
                            ps.to_display_frame_number,
                            dc_context->total_num_frame,
                            dc_context->estimated_num_frames,
                            video_fps,
                            ps.play_video,
                            ps.slider_frame_number,
                        };
                    camera_context_input.capture_texture_draw_trace =
                        clipped_frame_trace_log_writer.enabled() &&
                        zarr_loaded &&
                        zarr_loader.hasClippedCollection();
                    PreparedCameraViewFrameContext prepared_camera_context;
                    prepareCameraViewFrameContext(camera_context_input,
                                                  prepared_camera_context);
                    frame_mask_data_load_ms +=
                        prepared_camera_context.mask_data_load_ms;

                    const auto& camera_before_draw = scene->cameras[j];
                    const int frame_sync_front_frame_before_draw =
                        camera_before_draw.last_uploaded_frame;
                    const int frame_sync_front_local_before_draw =
                        camera_before_draw.last_uploaded_local_frame;
                    const int64_t frame_sync_front_pts_before_draw =
                        camera_before_draw.last_uploaded_pts;
                    const bool frame_sync_front_valid_before_draw =
                        camera_before_draw.texture_has_valid_frame;
                    const int frame_sync_staging_frame_before_draw =
                        camera_before_draw.playback_staging_frame;
                    const int frame_sync_staging_local_before_draw =
                        camera_before_draw.playback_staging_local_frame;
                    const int64_t frame_sync_staging_pts_before_draw =
                        camera_before_draw.playback_staging_pts;
                    const bool frame_sync_staging_valid_before_draw =
                        camera_before_draw.playback_staging_valid;

                    const CameraViewWindowResult camera_view_result =
                        drawCameraViewWindowContents(
                            prepared_camera_context.context);
                    accumulateCameraViewMaskPerfMetrics(
                        frame_mask_overlay_perf,
                        camera_view_result.perf.mask_overlay);

                    const auto& camera_after_draw = scene->cameras[j];
                    const int frame_sync_front_frame_after_draw =
                        camera_after_draw.last_uploaded_frame;
                    const int frame_sync_front_local_after_draw =
                        camera_after_draw.last_uploaded_local_frame;
                    const int64_t frame_sync_front_pts_after_draw =
                        camera_after_draw.last_uploaded_pts;
                    const bool frame_sync_front_valid_after_draw =
                        camera_after_draw.texture_has_valid_frame;
                    const int frame_sync_staging_frame_after_draw =
                        camera_after_draw.playback_staging_frame;
                    const int frame_sync_staging_local_after_draw =
                        camera_after_draw.playback_staging_local_frame;
                    const int64_t frame_sync_staging_pts_after_draw =
                        camera_after_draw.playback_staging_pts;
                    const bool frame_sync_staging_valid_after_draw =
                        camera_after_draw.playback_staging_valid;

                    if (frame_sync_trace_log_writer.enabled()) {
                        const int zarr_box_count =
                            static_cast<int>(zarr_boxes.size());
                        FrameSyncTraceLastState& last_trace_state =
                            frame_sync_trace_last_by_camera[win_name];
                        const bool trace_state_changed =
                            !last_trace_state.initialized ||
                            last_trace_state.has_presented_frame !=
                                has_presented_camera_frame ||
                            last_trace_state.target_frame !=
                                camera_view_presenter_context
                                    .target_display_frame ||
                            last_trace_state.presented_frame !=
                                presented_frame ||
                            last_trace_state.bbox_query_frame !=
                                zarr_bbox_query_frame ||
                            last_trace_state.latest_decoded_frame !=
                                latest_decoded ||
                            last_trace_state.front_frame_before_draw !=
                                frame_sync_front_frame_before_draw ||
                            last_trace_state.front_frame_after_draw !=
                                frame_sync_front_frame_after_draw ||
                            last_trace_state.staging_frame_before_draw !=
                                frame_sync_staging_frame_before_draw ||
                            last_trace_state.staging_frame_after_draw !=
                                frame_sync_staging_frame_after_draw ||
                            last_trace_state.zarr_box_count != zarr_box_count;

                        if (trace_state_changed) {
                            auto makeTextureState =
                                [](bool front_valid,
                                   int front_frame,
                                   bool staging_valid,
                                   int staging_frame) {
                                    return json{
                                        {"front_valid", front_valid},
                                        {"front_frame", front_frame},
                                        {"staging_valid", staging_valid},
                                        {"staging_frame", staging_frame},
                                    };
                                };
                            auto makeBboxSummary =
                                [](const std::vector<LoggedBoundingBox>& boxes) {
                                    json summary = {
                                        {"count",
                                         static_cast<int>(boxes.size())},
                                    };
                                    if (!boxes.empty()) {
                                        const LoggedBoundingBox& box =
                                            boxes.front();
                                        summary["first"] = {
                                            {"payload_frame_id",
                                             box.payload_frame_id},
                                            {"payload_camera_id",
                                             box.payload_camera_id},
                                            {"box_index_in_payload",
                                             static_cast<int>(
                                                 box.box_index_in_payload)},
                                            {"x", box.x_min},
                                            {"y", box.y_min},
                                            {"w", box.width},
                                            {"h", box.height},
                                            {"cx",
                                             box.x_min + box.width * 0.5f},
                                            {"cy",
                                             box.y_min + box.height * 0.5f},
                                            {"class_id", box.class_id},
                                            {"confidence", box.confidence},
                                        };
                                    }
                                    return summary;
                                };
                            auto makeClippedMapping =
                                [&](int frame) -> json {
                                    if (!zarr_loaded ||
                                        !zarr_loader
                                             .hasClippedCollection() ||
                                        frame < 0) {
                                        return nullptr;
                                    }
                                    const auto* row =
                                        zarr_loader.resolveClippedFrame(
                                            static_cast<int64_t>(frame));
                                    if (row == nullptr) {
                                        return nullptr;
                                    }
                                    json mapping = {
                                        {"parent_frame_index",
                                         row->parent_frame_index},
                                        {"recording_frame_id",
                                         row->recording_frame_id},
                                        {"clip_id", row->clip_id},
                                        {"clip_local_frame_index",
                                         row->clip_local_frame_index},
                                        {"camera_serial", row->camera_serial},
                                        {"selected_run_index",
                                         row->selected_run_index},
                                    };
                                    const auto* selected_run =
                                        zarr_loader.getClippedResolver()
                                            .selectedRun(
                                                row->selected_run_index);
                                    if (selected_run != nullptr) {
                                        mapping["work_unit_id"] =
                                            selected_run->work_unit_id;
                                        mapping["detect_run"] =
                                            selected_run->detect_run;
                                        mapping["refined_detect_run"] =
                                            selected_run
                                                ->refined_detect_run;
                                    }
                                    return mapping;
                                };

                            json details = {
                                {"camera_name", win_name},
                                {"view_idx", j},
                                {"has_presented_camera_frame",
                                 has_presented_camera_frame},
                                {"target_frame",
                                 camera_view_presenter_context
                                     .target_display_frame},
                                {"current_frame_after_presenter",
                                 current_frame_num},
                                {"presented_slot", presented_slot},
                                {"presented_frame", presented_frame},
                                {"presenter_resolved_frame",
                                 camera_view_presenter_result
                                     .resolved_current_frame_num},
                                {"playback_request",
                                 {{"requested_frame",
                                   playback_requested_camera_frame},
                                  {"presenter_target_frame",
                                   playback_presenter_target_frame},
                                  {"presenter_target_slot",
                                   playback_presenter_target_slot},
                                  {"target_clamped_to_buffer",
                                   playback_target_clamped_to_buffer},
                                  {"committed_frame_before_present",
                                   ps.to_display_frame_number}}},
                                {"swap_playback_surface_after_draw",
                                 swap_playback_surface_after_draw},
                                {"playback_surface_swapped_before_draw",
                                 playback_surface_swapped_before_draw},
                                {"bbox_query_frame", zarr_bbox_query_frame},
                                {"bbox", makeBboxSummary(zarr_boxes)},
                                {"loaded_bbox",
                                 makeBboxSummary(loaded_zarr_boxes)},
                                {"detection_details_frame_id", nullptr},
                                {"texture_before_draw",
                                 makeTextureState(
                                     frame_sync_front_valid_before_draw,
                                     frame_sync_front_frame_before_draw,
                                     frame_sync_staging_valid_before_draw,
                                     frame_sync_staging_frame_before_draw)},
                                {"texture_after_draw",
                                 makeTextureState(
                                     frame_sync_front_valid_after_draw,
                                     frame_sync_front_frame_after_draw,
                                     frame_sync_staging_valid_after_draw,
                                     frame_sync_staging_frame_after_draw)},
                                {"frame_sync_summary",
                                 {{"valid_slots",
                                   camera_view_result.frame_sync.valid_slots},
                                  {"empty_slots",
                                   camera_view_result.frame_sync.empty_slots},
                                  {"latest_decoded",
                                   camera_view_result
                                       .frame_sync.latest_decoded},
                                  {"recording_remaining",
                                   camera_view_result
                                       .frame_sync.recording_remaining},
                                  {"recording_total",
                                   camera_view_result
                                       .frame_sync.recording_total},
                                  {"debug_line",
                                   camera_view_result
                                       .frame_sync.debug_line}}},
                                {"clipped_mapping",
                                 {{"target",
                                   makeClippedMapping(
                                       camera_view_presenter_context
                                           .target_display_frame)},
                                  {"presented",
                                   makeClippedMapping(presented_frame)},
                                  {"bbox_query",
                                   makeClippedMapping(
                                       zarr_bbox_query_frame)},
                                  {"front_before_draw",
                                   makeClippedMapping(
                                       frame_sync_front_frame_before_draw)},
                                  {"front_after_draw",
                                   makeClippedMapping(
                                       frame_sync_front_frame_after_draw)}}},
                                {"clipped_state", clippedStateJson()},
                            };
                            details["bbox_frame_delta_from_presented"] =
                                has_presented_camera_frame
                                    ? json(zarr_bbox_query_frame -
                                           presented_frame)
                                    : json(nullptr);
                            details["detection_details_frame_id"] =
                                (zarr_loaded && has_presented_camera_frame)
                                    ? json(detection_details.frame_id)
                                    : json(nullptr);
                            details["bbox_matches_presented_frame"] =
                                has_presented_camera_frame &&
                                zarr_bbox_query_frame == presented_frame;
                            details["bbox_matches_front_before_draw"] =
                                frame_sync_front_valid_before_draw &&
                                zarr_bbox_query_frame ==
                                    frame_sync_front_frame_before_draw;
                            details["bbox_matches_front_after_draw"] =
                                frame_sync_front_valid_after_draw &&
                                zarr_bbox_query_frame ==
                                    frame_sync_front_frame_after_draw;

                            writeFrameSyncTraceEvent(
                                details,
                                j,
                                camera_view_presenter_context
                                    .target_display_frame,
                                camera_view_presenter_context
                                    .preferred_paused_slot,
                                presented_slot,
                                presented_frame,
                                camera_view_presenter_result
                                    .resolved_current_frame_num,
                                prewarm_playback_textures);

                            last_trace_state.initialized = true;
                            last_trace_state.has_presented_frame =
                                has_presented_camera_frame;
                            last_trace_state.target_frame =
                                camera_view_presenter_context
                                    .target_display_frame;
                            last_trace_state.presented_frame =
                                presented_frame;
                            last_trace_state.bbox_query_frame =
                                zarr_bbox_query_frame;
                            last_trace_state.latest_decoded_frame =
                                latest_decoded;
                            last_trace_state.front_frame_before_draw =
                                frame_sync_front_frame_before_draw;
                            last_trace_state.front_frame_after_draw =
                                frame_sync_front_frame_after_draw;
                            last_trace_state.staging_frame_before_draw =
                                frame_sync_staging_frame_before_draw;
                            last_trace_state.staging_frame_after_draw =
                                frame_sync_staging_frame_after_draw;
                            last_trace_state.zarr_box_count =
                                zarr_box_count;
                        }
                    }

                    if (clipped_frame_trace_log_writer.enabled() &&
                        zarr_loaded && zarr_loader.hasClippedCollection() &&
                        has_presented_camera_frame) {
                        const int current_parent_frame_index = presented_frame;
                        const int requested_parent_frame_index =
                            camera_view_presenter_context.target_display_frame;
                        const auto* current_row =
                            zarr_loader.resolveClippedFrame(
                                current_parent_frame_index);
                        const auto* requested_row =
                            zarr_loader.resolveClippedFrame(
                                requested_parent_frame_index);
                        const auto* bbox_row =
                            zarr_loader.resolveClippedFrame(
                                zarr_bbox_query_frame);
                        const auto* current_selected =
                            current_row != nullptr
                                ? zarr_loader.getClippedResolver().selectedRun(
                                      current_row->selected_run_index)
                                : nullptr;

                        int decoder_presented_local_frame = -1;
                        int64_t decoder_presented_pts = -1;
                        int decoder_frame_source_code = 0;
                        if (presented_slot >= 0 &&
                            presented_slot <
                                static_cast<int>(scene->size_of_buffer)) {
                            const auto& slot =
                                scene->cameras[j]
                                    .display_buffer[presented_slot];
                            if (!slot.available_to_write) {
                                decoder_presented_local_frame =
                                    slot.local_frame_number;
                                decoder_presented_pts = slot.frame_pts;
                                decoder_frame_source_code =
                                    slot.frame_source_code;
                            }
                        }
                        if (decoder_presented_local_frame < 0) {
                            decoder_presented_local_frame =
                                frame_sync_front_local_before_draw;
                            decoder_presented_pts =
                                frame_sync_front_pts_before_draw;
                        }

                        auto nullableInt = [](int64_t value) -> json {
                            return value >= 0 ? json(value) : json(nullptr);
                        };
                        auto sourceLabel = [](int code) -> const char* {
                            switch (code) {
                            case 1:
                                return "seek";
                            case 2:
                                return "sequential_decode";
                            default:
                                return "unknown";
                            }
                        };
                        auto presentationSource = [&]() -> const char* {
                            if (playback_surface_swapped_before_draw) {
                                return "prefetched_staged_texture";
                            }
                            if (camera_view_presenter_result.perf.upload_count >
                                0) {
                                return "uploaded_this_frame";
                            }
                            return "cached_front_texture";
                        };
                        auto selectedRunJson =
                            [&](const PaletteClippedResolver::SelectedRun* selected)
                            -> json {
                            if (selected == nullptr) {
                                return nullptr;
                            }
                            return json{
                                {"work_unit_id", selected->work_unit_id},
                                {"detect_run", selected->detect_run},
                                {"refined_detect_run",
                                 selected->refined_detect_run},
                                {"detect_group_path",
                                 selected->detect_group_path},
                                {"refined_group_path",
                                 selected->refined_group_path},
                                {"video_path", selected->video_path},
                            };
                        };
                        auto resolverJson =
                            [&](const PaletteClippedResolver::FrameRunRow* row)
                            -> json {
                            if (row == nullptr) {
                                return nullptr;
                            }
                            const auto* selected =
                                zarr_loader.getClippedResolver().selectedRun(
                                    row->selected_run_index);
                            return json{
                                {"resolved_parent_frame_index",
                                 row->parent_frame_index},
                                {"recording_frame_id",
                                 row->recording_frame_id},
                                {"clip_id", row->clip_id},
                                {"clip_index",
                                 static_cast<uint64_t>(
                                     row->selected_run_index)},
                                {"camera_serial", row->camera_serial},
                                {"clip_local_frame_index",
                                 row->clip_local_frame_index},
                                {"selected_run_index",
                                 static_cast<uint64_t>(
                                     row->selected_run_index)},
                                {"selected_run", selectedRunJson(selected)},
                            };
                        };
                        auto bboxJson =
                            [&](const std::vector<LoggedBoundingBox>& boxes)
                            -> json {
                            if (boxes.empty()) {
                                return nullptr;
                            }
                            const auto& box = boxes.front();
                            return json{
                                {"payload_frame_id", box.payload_frame_id},
                                {"payload_camera_id", box.payload_camera_id},
                                {"box_index_in_payload",
                                 static_cast<int>(box.box_index_in_payload)},
                                {"x", box.x_min},
                                {"y", box.y_min},
                                {"w", box.width},
                                {"h", box.height},
                                {"cx", box.x_min + box.width * 0.5f},
                                {"cy", box.y_min + box.height * 0.5f},
                                {"class_id", box.class_id},
                                {"confidence", box.confidence},
                            };
                        };
                        auto detectionSourceJson = [&]() -> json {
                            if (detection_details.detection_source.empty() &&
                                detection_details.detection_reason.empty()) {
                                return nullptr;
                            }
                            std::string reason;
                            if (!detection_details.detection_reason.empty()) {
                                reason =
                                    detection_details.detection_reason.front();
                            }
                            std::string reason_lower = reason;
                            std::transform(
                                reason_lower.begin(), reason_lower.end(),
                                reason_lower.begin(),
                                [](unsigned char c) {
                                    return static_cast<char>(
                                        std::tolower(c));
                                });
                            return json{
                                {"source_code",
                                 !detection_details.detection_source.empty()
                                     ? json(static_cast<int>(
                                           detection_details
                                               .detection_source.front()))
                                     : json(nullptr)},
                                {"source_kind",
                                 !reason.empty() ? json(reason)
                                                 : json(nullptr)},
                                {"manual",
                                 reason_lower.find("manual") !=
                                     std::string::npos},
                            };
                        };
                        auto textureDrawTraceJson =
                            [&](const CameraTextureDrawTrace& trace) -> json {
                            if (!trace.enabled ||
                                trace.queue_sequence == 0) {
                                return nullptr;
                            }
                            return json{
                                {"draw_sequence", trace.queue_sequence},
                                {"view_idx", trace.view_idx},
                                {"queued_texture_id",
                                 static_cast<uint64_t>(
                                     trace.queued_texture_id)},
                                {"front_texture_id",
                                 static_cast<uint64_t>(
                                     trace.front_texture_id)},
                                {"staging_texture_id",
                                 static_cast<uint64_t>(
                                     trace.staging_texture_id)},
                                {"front_pbo_id",
                                 static_cast<uint64_t>(
                                     trace.front_pbo_id)},
                                {"staging_pbo_id",
                                 static_cast<uint64_t>(
                                     trace.staging_pbo_id)},
                                {"queued_texture_matches_front",
                                 trace.queued_texture_id ==
                                     trace.front_texture_id},
                                {"queued_texture_matches_staging",
                                 trace.queued_texture_id ==
                                     trace.staging_texture_id},
                                {"front",
                                 {{"valid", trace.front_valid},
                                  {"parent_frame",
                                   nullableInt(
                                       trace.front_parent_frame)},
                                  {"local_frame",
                                   nullableInt(trace.front_local_frame)},
                                  {"pts", nullableInt(trace.front_pts)}}},
                                {"staging",
                                 {{"valid", trace.staging_valid},
                                  {"parent_frame",
                                   nullableInt(
                                       trace.staging_parent_frame)},
                                  {"local_frame",
                                   nullableInt(trace.staging_local_frame)},
                                  {"pts", nullableInt(trace.staging_pts)}}},
                                {"callback_observed",
                                 trace.callback_observed},
                                {"callback_count",
                                 trace.callback_count},
                                {"callback_active_texture",
                                 trace.callback_observed
                                     ? json(trace.callback_active_texture)
                                     : json(nullptr)},
                                {"callback_bound_texture_id",
                                 trace.callback_observed
                                     ? json(static_cast<uint64_t>(
                                           trace.callback_bound_texture_id))
                                     : json(nullptr)},
                                {"callback_bound_matches_queued",
                                 trace.callback_observed
                                     ? json(trace
                                                .callback_bound_matches_queued)
                                     : json(nullptr)},
                            };
                        };
                        auto clippedPlaybackStateJson = [&]() -> json {
                            json buffer = nullptr;
                            if (scene != nullptr && j >= 0 &&
                                j < scene->num_cams &&
                                scene->size_of_buffer > 0) {
                                const auto& camera = scene->cameras[j];
                                const int normalized_read_head =
                                    ps.read_head >= 0
                                        ? ps.read_head %
                                              static_cast<int>(
                                                  scene->size_of_buffer)
                                        : -1;
                                int valid_slots = 0;
                                int oldest_frame =
                                    std::numeric_limits<int>::max();
                                int newest_frame = -1;
                                int read_head_frame = -1;
                                int target_slot = -1;
                                int presented_slot_by_frame = -1;
                                int front_slot = -1;
                                int contiguous_span_start = -1;
                                int contiguous_span_end = -1;
                                std::vector<int> valid_frames;
                                valid_frames.reserve(scene->size_of_buffer);
                                for (int slot_idx = 0;
                                     slot_idx <
                                     static_cast<int>(scene->size_of_buffer);
                                     ++slot_idx) {
                                    const auto& slot =
                                        camera.display_buffer[slot_idx];
                                    if (slot.available_to_write ||
                                        slot.frame_number < 0) {
                                        continue;
                                    }
                                    valid_slots++;
                                    valid_frames.push_back(slot.frame_number);
                                    oldest_frame = std::min(
                                        oldest_frame, slot.frame_number);
                                    newest_frame = std::max(
                                        newest_frame, slot.frame_number);
                                    if (slot_idx == normalized_read_head) {
                                        read_head_frame = slot.frame_number;
                                    }
                                    if (slot.frame_number ==
                                        requested_parent_frame_index) {
                                        target_slot = slot_idx;
                                    }
                                    if (slot.frame_number == presented_frame) {
                                        presented_slot_by_frame = slot_idx;
                                    }
                                    if (slot.frame_number ==
                                        camera.last_uploaded_frame) {
                                        front_slot = slot_idx;
                                    }
                                }
                                if (!valid_frames.empty()) {
                                    std::sort(valid_frames.begin(),
                                              valid_frames.end());
                                    contiguous_span_end = valid_frames.back();
                                    contiguous_span_start =
                                        contiguous_span_end;
                                    for (int idx =
                                             static_cast<int>(
                                                 valid_frames.size()) -
                                             2;
                                         idx >= 0; --idx) {
                                        if (valid_frames[idx] + 1 ==
                                            contiguous_span_start) {
                                            contiguous_span_start =
                                                valid_frames[idx];
                                            continue;
                                        }
                                        break;
                                    }
                                }

                                std::string current_frame_source =
                                    "presenter_presented_frame";
                                if (!has_presented_camera_frame) {
                                    current_frame_source =
                                        ps.play_video && read_head_frame >= 0
                                            ? "read_head_slot"
                                            : "to_display_frame_number";
                                } else if (presented_slot >= 0 &&
                                           presented_slot ==
                                               normalized_read_head) {
                                    current_frame_source =
                                        "presenter_read_head_slot";
                                } else if (presented_slot_by_frame >= 0 &&
                                           presented_slot_by_frame !=
                                               normalized_read_head) {
                                    current_frame_source =
                                        "presenter_exact_frame_search";
                                }

                                buffer = {
                                    {"current_frame_source",
                                     current_frame_source},
                                    {"read_head", ps.read_head},
                                    {"normalized_read_head",
                                     normalized_read_head},
                                    {"read_head_frame",
                                     nullableInt(read_head_frame)},
                                    {"presented_slot", presented_slot},
                                    {"presented_slot_by_frame",
                                     presented_slot_by_frame},
                                    {"target_slot", target_slot},
                                    {"front_slot", front_slot},
                                    {"valid_slots", valid_slots},
                                    {"buffer_size",
                                     static_cast<int>(
                                         scene->size_of_buffer)},
                                    {"oldest_frame",
                                     valid_slots > 0
                                         ? json(oldest_frame)
                                         : json(nullptr)},
                                    {"newest_frame",
                                     valid_slots > 0 ? json(newest_frame)
                                                     : json(nullptr)},
                                    {"newest_contiguous_span_start",
                                     nullableInt(contiguous_span_start)},
                                    {"newest_contiguous_span_end",
                                     nullableInt(contiguous_span_end)},
                                    {"front_parent_frame",
                                     nullableInt(
                                         camera.last_uploaded_frame)},
                                    {"front_local_frame",
                                     nullableInt(
                                         camera.last_uploaded_local_frame)},
                                    {"staging_valid",
                                     camera.playback_staging_valid},
                                    {"staging_parent_frame",
                                     nullableInt(
                                         camera.playback_staging_frame)},
                                    {"staging_local_frame",
                                     nullableInt(
                                         camera.playback_staging_local_frame)},
                                };
                            }

                            return json{
                                {"play_video", ps.play_video},
                                {"to_display_frame_number",
                                 ps.to_display_frame_number},
                                {"slider_frame_number",
                                 ps.slider_frame_number},
                                {"pause_selected", ps.pause_selected},
                                {"pause_seeked", ps.pause_seeked},
                                {"just_seeked", ps.just_seeked},
                                {"slider_just_changed",
                                 ps.slider_just_changed},
                                {"buffer_browsed_since_pause",
                                 ps.buffer_browsed_since_pause},
                                {"paused_frame_on_toggle",
                                 nullableInt(ps.paused_frame_on_toggle)},
                                {"last_resume_path",
                                 resumePathName(ps.last_resume_path)},
                                {"last_resume_target_frame",
                                 nullableInt(ps.last_resume_target_frame)},
                                {"accumulated_play_time",
                                 ps.accumulated_play_time},
                                {"clock_frame",
                                 static_cast<int>(std::ceil(
                                     ps.accumulated_play_time *
                                     video_fps))},
                                {"clipped_rebase_before_play",
                                 clipped_rebase_before_play},
                                {"buffer", buffer},
                            };
                        };

                        const bool parent_matches_resolver =
                            current_row != nullptr &&
                            current_parent_frame_index ==
                                current_row->parent_frame_index;
                        const bool bbox_parent_matches_display =
                            zarr_bbox_query_frame ==
                            current_parent_frame_index;
                        const bool bbox_local_matches_resolver =
                            current_row != nullptr && bbox_row != nullptr &&
                            bbox_row->clip_local_frame_index ==
                                current_row->clip_local_frame_index;
                        const bool decoder_local_known =
                            current_row != nullptr &&
                            decoder_presented_local_frame >= 0;
                        const bool decoder_local_matches_resolver =
                            decoder_local_known &&
                            decoder_presented_local_frame ==
                                current_row->clip_local_frame_index;
                        const bool front_before_known =
                            current_row != nullptr &&
                            frame_sync_front_valid_before_draw &&
                            frame_sync_front_local_before_draw >= 0;
                        const bool front_after_known =
                            current_row != nullptr &&
                            frame_sync_front_valid_after_draw &&
                            frame_sync_front_local_after_draw >= 0;
                        const bool front_texture_matches_resolver_before =
                            front_before_known &&
                            frame_sync_front_local_before_draw ==
                                current_row->clip_local_frame_index;
                        const bool front_texture_matches_resolver_after =
                            front_after_known &&
                            frame_sync_front_local_after_draw ==
                                current_row->clip_local_frame_index;

                        clipped_frame_trace_stats.frames_traced++;
                        if (!parent_matches_resolver) {
                            clipped_frame_trace_stats.parent_mismatches++;
                        }
                        if (!bbox_parent_matches_display) {
                            clipped_frame_trace_stats
                                .bbox_parent_mismatches++;
                        }
                        if (!bbox_local_matches_resolver) {
                            clipped_frame_trace_stats.bbox_local_mismatches++;
                        }
                        if (decoder_local_known) {
                            const int64_t delta =
                                static_cast<int64_t>(
                                    decoder_presented_local_frame) -
                                current_row->clip_local_frame_index;
                            clipped_frame_trace_stats.decoder_local_delta.add(
                                delta);
                            if (!decoder_local_matches_resolver) {
                                clipped_frame_trace_stats
                                    .decoder_local_mismatches++;
                            }
                        }
                        clipped_frame_trace_stats.bbox_parent_delta.add(
                            static_cast<int64_t>(zarr_bbox_query_frame) -
                            current_parent_frame_index);
                        if (current_row != nullptr && bbox_row != nullptr) {
                            clipped_frame_trace_stats.bbox_local_delta.add(
                                bbox_row->clip_local_frame_index -
                                current_row->clip_local_frame_index);
                        }
                        if (front_before_known &&
                            !front_texture_matches_resolver_before) {
                            clipped_frame_trace_stats
                                .front_texture_before_mismatches++;
                        }
                        if (front_after_known &&
                            !front_texture_matches_resolver_after) {
                            clipped_frame_trace_stats
                                .front_texture_after_mismatches++;
                        }

                        double active_timebase = 0.0;
                        if (j < static_cast<int>(demuxers.size()) &&
                            demuxers[j] != nullptr) {
                            active_timebase = demuxers[j]->GetTimebase();
                        }
                        clipped_frame_trace_log_writer.write(
                            json{
                                {"event", "clipped_frame"},
                                {"ui_parent_timeline",
                                 {{"current_parent_frame_index",
                                   current_parent_frame_index},
                                  {"requested_parent_frame_index",
                                   requested_parent_frame_index},
                                  {"playback_running", ps.play_video},
                                  {"playback_speed", set_playback_speed}}},
                                {"playback_state",
                                 clippedPlaybackStateJson()},
                                {"resolver", resolverJson(current_row)},
                                {"requested_resolver",
                                 resolverJson(requested_row)},
                                {"video",
                                 {{"active_video_path",
                                   clipped_media_state.current_video_path},
                                  {"active_clip_id",
                                   clipped_media_state.clip_id},
                                  {"requested_decoder_local_frame",
                                   requested_row != nullptr
                                       ? json(requested_row
                                                  ->clip_local_frame_index)
                                       : json(nullptr)},
                                  {"decoder_presented_local_frame",
                                   nullableInt(
                                       decoder_presented_local_frame)},
                                  {"front_texture_local_frame_before_draw",
                                   nullableInt(
                                       frame_sync_front_local_before_draw)},
                                  {"front_texture_local_frame_after_draw",
                                   nullableInt(
                                       frame_sync_front_local_after_draw)},
                                  {"front_texture_parent_frame_before_draw",
                                   nullableInt(
                                       frame_sync_front_frame_before_draw)},
                                  {"front_texture_parent_frame_after_draw",
                                   nullableInt(
                                       frame_sync_front_frame_after_draw)},
                                  {"presented_pts",
                                   nullableInt(decoder_presented_pts)},
                                  {"front_texture_pts_before_draw",
                                   nullableInt(
                                       frame_sync_front_pts_before_draw)},
                                  {"front_texture_pts_after_draw",
                                   nullableInt(
                                       frame_sync_front_pts_after_draw)},
                                  {"timebase", active_timebase},
                                  {"decoder_frame_source",
                                   sourceLabel(decoder_frame_source_code)},
                                  {"presentation_source",
                                   presentationSource()},
                                  {"presented_slot", presented_slot},
                                  {"latest_decoded_parent_frame",
                                   latest_decoded},
                                  {"draw_texture",
                                   textureDrawTraceJson(
                                       scene->cameras[j]
                                           .texture_draw_trace)}}},
                                {"bbox",
                                 {{"bbox_query_parent_frame_index",
                                   zarr_bbox_query_frame},
                                  {"bbox_query_clip_local_frame_index",
                                   bbox_row != nullptr
                                       ? json(bbox_row
                                                  ->clip_local_frame_index)
                                       : json(nullptr)},
                                  {"bbox_payload_frame_index",
                                   bbox_row != nullptr
                                       ? json(bbox_row
                                                  ->clip_local_frame_index)
                                       : json(nullptr)},
                                  {"bbox_row_count",
                                   static_cast<int>(
                                       loaded_zarr_boxes.size())},
                                  {"first_bbox_source_image",
                                   bboxJson(loaded_zarr_boxes)},
                                  {"first_bbox_display",
                                   bboxJson(zarr_boxes)},
                                  {"first_bbox_source",
                                   detectionSourceJson()},
                                  {"detection_details_frame_id",
                                   detection_details.frame_id}}},
                                {"sanity",
                                 {{"parent_matches_resolver",
                                   parent_matches_resolver},
                                  {"bbox_parent_matches_display",
                                   bbox_parent_matches_display},
                                  {"bbox_local_matches_resolver",
                                   bbox_local_matches_resolver},
                                  {"decoder_local_matches_resolver",
                                   decoder_local_known
                                       ? json(
                                             decoder_local_matches_resolver)
                                       : json(nullptr)},
                                  {"front_texture_matches_resolver_before_draw",
                                   front_before_known
                                       ? json(
                                             front_texture_matches_resolver_before)
                                       : json(nullptr)},
                                  {"front_texture_matches_resolver_after_draw",
                                   front_after_known
                                       ? json(
                                             front_texture_matches_resolver_after)
                                       : json(nullptr)}}},
                                {"deltas",
                                 {{"decoder_presented_local_minus_clip_local",
                                   decoder_local_known
                                       ? json(
                                             decoder_presented_local_frame -
                                             current_row
                                                 ->clip_local_frame_index)
                                       : json(nullptr)},
                                  {"bbox_query_parent_minus_current_parent",
                                   zarr_bbox_query_frame -
                                       current_parent_frame_index},
                                  {"bbox_query_local_minus_clip_local",
                                   current_row != nullptr && bbox_row != nullptr
                                       ? json(bbox_row
                                                  ->clip_local_frame_index -
                                              current_row
                                                  ->clip_local_frame_index)
                                       : json(nullptr)}}}},
                            /*force_flush=*/false);

                        if ((clipped_frame_trace_stats.frames_traced % 300) ==
                            0) {
                            writeClippedFrameTraceSummary("periodic");
                        }
                    }

                    if (playback_smoke.enabled && playback_smoke.started &&
                        !playback_smoke.completed &&
                        has_presented_camera_frame) {
                        playback_smoke.last_presented_frame = presented_frame;
                        playback_smoke.max_presented_frame =
                            std::max(playback_smoke.max_presented_frame,
                                     presented_frame);
                        playback_smoke.last_presented_slot = presented_slot;
                        playback_smoke.last_view_idx = j;
                        ++playback_smoke.presented_count;
                        if (presented_frame >= playback_smoke.end_frame) {
                            playback_smoke.completed = true;
                            const double elapsed_s =
                                std::chrono::duration<double>(
                                    std::chrono::steady_clock::now() -
                                    playback_smoke.start_time)
                                    .count();
                            std::cout << "[PlaybackSmoke] PASS "
                                      << "start_frame="
                                      << playback_smoke.start_frame
                                      << " end_frame="
                                      << playback_smoke.end_frame
                                      << " presented_frame="
                                      << presented_frame
                                      << " presented_slot="
                                      << presented_slot
                                      << " view_idx=" << j
                                      << " presented_count="
                                      << playback_smoke.presented_count
                                      << " elapsed_s=" << elapsed_s
                                      << std::endl;
                            writePlaybackTraceEvent(
                                "playback_smoke_pass",
                                {{"start_frame",
                                  playback_smoke.start_frame},
                                 {"end_frame", playback_smoke.end_frame},
                                 {"presented_frame", presented_frame},
                                 {"presented_slot", presented_slot},
                                 {"presented_count",
                                  playback_smoke.presented_count},
                                 {"elapsed_s", elapsed_s}},
                                j,
                                camera_view_presenter_context
                                    .target_display_frame,
                                camera_view_presenter_context
                                    .preferred_paused_slot,
                                presented_slot,
                                presented_frame,
                                camera_view_presenter_result
                                    .resolved_current_frame_num,
                                prewarm_playback_textures);
                            app_exit_code = 0;
                            glfwSetWindowShouldClose(window->render_target,
                                                     GLFW_TRUE);
                        }
                    }

                    if (clipped_boundary_smoke.enabled &&
                        clipped_boundary_smoke.started &&
                        !clipped_boundary_smoke.completed &&
                        has_presented_camera_frame &&
                        presented_frame >= clipped_boundary_smoke.end_frame) {
                        const size_t expected_run = clippedSelectedRunForFrame(
                            clipped_boundary_smoke.end_frame);
                        const size_t presented_run = clippedSelectedRunForFrame(
                            presented_frame);
                        const bool bbox_matches_presented =
                            zarr_bbox_query_frame == presented_frame;
                        const bool run_matches =
                            expected_run !=
                                std::numeric_limits<size_t>::max() &&
                            presented_run == expected_run;
                        if (bbox_matches_presented && run_matches) {
                            clipped_boundary_smoke.completed = true;
                            std::cout << "[ClippedBoundarySmoke] PASS "
                                      << "presented_frame=" << presented_frame
                                      << " bbox_query_frame="
                                      << zarr_bbox_query_frame
                                      << " clip=" << clipped_media_state.clip_id
                                      << std::endl;
                            writeClippedHandoffTraceEvent(
                                "smoke_pass",
                                {{"presented_frame", presented_frame},
                                 {"bbox_query_frame", zarr_bbox_query_frame},
                                 {"expected_end_frame",
                                  clipped_boundary_smoke.end_frame},
                                 {"selected_run_index", presented_run},
                                 {"clip_id", clipped_media_state.clip_id}});
                            app_exit_code = 0;
                            glfwSetWindowShouldClose(window->render_target,
                                                     GLFW_TRUE);
                        } else {
                            std::cerr << "[ClippedBoundarySmoke] FAIL "
                                      << "presented_frame=" << presented_frame
                                      << " bbox_query_frame="
                                      << zarr_bbox_query_frame
                                      << " expected_run=" << expected_run
                                      << " presented_run=" << presented_run
                                      << std::endl;
                            writeClippedHandoffTraceEvent(
                                "smoke_fail",
                                {{"presented_frame", presented_frame},
                                 {"bbox_query_frame", zarr_bbox_query_frame},
                                 {"expected_end_frame",
                                  clipped_boundary_smoke.end_frame},
                                 {"expected_run", expected_run},
                                 {"presented_run", presented_run}});
                            app_exit_code = 4;
                            glfwSetWindowShouldClose(window->render_target,
                                                     GLFW_TRUE);
                        }
                    }

                    if (zarr_loaded) {
                        applyCameraViewSubjectMaskPick(
                            camera_view_result,
                            zarr_loader,
                            frame_debug_window_state);
                        applyCameraViewSubjectMaskPaint(
                            camera_view_result,
                            frame_debug_window_state);
                    }

                    perf_camera_viewport_width_px =
                        camera_view_result.perf.viewport_width_px;
                    perf_camera_viewport_height_px =
                        camera_view_result.perf.viewport_height_px;
                    perf_camera_view_x_min =
                        camera_view_result.perf.view_x_min;
                    perf_camera_view_x_max =
                        camera_view_result.perf.view_x_max;
                    perf_camera_view_y_min =
                        camera_view_result.perf.view_y_min;
                    perf_camera_view_y_max =
                        camera_view_result.perf.view_y_max;
                    perf_camera_view_visible_fraction =
                        camera_view_result.perf.visible_fraction;
                    perf_camera_view_zoomed_in =
                        camera_view_result.perf.zoomed_in;
                    frame_camera_plot_image_ui_ms +=
                        camera_view_result.perf.plot_image_ui_ms;
                    frame_camera_overlay_ui_ms +=
                        camera_view_result.perf.overlay_ui_ms;
                    frame_bbox_overlay_build_ms +=
                        camera_view_result.perf.bbox_overlay_build_ms;
                    frame_bbox_overlay_draw_ms +=
                        camera_view_result.perf.bbox_overlay_draw_ms;
                    frame_bbox_overlay_item_count +=
                        camera_view_result.perf.bbox_overlay_item_count;
                    frame_subject_shape_overlay_ms +=
                        camera_view_result.perf.subject_shape_overlay_ms;
                    frame_tail_kinematics_overlay_ms +=
                        camera_view_result.perf.tail_kinematics_overlay_ms;
                    frame_camera_playback_swap_ms +=
                        camera_view_result.perf.playback_swap_ms;
                    frame_camera_scene_ui_ms +=
                        camera_view_result.perf.scene_ui_ms;
                    frame_sync_valid_slots =
                        camera_view_result.frame_sync.valid_slots;
                    frame_sync_empty_slots =
                        camera_view_result.frame_sync.empty_slots;
                    frame_sync_latest_decoded =
                        camera_view_result.frame_sync.latest_decoded;
                    frame_sync_recording_remaining =
                        camera_view_result.frame_sync.recording_remaining;
                    frame_sync_recording_total =
                        camera_view_result.frame_sync.recording_total;
                    frame_sync_debug_line =
                        camera_view_result.frame_sync.debug_line;

                    const FullFrameRectEditResult& full_frame_edit_result =
                        camera_view_result.full_frame_edit_result;
                    frame_debug_window_state.keypoint_review_panel.full_frame_edit =
                        camera_view_result.full_frame_keypoint_edit_state;

                    g_zarr_bbox_edit_state.selected_frame =
                        full_frame_edit_result.state.selected_frame;
                    g_zarr_bbox_edit_state.selected_box =
                        full_frame_edit_result.state.selected_box;
                    g_zarr_bbox_edit_state.drag_active =
                        full_frame_edit_result.state.drag_active;
                    g_zarr_bbox_edit_state.drag_mouse_button =
                        full_frame_edit_result.state.drag_mouse_button;
                    g_zarr_bbox_edit_state.drag_offset_x =
                        full_frame_edit_result.state.drag_offset_x;
                    g_zarr_bbox_edit_state.drag_offset_y =
                        full_frame_edit_result.state.drag_offset_y;
                    g_zarr_bbox_edit_state.draw_mode =
                        full_frame_edit_result.state.draw_mode;
                    g_zarr_bbox_edit_state.draw_active =
                        full_frame_edit_result.state.draw_active;
                    g_zarr_bbox_edit_state.draw_frame =
                        full_frame_edit_result.state.draw_frame;
                    g_zarr_bbox_edit_state.draw_anchor_x =
                        full_frame_edit_result.state.draw_anchor_x;
                    g_zarr_bbox_edit_state.draw_anchor_y =
                        full_frame_edit_result.state.draw_anchor_y;
                    g_zarr_bbox_edit_state.draw_current_x =
                        full_frame_edit_result.state.draw_current_x;
                    g_zarr_bbox_edit_state.draw_current_y =
                        full_frame_edit_result.state.draw_current_y;

                    if (full_frame_edit_result.request_reset_frame) {
                        g_zarr_bbox_edit_state.clearFrameEdits(
                            current_frame_num);
                        zarr_boxes = loaded_zarr_boxes;
                    }
                    if (full_frame_edit_result.request_delete_selected) {
                        deleteSelectedBoxOnCurrentFrame();
                    }
                    if (full_frame_edit_result.request_add_rect) {
                        auto& editable_boxes =
                            g_zarr_bbox_edit_state.ensureFrameOverride(
                                current_frame_num, loaded_zarr_boxes,
                                &detection_details);
                        auto& added_flags =
                            g_zarr_bbox_edit_state.ensureAddedFlags(
                                current_frame_num, editable_boxes.size());
                        auto& manual_flags =
                            g_zarr_bbox_edit_state.ensureManualFlags(
                                current_frame_num, editable_boxes.size());
                        g_zarr_bbox_edit_state.ensureSourceMetadata(
                            current_frame_num, editable_boxes.size());
                        auto& source_indices =
                            g_zarr_bbox_edit_state
                                .frame_source_indices[current_frame_num];
                        auto& source_detection_source =
                            g_zarr_bbox_edit_state
                                .frame_source_detection_source[current_frame_num];
                        auto& source_reason =
                            g_zarr_bbox_edit_state
                                .frame_source_reason[current_frame_num];

                        uint16_t new_class_id = 0;
                        float new_confidence = 1.0f;
                        if (g_zarr_bbox_edit_state.selected_frame ==
                                current_frame_num &&
                            g_zarr_bbox_edit_state.selected_box >= 0 &&
                            g_zarr_bbox_edit_state.selected_box <
                                static_cast<int>(zarr_boxes.size())) {
                            const auto& selected_box =
                                zarr_boxes[g_zarr_bbox_edit_state.selected_box];
                            new_class_id = selected_box.class_id;
                            new_confidence = selected_box.confidence;
                        } else if (!zarr_boxes.empty()) {
                            new_class_id = zarr_boxes.front().class_id;
                            new_confidence = zarr_boxes.front().confidence;
                        }

                        LoggedBoundingBox new_box{};
                        new_box.payload_timestamp_ns_epoch = 0;
                        new_box.received_timestamp_ns_epoch = 0;
                        new_box.payload_frame_id = static_cast<uint64_t>(
                            std::max(0, current_frame_num));
                        new_box.payload_camera_id = 0;
                        new_box.box_index_in_payload =
                            static_cast<uint8_t>(editable_boxes.size());
                        new_box.x_min = full_frame_edit_result.new_rect.x_min;
                        new_box.y_min = full_frame_edit_result.new_rect.y_min;
                        new_box.width = full_frame_edit_result.new_rect.width;
                        new_box.height = full_frame_edit_result.new_rect.height;
                        new_box.class_id = new_class_id;
                        new_box.confidence =
                            std::isfinite(new_confidence) ? new_confidence
                                                          : 1.0f;

                        editable_boxes.push_back(new_box);
                        added_flags.push_back(1);
                        manual_flags.push_back(1);
                        source_indices.push_back(-1);
                        source_detection_source.push_back(0);
                        source_reason.emplace_back("manual");
                        g_zarr_bbox_edit_state.dirty_frames.insert(
                            current_frame_num);
                        g_zarr_bbox_edit_state.selected_frame =
                            current_frame_num;
                        g_zarr_bbox_edit_state.selected_box =
                            static_cast<int>(editable_boxes.size() - 1);
                    }
                    if (full_frame_edit_result.request_move_selected) {
                        auto& editable_boxes =
                            g_zarr_bbox_edit_state.ensureFrameOverride(
                                current_frame_num, loaded_zarr_boxes,
                                &detection_details);
                        auto& manual_flags =
                            g_zarr_bbox_edit_state.ensureManualFlags(
                                current_frame_num, editable_boxes.size());
                        const int selected_idx =
                            full_frame_edit_result.move_box_index;
                        if (selected_idx >= 0 &&
                            selected_idx <
                                static_cast<int>(editable_boxes.size())) {
                            LoggedBoundingBox& moving_box =
                                editable_boxes[selected_idx];
                            const float max_x = std::max(
                                0.0f,
                                static_cast<float>(scene->cameras[j].image_width) -
                                    moving_box.width);
                            const float max_y = std::max(
                                0.0f,
                                static_cast<float>(scene->cameras[j].image_height) -
                                    moving_box.height);
                            moving_box.x_min = std::clamp(
                                full_frame_edit_result.move_target_x, 0.0f,
                                max_x);
                            moving_box.y_min = std::clamp(
                                full_frame_edit_result.move_target_y, 0.0f,
                                max_y);
                            if (selected_idx <
                                static_cast<int>(manual_flags.size())) {
                                manual_flags[selected_idx] = 1;
                            }
                            g_zarr_bbox_edit_state.dirty_frames.insert(
                                current_frame_num);
                        } else {
                            g_zarr_bbox_edit_state.clearSelection();
                        }
                    }

                    if (use_legacy_manual_keypoint_tools) {
                        legacy_labeling_state.keypoints_find =
                            camera_view_result.legacy_manual_keypoints_find;
                        is_view_focused[j] = camera_view_result.view_focused;
                    }
                    const CameraViewTransportControlsResult&
                        camera_transport_result =
                            camera_view_result.transport_result;
                    ps.slider_frame_number =
                        camera_transport_result.slider_frame_number;
                    ps.slider_just_changed =
                        camera_transport_result.slider_just_changed;
                    if (camera_transport_result.toggle_playback) {
                        applyPlaybackToggleForPerf();
                        writePlaybackTraceEvent(
                            "toggle_playback",
                            json{{"source", "camera_controls"}});
                    }
                    if (camera_transport_result.step_delta != 0) {
                        writeClippedPlaybackStateEvent(
                            "step_request",
                            json{{"source", "camera_controls"},
                                 {"phase", "before"},
                                 {"delta",
                                  camera_transport_result.step_delta}},
                            true);
                        writePlaybackTraceEvent(
                            "step_request",
                            json{{"source", "camera_controls"},
                                 {"phase", "before"},
                                 {"delta",
                                  camera_transport_result.step_delta}});
                        playback_session_controller.stepFrames(
                            camera_transport_result.step_delta);
                        writeClippedPlaybackStateEvent(
                            "step_request",
                            json{{"source", "camera_controls"},
                                 {"phase", "after"},
                                 {"delta",
                                  camera_transport_result.step_delta}},
                            true);
                        writePlaybackTraceEvent(
                            "step_request",
                            json{{"source", "camera_controls"},
                                 {"phase", "after"},
                                 {"delta",
                                  camera_transport_result.step_delta}});
                    }
                    if (camera_transport_result.seek_target_frame.has_value()) {
                        writeClippedPlaybackStateEvent(
                            "seek_request",
                            json{{"source", "camera_controls"},
                                 {"phase", "before"},
                                 {"target_frame",
                                  *camera_transport_result.seek_target_frame},
                                 {"force_inaccurate",
                                  camera_transport_result
                                      .force_inaccurate_seek}},
                            true);
                        writePlaybackTraceEvent(
                            "seek_request",
                            json{{"source", "camera_controls"},
                                 {"target_frame",
                                  *camera_transport_result.seek_target_frame},
                                 {"force_inaccurate",
                                  camera_transport_result
                                      .force_inaccurate_seek}});
                        playback_session_controller.seekToFrame(
                            *camera_transport_result.seek_target_frame, true,
                            camera_transport_result.force_inaccurate_seek);
                        writeClippedPlaybackStateEvent(
                            "seek_request",
                            json{{"source", "camera_controls"},
                                 {"phase", "after"},
                                 {"target_frame",
                                  *camera_transport_result.seek_target_frame},
                                 {"force_inaccurate",
                                  camera_transport_result
                                      .force_inaccurate_seek}},
                            true);
                    }
                }
                ImGui::End();
            }

            const CameraViewPlaybackShortcutsResult playback_shortcuts =
                handleCameraViewPlaybackShortcuts();
            if (playback_shortcuts.toggle_playback) {
                applyPlaybackToggleForPerf();
                writePlaybackTraceEvent("toggle_playback",
                                        json{{"source", "shortcut"}});
            }
            if (playback_shortcuts.step_delta != 0) {
                writeClippedPlaybackStateEvent(
                    "step_request",
                    json{{"source", "shortcut"},
                         {"phase", "before"},
                         {"delta", playback_shortcuts.step_delta}},
                    true);
                writePlaybackTraceEvent(
                    "step_request",
                    json{{"source", "shortcut"},
                         {"phase", "before"},
                         {"delta", playback_shortcuts.step_delta}});
                playback_session_controller.stepFrames(
                    playback_shortcuts.step_delta);
                writeClippedPlaybackStateEvent(
                    "step_request",
                    json{{"source", "shortcut"},
                         {"phase", "after"},
                         {"delta", playback_shortcuts.step_delta}},
                    true);
                writePlaybackTraceEvent(
                    "step_request",
                    json{{"source", "shortcut"},
                         {"phase", "after"},
                         {"delta", playback_shortcuts.step_delta}});
            }

            for (const auto &[name, flag] : window_need_decoding) {
                window_was_decoding[name] = flag.load();
            }
        }

        if (frame_debug_window_state.keypoint_review_panel
                .show_advanced_crop_preview &&
            zarr_loaded &&
            (zarr_loader.hasCropImages() || zarr_loader.hasKeypointData() ||
             zarr_loader.hasEyeMasks())) {
            const auto crop_preview_ui_start = std::chrono::steady_clock::now();
            RefinedKeypointRepository refined_keypoint_repo(zarr_loader);
            CropFrameSource live_crop_frame_source;
            int crop_preview_frame_num = current_frame_num;
            if (video_loaded) {
                const int visible_idx =
                    playback_session_controller.getVisibleCameraIndex();
                if (visible_idx >= 0 && scene->size_of_buffer > 0) {
                    const auto& camera = scene->cameras[visible_idx];
                    if (ps.play_video && camera.texture_has_valid_frame &&
                        camera.last_uploaded_frame >= 0) {
                        crop_preview_frame_num = camera.last_uploaded_frame;
                    }
                    const int preferred_slot = ps.read_head % scene->size_of_buffer;
                    const int slot_index = findCameraDisplaySlotForFrame(
                        *scene, visible_idx, crop_preview_frame_num,
                        preferred_slot);
                    if (camera.texture_has_valid_frame &&
                        camera.last_uploaded_frame == crop_preview_frame_num &&
                        camera.image_texture != 0) {
                        live_crop_frame_source.frame_number = crop_preview_frame_num;
                        live_crop_frame_source.width =
                            static_cast<int>(camera.image_width);
                        live_crop_frame_source.height =
                            static_cast<int>(camera.image_height);
                        live_crop_frame_source.texture_id = camera.image_texture;
                        live_crop_frame_source.texture_width =
                            camera.display_texture_width > 0
                                ? camera.display_texture_width
                                : static_cast<int>(camera.image_width);
                        live_crop_frame_source.texture_height =
                            camera.display_texture_height > 0
                                ? camera.display_texture_height
                                : static_cast<int>(camera.image_height);
                        live_crop_frame_source.texture_frame_number =
                            camera.last_uploaded_frame;
                    }
                    if (slot_index >= 0) {
                        const auto& slot = camera.display_buffer[slot_index];
                        if (!slot.available_to_write &&
                            slot.frame_number == crop_preview_frame_num &&
                            slot.frame != nullptr) {
                            live_crop_frame_source.frame = slot.frame;
                            live_crop_frame_source.frame_number =
                                slot.frame_number;
                            live_crop_frame_source.width =
                                static_cast<int>(camera.image_width);
                            live_crop_frame_source.height =
                                static_cast<int>(camera.image_height);
                            live_crop_frame_source.pitch_bytes =
                                slot.pitch_bytes;
                            live_crop_frame_source.color_matrix =
                                slot.color_matrix;
                            live_crop_frame_source.color_range =
                                slot.color_range;
                            if (scene->use_cpu_buffer &&
                                slot.format == PictureBufferFormat::RGBA32) {
                                live_crop_frame_source.storage =
                                    CropFrameStorage::HostRGBA32;
                            } else if (slot.format ==
                                       PictureBufferFormat::RGBA32) {
                                live_crop_frame_source.storage =
                                    CropFrameStorage::DeviceRGBA32;
                            } else if (slot.format ==
                                       PictureBufferFormat::NV12) {
                                live_crop_frame_source.storage =
                                    CropFrameStorage::DeviceNV12;
                            }
                        }
                    }
                }
            }
            ZarrPersistedCropProvider persisted_crop_image_provider(zarr_loader);
            LiveCropImageProvider live_crop_image_provider(
                zarr_loader, live_crop_frame_source);
            ChainedCropImageProvider crop_image_provider(
                live_crop_image_provider, persisted_crop_image_provider);
            std::optional<CropSpec> selected_crop_spec;
            int selected_detection_index = -1;
            if (g_zarr_bbox_edit_state.selected_frame == crop_preview_frame_num &&
                g_zarr_bbox_edit_state.selected_box >= 0 &&
                zarr_loader.hasDetectionData()) {
                const auto loaded_crop_boxes =
                    zarr_loader.getBoundingBoxesForFrame(crop_preview_frame_num);
                const auto resolved_crop_boxes =
                    g_zarr_bbox_edit_state.resolveFrameBoxes(crop_preview_frame_num,
                                                             loaded_crop_boxes);
                if (g_zarr_bbox_edit_state.selected_box <
                    static_cast<int>(resolved_crop_boxes.size())) {
                    const auto& selected_box =
                        resolved_crop_boxes[static_cast<size_t>(
                            g_zarr_bbox_edit_state.selected_box)];
                    CropSpec crop_spec;
                    crop_spec.offset_x = selected_box.x_min;
                    crop_spec.offset_y = selected_box.y_min;
                    crop_spec.width_px = selected_box.width;
                    crop_spec.height_px = selected_box.height;
                    crop_spec.valid = selected_box.width > 0.0f &&
                                      selected_box.height > 0.0f;
                    if (crop_spec.valid) {
                        selected_crop_spec = crop_spec;
                    }

                    auto source_it =
                        g_zarr_bbox_edit_state.frame_source_indices.find(
                            crop_preview_frame_num);
                    if (source_it !=
                            g_zarr_bbox_edit_state.frame_source_indices.end() &&
                        g_zarr_bbox_edit_state.selected_box <
                            static_cast<int>(source_it->second.size())) {
                        selected_detection_index =
                            source_it->second[static_cast<size_t>(
                                g_zarr_bbox_edit_state.selected_box)];
                    } else if (g_zarr_bbox_edit_state.selected_box <
                               static_cast<int>(loaded_crop_boxes.size())) {
                        selected_detection_index =
                            g_zarr_bbox_edit_state.selected_box;
                    }
                }
            }
            const CropPreviewWindowContext crop_preview_context{
                crop_image_provider,
                zarr_loader,
                refined_keypoint_repo,
                crop_preview_frame_num,
                g_zarr_bbox_edit_state.selected_frame,
                g_zarr_bbox_edit_state.selected_box,
                selected_detection_index,
                selected_crop_spec,
                ps.play_video,
                &frame_debug_window_state.keypoint_review_panel
                     .show_advanced_crop_preview,
            };
            const auto crop_preview_result = drawCropPreviewWindow(
                crop_preview_context, crop_preview_window_state);
            frame_crop_preview_perf = crop_preview_result.perf;

            startKeypointWriteIfRequested(
                pending_keypoint_write,
                zarr_loader,
                crop_preview_result.editor_action,
                crop_preview_result.selected_keypoint_selection,
                true,
                true,
                frame_debug_window_state.keypoint_review_panel
                    .manual_write_status);
            frame_crop_preview_ui_ms +=
                durationMs(std::chrono::steady_clock::now() - crop_preview_ui_start);
        }

        if (stimulus_player.loaded && show_stimulus_debug_windows) {
            const auto stimulus_debug_windows_result =
                drawStimulusPlaybackDebugWindows(
                    StimulusPlaybackDebugWindowsContext{
                        stimulus_player,
                        zarr_loaded ? &zarr_loader : nullptr,
                        ps,
                        seek_progress,
                        current_frame_num,
                        latest_decoded_frame[stimulus_player.window_name]
                            .load(),
                    });
            frame_stimulus_window_ui_ms +=
                stimulus_debug_windows_result.stimulus_window_ui_ms;
            frame_stimulus_buffer_window_ui_ms +=
                stimulus_debug_windows_result.stimulus_buffer_window_ui_ms;
        }

        if (use_legacy_manual_keypoint_tools) {
            const auto keypoints_window_ui_start =
                std::chrono::steady_clock::now();
            const KeypointsWindowContext keypoints_window_context{
                static_cast<int>(scene->num_cams),
                legacy_labeling_state,
                current_frame_num,
                camera_names,
                is_view_focused,
            };
            drawKeypointsWindow(keypoints_window_context);
            frame_keypoints_window_ui_ms += durationMs(
                std::chrono::steady_clock::now() - keypoints_window_ui_start);
        }

        if (use_legacy_manual_keypoint_tools) {
            const auto labeling_tool_ui_start =
                std::chrono::steady_clock::now();
#if CRIMSON_ENABLE_SFM
            constexpr bool triangulation_supported = true;
#else
            constexpr bool triangulation_supported = false;
#endif
            const LabelingToolWindowContext labeling_tool_context{
                root_dir,
                legacy_labeling_state,
                static_cast<int>(scene->num_cams),
                current_frame_num,
                triangulation_supported,
                legacy_labeling_state.nextLabeledFrameAfter(current_frame_num),
            };
            const LabelingToolWindowResult labeling_tool_result =
                drawLabelingToolWindow(labeling_tool_context,
                                       labeling_tool_window_state);
            const LabelingToolWorkflowContext labeling_tool_workflow_context{
                legacy_labeling_state,
                current_frame_num,
                camera_params,
                scene,
                camera_names,
                &input_is_imgs,
                imgs_names,
                error_message,
                show_error,
            };
            const LabelingToolWorkflowResult labeling_tool_workflow_result =
                applyLabelingToolWindowActions(labeling_tool_result,
                                               labeling_tool_workflow_context);

            if (labeling_tool_workflow_result.jump_target_frame.has_value()) {
                playback_session_controller.seekToFrame(
                    *labeling_tool_workflow_result.jump_target_frame, true);
            }

            frame_labeling_tool_ui_ms += durationMs(
                std::chrono::steady_clock::now() - labeling_tool_ui_start);
        }

        static TimelineScrollState shared_timeline_scroll_state;
        static StimulusEventTimelineWindowState stimulus_timeline_window_state;
        static AnalysisTimelineWindowState analysis_timeline_window_state;

        // Stimulus Event Timeline Window
        if (zarr_loaded) {
            const auto stimulus_timeline_ui_start =
                std::chrono::steady_clock::now();
            StimulusEventTimelineWindowContext stimulus_timeline_context{
                zarr_loader,
                shared_timeline_scroll_state,
                current_frame_num,
                video_fps,
            };
            StimulusEventTimelineWindowResult stimulus_timeline_result =
                drawStimulusEventTimelineWindow(stimulus_timeline_context,
                                               stimulus_timeline_window_state);
            if (stimulus_timeline_result.seek_target_frame.has_value()) {
                writeClippedPlaybackStateEvent(
                    "seek_request",
                    json{{"source", "stimulus_event_timeline"},
                         {"phase", "before"},
                         {"target_frame",
                          *stimulus_timeline_result.seek_target_frame}},
                    true);
                playback_session_controller.seekToFrame(
                    *stimulus_timeline_result.seek_target_frame, true);
                writeClippedPlaybackStateEvent(
                    "seek_request",
                    json{{"source", "stimulus_event_timeline"},
                         {"phase", "after"},
                         {"target_frame",
                          *stimulus_timeline_result.seek_target_frame}},
                    true);
            }
            frame_stimulus_timeline_ui_ms += durationMs(
                std::chrono::steady_clock::now() - stimulus_timeline_ui_start);
        }

        // Analysis timeline window
        if (zarr_loaded &&
            (zarr_loader.hasMovementData() ||
             zarr_loader.hasDeferredMovementData() ||
             zarr_loader.hasEyeAngleAnalysisData() ||
             zarr_loader.hasTailKinematicsData() ||
             zarr_loader.hasStimulusSteps() ||
             zarr_loader.hasStimulusEvents())) {
            const auto analysis_timeline_ui_start =
                std::chrono::steady_clock::now();
            AnalysisTimelineWindowContext analysis_timeline_context{
                zarr_loader,
                shared_timeline_scroll_state,
                current_frame_num,
                video_fps,
                &frame_analysis_timeline_perf,
            };
            drawAnalysisTimelineWindow(analysis_timeline_context,
                                       analysis_timeline_window_state);
            frame_movement_timeline_ui_ms += durationMs(
                std::chrono::steady_clock::now() - analysis_timeline_ui_start);
            frame_analysis_timeline_perf.total_window_ms =
                frame_movement_timeline_ui_ms;
        }

        shared_timeline_scroll_state.prev_enabled =
            shared_timeline_scroll_state.enabled;

        processHelpMenuShortcut(show_help_window);

        if (show_help_window) {
            const auto help_menu_ui_start = std::chrono::steady_clock::now();
            drawHelpMenuWindow(show_help_window);
            frame_help_menu_ui_ms +=
                durationMs(std::chrono::steady_clock::now() - help_menu_ui_start);
        }

        drawErrorPopup(show_error, error_message);

        // Rendering
        frame_ui_build_ms =
            durationMs(std::chrono::steady_clock::now() - ui_build_start);
        const auto imgui_render_start = std::chrono::steady_clock::now();
        ImGui::Render();
        frame_imgui_render_ms = durationMs(
            std::chrono::steady_clock::now() - imgui_render_start);
        ImDrawData* imgui_draw_data = ImGui::GetDrawData();
        if (imgui_draw_data != nullptr) {
            frame_imgui_draw_list_count = imgui_draw_data->CmdListsCount;
            frame_imgui_total_vtx_count = imgui_draw_data->TotalVtxCount;
            frame_imgui_total_idx_count = imgui_draw_data->TotalIdxCount;
            int draw_cmd_count = 0;
            for (int list_idx = 0; list_idx < imgui_draw_data->CmdListsCount;
                 ++list_idx) {
                const ImDrawList* draw_list = imgui_draw_data->CmdLists[list_idx];
                if (draw_list != nullptr) {
                    draw_cmd_count += draw_list->CmdBuffer.Size;
                }
            }
            frame_imgui_draw_cmd_count = draw_cmd_count;
        }
        int display_w, display_h;
        glfwGetFramebufferSize(window->render_target, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(clear_color.x * clear_color.w,
                     clear_color.y * clear_color.w,
                     clear_color.z * clear_color.w, clear_color.w);
        glClear(GL_COLOR_BUFFER_BIT);
        const auto gl_draw_start = std::chrono::steady_clock::now();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        frame_gl_draw_ms = durationMs(std::chrono::steady_clock::now() -
                                      gl_draw_start);
        if (clipped_frame_trace_log_writer.enabled() && zarr_loaded &&
            zarr_loader.hasClippedCollection() && scene != nullptr) {
            auto nullableInt = [](int64_t value) -> json {
                return value >= 0 ? json(value) : json(nullptr);
            };
            for (int camera_idx = 0;
                 camera_idx < static_cast<int>(scene->num_cams);
                 ++camera_idx) {
                auto& trace =
                    scene->cameras[camera_idx].texture_draw_trace;
                if (!trace.enabled || trace.queue_sequence == 0 ||
                    trace.last_logged_sequence == trace.queue_sequence) {
                    continue;
                }
                trace.last_logged_sequence = trace.queue_sequence;
                if (!trace.callback_observed) {
                    clipped_frame_trace_stats
                        .texture_draw_callback_missing++;
                } else if (!trace.callback_bound_matches_queued) {
                    clipped_frame_trace_stats
                        .texture_draw_bound_mismatches++;
                }
                clipped_frame_trace_log_writer.write(
                    json{
                        {"event", "clipped_texture_draw"},
                        {"draw_sequence", trace.queue_sequence},
                        {"view_idx", trace.view_idx},
                        {"queued_texture_id",
                         static_cast<uint64_t>(trace.queued_texture_id)},
                        {"front_texture_id",
                         static_cast<uint64_t>(trace.front_texture_id)},
                        {"staging_texture_id",
                         static_cast<uint64_t>(trace.staging_texture_id)},
                        {"front_pbo_id",
                         static_cast<uint64_t>(trace.front_pbo_id)},
                        {"staging_pbo_id",
                         static_cast<uint64_t>(trace.staging_pbo_id)},
                        {"queued_texture_matches_front",
                         trace.queued_texture_id == trace.front_texture_id},
                        {"queued_texture_matches_staging",
                         trace.queued_texture_id == trace.staging_texture_id},
                        {"front",
                         {{"valid", trace.front_valid},
                          {"parent_frame",
                           nullableInt(trace.front_parent_frame)},
                          {"local_frame",
                           nullableInt(trace.front_local_frame)},
                          {"pts", nullableInt(trace.front_pts)}}},
                        {"staging",
                         {{"valid", trace.staging_valid},
                          {"parent_frame",
                           nullableInt(trace.staging_parent_frame)},
                          {"local_frame",
                           nullableInt(trace.staging_local_frame)},
                          {"pts", nullableInt(trace.staging_pts)}}},
                        {"callback",
                         {{"observed", trace.callback_observed},
                          {"count", trace.callback_count},
                          {"active_texture",
                           trace.callback_observed
                               ? json(trace.callback_active_texture)
                               : json(nullptr)},
                          {"bound_texture_id",
                           trace.callback_observed
                               ? json(static_cast<uint64_t>(
                                     trace.callback_bound_texture_id))
                               : json(nullptr)},
                          {"bound_matches_queued",
                           trace.callback_observed
                               ? json(trace.callback_bound_matches_queued)
                               : json(nullptr)}}}},
                    /*force_flush=*/false);

                if (clipped_texture_dump.enabled &&
                    !clipped_texture_dump.dumped &&
                    trace.callback_observed &&
                    trace.callback_bound_texture_id != 0 &&
                    trace.front_parent_frame ==
                        clipped_texture_dump.parent_frame) {
                    clipped_texture_dump.dumped = true;
                    const GlTextureDumpResult dump_result =
                        dumpGlTextureToPng(trace.callback_bound_texture_id,
                                           clipped_texture_dump.output_path);

                    json resolver_json = nullptr;
                    if (trace.front_parent_frame >= 0) {
                        const auto* row = zarr_loader.resolveClippedFrame(
                            trace.front_parent_frame);
                        if (row != nullptr) {
                            resolver_json = json{
                                {"resolved_parent_frame_index",
                                 row->parent_frame_index},
                                {"recording_frame_id",
                                 row->recording_frame_id},
                                {"clip_id", row->clip_id},
                                {"clip_local_frame_index",
                                 row->clip_local_frame_index},
                                {"camera_serial", row->camera_serial},
                                {"selected_run_index",
                                 row->selected_run_index},
                            };
                        }
                    }

                    const std::filesystem::path metadata_path =
                        pathWithExtension(dump_result.raw_path, ".json");
                    json dump_event = {
                        {"event", "clipped_texture_dump"},
                        {"requested_parent_frame",
                         clipped_texture_dump.parent_frame},
                        {"ok", dump_result.ok},
                        {"error", dump_result.error.empty()
                                      ? json(nullptr)
                                      : json(dump_result.error)},
                        {"raw_path", dump_result.raw_path.string()},
                        {"flip_y_path", dump_result.flip_y_path.string()},
                        {"metadata_path", metadata_path.string()},
                        {"width", dump_result.width},
                        {"height", dump_result.height},
                        {"draw_sequence", trace.queue_sequence},
                        {"view_idx", trace.view_idx},
                        {"bound_texture_id",
                         static_cast<uint64_t>(
                             trace.callback_bound_texture_id)},
                        {"queued_texture_id",
                         static_cast<uint64_t>(trace.queued_texture_id)},
                        {"front_texture_id",
                         static_cast<uint64_t>(trace.front_texture_id)},
                        {"staging_texture_id",
                         static_cast<uint64_t>(trace.staging_texture_id)},
                        {"bound_matches_queued",
                         trace.callback_bound_matches_queued},
                        {"front",
                         {{"valid", trace.front_valid},
                          {"parent_frame",
                           nullableInt(trace.front_parent_frame)},
                          {"local_frame",
                           nullableInt(trace.front_local_frame)},
                          {"pts", nullableInt(trace.front_pts)}}},
                        {"staging",
                         {{"valid", trace.staging_valid},
                          {"parent_frame",
                           nullableInt(trace.staging_parent_frame)},
                          {"local_frame",
                           nullableInt(trace.staging_local_frame)},
                          {"pts", nullableInt(trace.staging_pts)}}},
                        {"resolver", resolver_json},
                    };

                    std::error_code metadata_ec;
                    if (metadata_path.has_parent_path()) {
                        std::filesystem::create_directories(
                            metadata_path.parent_path(), metadata_ec);
                    }
                    if (!metadata_ec) {
                        std::ofstream metadata_stream(
                            metadata_path,
                            std::ios::out | std::ios::trunc);
                        if (metadata_stream.is_open()) {
                            metadata_stream << dump_event.dump(2) << "\n";
                        } else {
                            dump_event["metadata_write_error"] =
                                "failed to open metadata file";
                        }
                    } else {
                        dump_event["metadata_write_error"] =
                            metadata_ec.message();
                    }

                    clipped_frame_trace_log_writer.write(
                        dump_event, /*force_flush=*/true);
                    if (dump_result.ok) {
                        std::cout << "[ClippedTextureDump] Wrote "
                                  << dump_result.raw_path << " and "
                                  << dump_result.flip_y_path << std::endl;
                    } else {
                        std::cerr << "[ClippedTextureDump] Failed: "
                                  << dump_result.error << std::endl;
                    }
                }
            }
        }

        // Update and Render additional Platform Windows
        // (Platform functions may change the current OpenGL context, so we
        // save/restore it to make it easier to paste this code elsewhere.
        //  For this specific demo app we could also call
        //  glfwMakeContextCurrent(window) directly)
//         if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
//             GLFWwindow *backup_current_context = glfwGetCurrentContext();
//             ImGui::UpdatePlatformWindows();
//             ImGui::RenderPlatformWindowsDefault();
//             glfwMakeContextCurrent(backup_current_context);
        const auto swap_start = std::chrono::steady_clock::now();
        glfwSwapBuffers(window->render_target);
        frame_swap_ms =
            durationMs(std::chrono::steady_clock::now() - swap_start);

        const bool playback_was_just_seeked = ps.just_seeked;
        if (playback_was_just_seeked) {
            ps.just_seeked = false;
        }
        if (dc_context->decoding_flag && ps.play_video &&
            scene->size_of_buffer > 0) {
            playback_commit_previous_frame = ps.to_display_frame_number;
            const bool presented_from_slot =
                playback_trace_presented_slot >= 0 &&
                playback_trace_presented_frame >= 0;
            const bool commit_presented_frame =
                presented_from_slot &&
                playback_trace_presented_frame >=
                    playback_commit_previous_frame;

            if (commit_presented_frame) {
                playback_commit_frame = playback_trace_presented_frame;
                playback_commit_slot = playback_trace_presented_slot;
                if (playback_commit_slot < 0) {
                    playback_commit_slot = findExactBufferedFrameSlot(
                        playback_commit_frame,
                        ps.read_head %
                            static_cast<int>(scene->size_of_buffer));
                }
                ps.to_display_frame_number = playback_commit_frame;
                ps.slider_frame_number = playback_commit_frame;
                if (playback_commit_slot >= 0) {
                    ps.read_head = playback_commit_slot;
                }

                for (int slot_idx = 0;
                     slot_idx < static_cast<int>(scene->size_of_buffer);
                     ++slot_idx) {
                    for (int cam_idx = 0;
                         cam_idx < static_cast<int>(scene->num_cams);
                         ++cam_idx) {
                        auto& slot =
                            scene->cameras[cam_idx].display_buffer[slot_idx];
                        auto metadata = frameSlotSnapshotReadable(slot);
                        if (!metadata ||
                            metadata->frame_number >= playback_commit_frame) {
                            continue;
                        }
                        ++playback_release_attempts;
                        if (frameSlotTryReleaseForReuse(
                                slot, metadata->frame_number)) {
                            ++playback_release_count;
                        } else {
                            ++playback_release_skip_count;
                        }
                    }
                }
            } else {
                playback_release_deferred =
                    playback_presenter_target_frame >
                    playback_commit_previous_frame;
            }

            if (playback_commit_frame >= 0 ||
                playback_release_attempts > 0 ||
                playback_release_deferred ||
                playback_target_clamped_to_buffer ||
                playback_was_just_seeked) {
                json commit_details{
                    {"requested_frame", playback_requested_camera_frame},
                    {"presenter_target_frame",
                     playback_presenter_target_frame},
                    {"presenter_target_slot",
                     playback_presenter_target_slot},
                    {"target_clamped_to_buffer",
                     playback_target_clamped_to_buffer},
                    {"previous_committed_frame",
                     playback_commit_previous_frame},
                    {"committed_frame", playback_commit_frame},
                    {"committed_slot", playback_commit_slot},
                    {"presented_from_slot", presented_from_slot},
                    {"presented_frame", playback_trace_presented_frame},
                    {"presented_slot", playback_trace_presented_slot},
                    {"release_attempts", playback_release_attempts},
                    {"release_count", playback_release_count},
                    {"release_skip_count", playback_release_skip_count},
                    {"release_deferred", playback_release_deferred},
                    {"was_just_seeked", playback_was_just_seeked},
                };
                writePlaybackTraceEvent(
                    "playback_present_commit",
                    commit_details,
                    playback_trace_presenter_view_idx,
                    playback_presenter_target_frame,
                    playback_trace_presenter_preferred_paused_slot,
                    playback_trace_presented_slot,
                    playback_trace_presented_frame,
                    playback_trace_presenter_resolved_frame,
                    playback_trace_prewarm_active);
                if (clipped_collection_playback) {
                    writeClippedPlaybackStateEvent(
                        "playback_present_commit",
                        commit_details,
                        false);
                }
            }
        }

        if (zarr_loaded) {
            zarr_loader.requestRefinedSubjectMaskOptionalOverlayPrefetch();
        }

        if (cli_frame_cap_fps > 0.0) {
            const auto target_period =
                std::chrono::duration<double>(1.0 / cli_frame_cap_fps);
            const auto target_end =
                frame_loop_start +
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    target_period);
            const auto before_sleep = std::chrono::steady_clock::now();
            if (before_sleep < target_end) {
                std::this_thread::sleep_until(target_end);
                frame_cap_sleep_ms =
                    durationMs(std::chrono::steady_clock::now() - before_sleep);
            }
        }

        const bool perf_playback_start_warmup_active =
            ps.play_video && perf_playback_start_frame >= 0 &&
            perf_frames_since_playback_start < kPlaybackWarmupPerfFrames;
        const int perf_frames_since_playback_start_value =
            perf_playback_start_frame >= 0
                ? static_cast<int>(std::min<uint64_t>(
                      perf_frames_since_playback_start,
                      static_cast<uint64_t>(
                          std::numeric_limits<int>::max())))
                : -1;
        const PerfLogFrameContext perf_frame_context{
            camera_names,
            cwd,
            argv0_path,
            cli_recording_path,
            cli_zarr_override_path,
            ps.play_video,
            set_playback_speed,
            inst_speed,
            video_fps,
            perf_requested_camera_frame,
            ps.to_display_frame_number,
            current_frame_num,
            perf_min_decoded_camera_frame,
            perf_playback_start_warmup_active,
            perf_frames_since_playback_start_value,
            perf_playback_start_frame,
            perf_playback_resume_path,
            perf_playback_resume_target_frame,
            scene->use_cpu_buffer,
            static_cast<int>(scene->size_of_buffer),
            label_buffer_size,
            video_loaded,
            playbackPreviewScaleLabel(playback_preview_scale_mode),
            playbackPreviewIsActive(ps.play_video, yolo_detection,
                                    playback_preview_scale_mode),
            playbackRendererModeLabel(playback_renderer_mode),
            static_cast<int>(perf_camera_viewport_width_px),
            static_cast<int>(perf_camera_viewport_height_px),
            perf_camera_view_x_min,
            perf_camera_view_x_max,
            perf_camera_view_y_min,
            perf_camera_view_y_max,
            perf_camera_view_visible_fraction,
            perf_camera_view_zoomed_in,
            frame_camera_upload_count,
            frame_camera_upload_ms,
            frame_camera_texture_resize_ms,
            frame_camera_preview_resize_ms,
            frame_camera_display_convert_ms,
            frame_camera_pbo_copy_ms,
            frame_camera_texture_upload_ms,
            frame_camera_playback_front_path_ms,
            frame_camera_playback_stage_total_ms,
            frame_camera_playback_stage_upload_ms,
            frame_camera_playback_prewarm_total_ms,
            frame_camera_playback_prewarm_upload_ms,
            frame_camera_playback_prewarm_count,
            frame_camera_playback_swap_ms,
            frame_camera_plot_image_ui_ms,
            frame_camera_overlay_ui_ms,
            frame_subject_shape_overlay_ms,
            frame_tail_kinematics_overlay_ms,
            frame_camera_scene_ui_ms,
            frame_file_browser_ui_ms,
            frame_frame_debug_ui_ms,
            frame_buffer_window_ui_ms,
            frame_crop_preview_ui_ms,
            frame_crop_preview_perf,
            frame_stimulus_buffer_window_ui_ms,
            frame_keypoints_window_ui_ms,
            frame_labeling_tool_ui_ms,
            frame_stimulus_window_ui_ms,
            frame_stimulus_timeline_ui_ms,
            frame_movement_timeline_ui_ms,
            frame_help_menu_ui_ms,
            frame_gl_draw_ms,
            frame_swap_ms,
            frame_cap_sleep_ms,
            cli_frame_cap_fps,
            frame_ui_build_ms,
            frame_imgui_render_ms,
            frame_imgui_draw_cmd_count,
            frame_imgui_draw_list_count,
            frame_imgui_total_vtx_count,
            frame_imgui_total_idx_count,
            &stimulus_player,
            stimulus_use_software_decode,
            stimulus_use_cpu_buffer,
            stimulus_buffer_size,
            ps.current_stimulus_frame,
            latest_decoded_frame[stimulus_player.window_name].load(),
            static_cast<int>(window->swap_interval),
            static_cast<int>(window->width),
            static_cast<int>(window->height),
            frame_loop_start,
            &frame_analysis_timeline_perf,
            frame_bbox_query_frame,
            frame_bbox_loaded_count,
            frame_bbox_display_count,
            frame_bbox_get_boxes_ms,
            frame_bbox_edit_resolve_ms,
            frame_bbox_get_raw_detections_ms,
            frame_bbox_load_total_ms,
            frame_bbox_overlay_build_ms,
            frame_bbox_overlay_draw_ms,
            frame_bbox_overlay_item_count,
        };
        maybeWritePerfLogSample(
            perf_log_writer,
            perf_frame_context,
            kPerfLogSamplePeriod);
        if (playback_trace_log_writer.enabled() && video_loaded &&
            (!ps.play_video || seek_progress.state != SeekState::Idle)) {
            writePlaybackTraceEvent(
                "frame",
                json{{"frame_loop_ms",
                      durationMs(std::chrono::steady_clock::now() -
                                 frame_loop_start)},
                     {"camera_upload_count", frame_camera_upload_count},
                     {"camera_upload_ms", frame_camera_upload_ms},
                     {"prewarm_count",
                      frame_camera_playback_prewarm_count},
                     {"prewarm_ms",
                      frame_camera_playback_prewarm_total_ms},
                     {"frame_cap_sleep_ms", frame_cap_sleep_ms}},
                playback_trace_presenter_view_idx,
                playback_trace_presenter_target_frame,
                playback_trace_presenter_preferred_paused_slot,
                playback_trace_presented_slot,
                playback_trace_presented_frame,
                playback_trace_presenter_resolved_frame,
                playback_trace_prewarm_active);
        }

        int32_t selected_mask_roi_index = -1;
        std::string selected_mask_component_name;
        if (frame_debug_window_state.subject_mask_edit_session.active()) {
            const auto& target =
                frame_debug_window_state.subject_mask_edit_session.target();
            selected_mask_roi_index = target.roi_index;
            selected_mask_component_name = target.component_name;
        }
        const bool periodic_mask_perf_sample =
            (mask_perf_sample_index++ %
             static_cast<uint64_t>(cli_mask_perf_sample_every)) == 0;
        const bool playback_warmup_mask_perf_sample =
            perf_playback_start_warmup_active &&
            ((perf_frames_since_playback_start %
              kPlaybackWarmupPerfSampleStride) == 0);
        const bool playback_prewarm_mask_perf_sample =
            frame_camera_playback_prewarm_count > 0;
        const bool should_write_mask_perf_sample =
            periodic_mask_perf_sample || playback_warmup_mask_perf_sample ||
            playback_prewarm_mask_perf_sample;
        if (should_write_mask_perf_sample) {
            writeMaskPerfLogSample(
                mask_perf_log_writer,
                MaskPerfLogFrameContext{
                    cwd,
                    argv0_path,
                    cli_recording_path,
                    cli_zarr_override_path,
                    zarr_loaded ? zarr_loader.getArchivePath() : std::string{},
                    current_frame_num,
                    ps.to_display_frame_number,
                    ps.play_video,
                    zarr_loaded && show_eye_masks && zarr_loader.hasEyeMasks(),
                    zarr_loaded,
                    cli_mask_perf_sample_every,
                    playback_warmup_mask_perf_sample,
                    zarr_loaded ? zarr_loader.getEyeMaskSourceLabel()
                                : std::string{},
                    zarr_loaded ? zarr_loader.getEyeMaskSourcePath()
                                : std::string{},
                    zarr_loaded ? zarr_loader.getEyeMaskRunName()
                                : std::string{},
                    selected_mask_roi_index,
                    selected_mask_component_name,
                    frame_mask_data_load_ms,
                    frame_mask_overlay_perf,
                    &perf_frame_context,
                    frame_loop_start,
                });
        }
        if (ps.play_video && perf_playback_start_frame >= 0) {
            perf_frames_since_playback_start++;
        } else if (!ps.play_video && perf_playback_start_frame >= 0) {
            resetPlaybackStartPerf();
        }
    }

    writeClippedFrameTraceSummary("shutdown");

    // Cleanup
    destroyStimulusPlayback(stimulus_player);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window->render_target);
    glfwTerminate();

    dc_context->stop_flag = true;
    // wait for threads to join
    for (auto &t : decoder_threads)
        t.join();

    return app_exit_code;
}
