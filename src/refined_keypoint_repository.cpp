#include "refined_keypoint_repository.h"

#include "zarr_loader_internal.h"

#include <openssl/sha.h>

namespace {

using json = nlohmann::json;

bool isAllowedValue(const std::string& value,
                    std::initializer_list<const char*> allowed) {
    for (const char* candidate : allowed) {
        if (value == candidate) {
            return true;
        }
    }
    return false;
}

std::optional<std::string> sha256Hex(const std::string& payload) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    if (SHA256(reinterpret_cast<const unsigned char*>(payload.data()),
               payload.size(),
               digest) == nullptr) {
        return std::nullopt;
    }

    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.resize(SHA256_DIGEST_LENGTH * 2);
    for (size_t i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        out[i * 2] = kHex[(digest[i] >> 4) & 0x0F];
        out[i * 2 + 1] = kHex[digest[i] & 0x0F];
    }
    return out;
}

json getNestedObjectOrNull(const json& source, const char* key) {
    if (source.contains(key) && source[key].is_object()) {
        return source[key];
    }
    return nullptr;
}

json buildKeypointSignature(const json& attrs) {
    json params = getNestedObjectOrNull(attrs, "parameters");
    if (!params.is_object()) {
        const json provenance = getNestedObjectOrNull(attrs, "provenance");
        if (provenance.is_object() &&
            provenance.contains("parameters") &&
            provenance["parameters"].is_object()) {
            params = provenance["parameters"];
        }
    }

    json parameter_source = nullptr;
    if (attrs.contains("parameter_source")) {
        parameter_source = attrs["parameter_source"];
    } else if (params.is_object() &&
               params.contains("parameter_source")) {
        parameter_source = params["parameter_source"];
    }

    json parameters_hash = nullptr;
    if (params.is_object()) {
        if (const auto digest = sha256Hex(params.dump());
            digest.has_value()) {
            parameters_hash = *digest;
        }
    }

    return json{
        {"signature_version", 1},
        {"source_keypoints_run", attrs.value("source_keypoints_run", json(nullptr))},
        {"source_crop_run", attrs.value("source_crop_run", json(nullptr))},
        {"source_detect_run", attrs.value("source_detect_run", json(nullptr))},
        {"source_refined_run", attrs.value("source_refined_run", json(nullptr))},
        {"parameter_source", parameter_source},
        {"parameters_hash", parameters_hash},
    };
}

}  // namespace

bool RefinedKeypointRepository::canEditActiveRun(std::string* reason) const {
    if (!loader_.hasKeypointData()) {
        if (reason != nullptr) {
            *reason = "No keypoint run is loaded.";
        }
        return false;
    }
    if (loader_.getKeypointsRunName().empty()) {
        if (reason != nullptr) {
            *reason = "Loaded keypoints have no resolved run name.";
        }
        return false;
    }
    if (!loader_.isRefinedKeypoints()) {
        if (reason != nullptr) {
            *reason = "Keypoints are loaded from a raw run; refined runs are required for in-place edits.";
        }
        return false;
    }
    if (reason != nullptr) {
        reason->clear();
    }
    return true;
}

RefinedKeypointSelection
RefinedKeypointRepository::resolveFrameDetectionSelection(
    size_t frame_id,
    size_t detection_index,
    bool use_interpolated) const {
    RefinedKeypointSelection selection;
    selection.frame_id = frame_id;
    selection.detection_index = detection_index;
    selection.run_name = loader_.getKeypointsRunName();
    selection.editable = loader_.isRefinedKeypoints();

    if (!loader_.hasKeypointData()) {
        selection.message = "No keypoint run is loaded.";
        return selection;
    }
    if (selection.run_name.empty()) {
        selection.message = "Loaded keypoints have no resolved run name.";
        return selection;
    }

    selection.roi_metadata = loader_.getKeypointRoiMetadataForFrameDetection(
        frame_id, detection_index, use_interpolated);
    selection.roi_index = selection.roi_metadata.roi_index;
    if (!selection.roi_metadata.valid) {
        selection.message =
            use_interpolated
                ? "Interpolated detections do not have a stable keypoint ROI mapping."
                : "Selected detection has no keypoint ROI mapping.";
        return selection;
    }

    selection.valid = true;
    if (!selection.roi_metadata.has_crop_metadata) {
        selection.message =
            "ROI row resolved, but crop placement metadata is unavailable.";
    }
    return selection;
}

bool RefinedKeypointRepository::writeReviewStatus(
    const RefinedKeypointReviewStatusWriteOptions& options,
    std::string& error_message,
    std::string* resolved_run_name) const {
    error_message.clear();

    std::string edit_reason;
    if (!canEditActiveRun(&edit_reason)) {
        error_message = edit_reason;
        return false;
    }
    if (!isAllowedValue(options.state,
                        {"approved", "pending", "rejected", "needs_review"})) {
        error_message =
            "Review state must be one of approved|pending|rejected|needs_review.";
        return false;
    }
    if (!isAllowedValue(options.method,
                        {"manual", "algorithmic", "hybrid", "spotcheck"})) {
        error_message =
            "Review method must be one of manual|algorithmic|hybrid|spotcheck.";
        return false;
    }
    if (!isAllowedValue(options.intended_use,
                        {"training", "full_recording"})) {
        error_message =
            "Intended use must be one of training|full_recording.";
        return false;
    }

    const std::string archive_path = loader_.getArchivePath();
    if (archive_path.empty()) {
        error_message = "No loaded Zarr archive to write into.";
        return false;
    }

    const std::string kvstore_path = normalizeKvstoreFileRootPath(archive_path);
    auto kv_spec = ts::kvstore::Spec::FromJson(
        {{"driver", "file"}, {"path", kvstore_path}});
    if (!kv_spec.ok()) {
        error_message =
            "Failed to create kvstore spec: " + kv_spec.status().ToString();
        return false;
    }
    auto store_result = ts::kvstore::Open(kv_spec.value()).result();
    if (!store_result.ok()) {
        error_message =
            "Failed to open kvstore: " + store_result.status().ToString();
        return false;
    }
    const auto store = store_result.value();

    std::string parent_name;
    if (auto attrs = readAttrsAny(store, "refined_keypoints_runs");
        attrs.has_value() && attrs->is_object()) {
        parent_name = "refined_keypoints_runs";
    } else if (auto attrs = readAttrsAny(store, "keypoints_refined_runs");
               attrs.has_value() && attrs->is_object()) {
        parent_name = "keypoints_refined_runs";
    } else {
        error_message =
            "No refined_keypoints_runs parent exists in the loaded archive.";
        return false;
    }

    const std::string run_name = loader_.getKeypointsRunName();
    if (run_name.empty()) {
        error_message = "Loaded refined keypoints have no resolved run name.";
        return false;
    }
    if (resolved_run_name != nullptr) {
        *resolved_run_name = run_name;
    }

    const std::string run_path = parent_name + "/" + run_name;
    auto run_meta = readNodeMetaV3(store, run_path);
    if (!run_meta.has_value()) {
        error_message =
            "Refined keypoint run metadata is missing: " + run_path;
        return false;
    }
    json normalized_run_meta = normalizeGroupMetadataV3(*run_meta);
    json& run_attrs = normalized_run_meta["attributes"];

    const std::string timestamp_utc = currentUtcIsoTimestamp();
    json payload = {
        {"state", options.state},
        {"method", options.method},
        {"intended_use", options.intended_use},
        {"timestamp_utc", timestamp_utc},
        {"timestamp", timestamp_utc},
    };
    if (!options.reviewer.empty()) {
        payload["reviewer"] = options.reviewer;
    }
    if (!options.notes.empty()) {
        payload["notes"] = options.notes;
    }

    run_attrs["keypoint_review_status"] = payload;
    json signature = getNestedObjectOrNull(run_attrs, "keypoint_signature");
    if (!signature.is_object()) {
        signature = buildKeypointSignature(run_attrs);
        run_attrs["keypoint_signature"] = signature;
    }
    run_attrs["keypoint_review_signature"] = signature;

    if (!writeNodeMetaV3(store, run_path, normalized_run_meta, &error_message)) {
        return false;
    }

    if (options.update_latest) {
        json parent_meta = normalizeGroupMetadataV3(
            readNodeMetaV3(store, parent_name).value_or(makeEmptyGroupMetadataV3()));
        parent_meta["attributes"]["keypoint_review_status_latest"] = run_name;
        if (!writeNodeMetaV3(store, parent_name, parent_meta, &error_message)) {
            return false;
        }
    }

    return true;
}
