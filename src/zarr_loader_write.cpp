#include "zarr_loader_internal.h"
#include <iostream>

using json = nlohmann::json;

bool ZarrDetectionLoader::writeManualRefinedDetections(
    const std::vector<int32_t>& frame_indices,
    const std::vector<std::array<double, 4>>& bbox_norm_coords,
    const std::vector<float>& scores,
    const std::vector<int32_t>& class_ids,
    const std::vector<int32_t>& frame_counts,
    const std::vector<int8_t>& detection_source,
    const std::vector<std::string>& reason_labels,
    const std::string& manual_group,
    const std::string& source_variant,
    std::string& error_message,
    std::string* resolved_refined_run,
    const ManualWriteReviewOptions& review_options) {
    error_message.clear();

    if (root_path_.empty()) {
        error_message = "No loaded Zarr archive to write into.";
        return false;
    }
    if (frame_counts.empty()) {
        error_message = "frame_counts is empty.";
        return false;
    }

    const size_t n_detections = frame_indices.size();
    if (bbox_norm_coords.size() != n_detections ||
        scores.size() != n_detections ||
        class_ids.size() != n_detections) {
        error_message =
            "Detection-level payload arrays have inconsistent lengths.";
        return false;
    }
    if (!detection_source.empty() && detection_source.size() != n_detections) {
        error_message = "detection_source length mismatch.";
        return false;
    }
    if (!reason_labels.empty() && reason_labels.size() != n_detections) {
        error_message = "reason length mismatch.";
        return false;
    }

    if (review_options.intended_use != "training" &&
        review_options.intended_use != "full_recording") {
        error_message = "review_options.intended_use must be \"training\" or \"full_recording\", got \"" +
                        review_options.intended_use + "\".";
        return false;
    }
    if (review_options.state != "approved" && review_options.state != "pending" &&
        review_options.state != "rejected" && review_options.state != "needs_review") {
        error_message = "review_options.state must be one of approved|pending|rejected|needs_review, got \"" +
                        review_options.state + "\".";
        return false;
    }
    if (review_options.method != "manual" && review_options.method != "algorithmic" &&
        review_options.method != "hybrid" && review_options.method != "spotcheck") {
        error_message = "review_options.method must be one of manual|algorithmic|hybrid|spotcheck, got \"" +
                        review_options.method + "\".";
        return false;
    }

    std::string manual_group_name = manual_group;
    while (!manual_group_name.empty() && manual_group_name.front() == '/') {
        manual_group_name.erase(manual_group_name.begin());
    }
    while (!manual_group_name.empty() && manual_group_name.back() == '/') {
        manual_group_name.pop_back();
    }
    if (manual_group_name.empty()) {
        manual_group_name = "manual";
    }

    std::vector<int32_t> frame_counts_sanitized = frame_counts;
    int64_t frame_total = 0;
    for (size_t i = 0; i < frame_counts_sanitized.size(); ++i) {
        if (frame_counts_sanitized[i] < 0) {
            error_message = "frame_counts contains negative values.";
            return false;
        }
        frame_total += static_cast<int64_t>(frame_counts_sanitized[i]);
    }
    if (frame_total != static_cast<int64_t>(n_detections)) {
        error_message =
            "sum(frame_counts) does not match detection row count.";
        return false;
    }

    std::vector<int32_t> counts_from_rows(frame_counts_sanitized.size(), 0);
    for (int32_t frame_id : frame_indices) {
        if (frame_id < 0 ||
            static_cast<size_t>(frame_id) >= frame_counts_sanitized.size()) {
            error_message =
                "frame_indices contains values outside frame_counts bounds.";
            return false;
        }
        ++counts_from_rows[static_cast<size_t>(frame_id)];
    }
    if (counts_from_rows != frame_counts_sanitized) {
        error_message =
            "frame_counts does not match row-wise frame_indices counts.";
        return false;
    }

    std::vector<float> scores_sanitized = scores;
    for (float& score : scores_sanitized) {
        if (!std::isfinite(score)) {
            score = 1.0f;
        }
    }

    std::vector<double> bbox_flat;
    bbox_flat.reserve(n_detections * 4);
    for (const auto& row : bbox_norm_coords) {
        for (double value : row) {
            if (!std::isfinite(value)) {
                error_message = "bbox_norm_coords contains non-finite values.";
                return false;
            }
            bbox_flat.push_back(std::clamp(value, 0.0, 1.0));
        }
    }

    std::vector<int8_t> detection_source_sanitized;
    detection_source_sanitized.reserve(n_detections);
    if (detection_source.empty()) {
        detection_source_sanitized.assign(n_detections, 0);
    } else {
        for (int8_t value : detection_source) {
            detection_source_sanitized.push_back(value != 0 ? 1 : 0);
        }
    }

    std::vector<std::string> reasons = reason_labels;
    reasons.resize(n_detections);
    size_t clean_rows = 0;
    size_t interpolated_rows = 0;
    size_t manual_rows = 0;
    for (size_t i = 0; i < reasons.size(); ++i) {
        std::string lowered = toLowerCopy(reasons[i]);
        if (lowered.empty()) {
            lowered = detection_source_sanitized[i] != 0
                          ? "interpolated"
                          : "clean";
        }
        if (lowered.find("manual") != std::string::npos) {
            reasons[i] = "manual";
            detection_source_sanitized[i] = 0;
            ++manual_rows;
        } else if (lowered.find("interp") != std::string::npos) {
            reasons[i] = "interpolated";
            detection_source_sanitized[i] = 1;
            ++interpolated_rows;
        } else {
            reasons[i] = "clean";
            detection_source_sanitized[i] = 0;
            ++clean_rows;
        }
    }

    size_t reason_bytes_width = 16;
    for (const auto& reason : reasons) {
        reason_bytes_width = std::max(reason_bytes_width, reason.size() + 1);
    }
    std::vector<uint8_t> reason_bytes(n_detections * reason_bytes_width, 0);
    for (size_t i = 0; i < n_detections; ++i) {
        const std::string& reason = reasons[i];
        const size_t row_offset = i * reason_bytes_width;
        const size_t copy_len = std::min(reason.size(), reason_bytes_width - 1);
        if (copy_len > 0) {
            std::memcpy(reason_bytes.data() + row_offset, reason.data(), copy_len);
        }
        reason_bytes[row_offset + copy_len] = 0;
    }

    std::vector<int32_t> frame_mapping = frame_indices;
    std::vector<int32_t> n_detections_alias = frame_counts_sanitized;
    std::vector<int32_t> retune_id(n_detections, -1);

    const std::string kvstore_path = normalizeKvstoreFileRootPath(root_path_);
    auto kv_spec = ts::kvstore::Spec::FromJson(
        {{"driver", "file"}, {"path", kvstore_path}});
    if (!kv_spec.ok()) {
        error_message =
            "Failed to create kvstore spec: " + kv_spec.status().ToString();
        return false;
    }
    auto store_result = ts::kvstore::Open(kv_spec.value(), context_).result();
    if (!store_result.ok()) {
        error_message =
            "Failed to open kvstore: " + store_result.status().ToString();
        return false;
    }
    const auto store = store_result.value();

    auto refined_group_attrs = readAttrsAny(store, "refined_detect_runs");
    if (!refined_group_attrs.has_value() || !refined_group_attrs->is_object()) {
        error_message = "refined_detect_runs group is missing.";
        return false;
    }
    const std::string refined_run = extractLatestRunName(*refined_group_attrs);
    if (refined_run.empty()) {
        error_message = "refined_detect_runs has no latest run pointer.";
        return false;
    }
    if (resolved_refined_run) {
        *resolved_refined_run = refined_run;
    }

    const std::string refined_run_path = "refined_detect_runs/" + refined_run + "/";
    auto refined_run_meta = readNodeMetaV3(store, refined_run_path);
    if (!refined_run_meta.has_value()) {
        error_message =
            "Latest refined run metadata is missing: " + refined_run_path;
        return false;
    }

    std::string source_variant_value = toLowerCopy(source_variant);
    if (source_variant_value != "filtered" &&
        source_variant_value != "interpolated") {
        source_variant_value = (active_dataset_ == DetectionDataset::RefinedFiltered)
                                   ? "filtered"
                                   : "interpolated";
    }

    const std::string manual_group_path = refined_run_path + manual_group_name + "/";
    auto delete_result =
        ts::kvstore::DeleteRange(store, ts::KeyRange::Prefix(manual_group_path))
            .result();
    if (!delete_result.ok()) {
        error_message =
            "Failed to clear existing manual subgroup: " +
            delete_result.status().ToString();
        return false;
    }

    const ts::Index det_chunk = std::max<ts::Index>(
        1, std::min<ts::Index>(1000, static_cast<ts::Index>(n_detections)));
    const ts::Index frame_chunk = std::max<ts::Index>(
        1, std::min<ts::Index>(10000,
                               static_cast<ts::Index>(frame_counts_sanitized.size())));
    const ts::Index reason_width_index =
        static_cast<ts::Index>(reason_bytes_width);

    if (!writeNumericArray1D<int32_t>(
            store,
            context_,
            manual_group_path + "frame_indices",
            "int32",
            frame_indices,
            det_chunk,
            0,
            true,
            &error_message)) {
        return false;
    }
    if (!writeNumericArray2DFlat<double>(
            store,
            context_,
            manual_group_path + "bbox_norm_coords",
            "float64",
            bbox_flat,
            static_cast<ts::Index>(n_detections),
            4,
            det_chunk,
            4,
            0.0,
            true,
            &error_message)) {
        return false;
    }
    if (!writeNumericArray1D<float>(
            store,
            context_,
            manual_group_path + "scores",
            "float32",
            scores_sanitized,
            det_chunk,
            0.0f,
            true,
            &error_message)) {
        return false;
    }
    if (!writeNumericArray1D<int32_t>(
            store,
            context_,
            manual_group_path + "class_ids",
            "int32",
            class_ids,
            det_chunk,
            0,
            true,
            &error_message)) {
        return false;
    }
    if (!writeNumericArray1D<int32_t>(
            store,
            context_,
            manual_group_path + "frame_counts",
            "int32",
            frame_counts_sanitized,
            frame_chunk,
            0,
            true,
            &error_message)) {
        return false;
    }
    if (!writeNumericArray1D<int32_t>(
            store,
            context_,
            manual_group_path + "n_detections",
            "int32",
            n_detections_alias,
            frame_chunk,
            0,
            true,
            &error_message)) {
        return false;
    }
    if (!writeNumericArray1D<int32_t>(
            store,
            context_,
            manual_group_path + "frame_mapping",
            "int32",
            frame_mapping,
            det_chunk,
            0,
            true,
            &error_message)) {
        return false;
    }
    if (!writeNumericArray1D<int8_t>(
            store,
            context_,
            manual_group_path + "detection_source",
            "int8",
            detection_source_sanitized,
            det_chunk,
            0,
            false,
            &error_message)) {
        return false;
    }
    if (!writeNumericArray2DFlat<uint8_t>(
            store,
            context_,
            manual_group_path + "reason_bytes",
            "uint8",
            reason_bytes,
            static_cast<ts::Index>(n_detections),
            reason_width_index,
            det_chunk,
            reason_width_index,
            0,
            false,
            &error_message)) {
        return false;
    }
    if (!writeNumericArray1D<int32_t>(
            store,
            context_,
            manual_group_path + "retune_id",
            "int32",
            retune_id,
            det_chunk,
            -1,
            true,
            &error_message)) {
        return false;
    }

    const std::string timestamp = currentUtcIsoTimestamp();
    json manual_meta = makeEmptyGroupMetadataV3();
    auto& manual_attrs = manual_meta["attributes"];
    manual_attrs["reason_encoding"] = "utf8-null-terminated";
    manual_attrs["reason_bytes_width"] = static_cast<int64_t>(reason_bytes_width);
    manual_attrs["reason_bytes_null_terminated"] = true;
    manual_attrs["reason_fallback_order"] =
        json::array({"reason_bytes", "reason", "detection_source"});
    manual_attrs["storage_layout"] = "columnar";
    const json manual_fields = json::array(
        {"frame_indices",
         "bbox_norm_coords",
         "scores",
         "class_ids",
         "frame_counts",
         "n_detections",
         "frame_mapping",
         "detection_source",
         "reason_bytes",
         "retune_id"});
    manual_attrs["column_fields"] = manual_fields;
    manual_attrs["field_names"] = manual_fields;
    manual_attrs["total_detections"] = static_cast<int64_t>(n_detections);
    manual_attrs["clean_detections"] = static_cast<int64_t>(clean_rows);
    manual_attrs["interpolated_detections"] =
        static_cast<int64_t>(interpolated_rows);
    manual_attrs["manual_detections"] = static_cast<int64_t>(manual_rows);
    manual_attrs["detection_source_type"] = "manual";
    manual_attrs["detection_source_path"] =
        "refined_detect_runs/" + refined_run + "/" + manual_group_name;
    manual_attrs["source_refined_run"] = refined_run;
    manual_attrs["source_variant"] = source_variant_value;
    manual_attrs["manual_review_timestamp"] = timestamp;
    if (!writeNodeMetaV3(store, manual_group_path, manual_meta, &error_message)) {
        return false;
    }

    json run_meta = normalizeGroupMetadataV3(*refined_run_meta);
    auto& run_attrs = run_meta["attributes"];
    run_attrs["manual_review_latest"] = manual_group_name;
    json review_status = json{
        {"state", review_options.state},
        {"method", review_options.method},
        {"intended_use", review_options.intended_use},
        {"timestamp", timestamp},
        {"resolved_group", manual_group_name},
        {"preference_chain",
         json::array({"manual", "interpolated", "filtered", "raw"})}};
    if (!review_options.reviewer.empty()) {
        review_status["reviewer"] = review_options.reviewer;
    }
    if (!review_options.notes.empty()) {
        review_status["notes"] = review_options.notes;
    }
    run_attrs["detect_review_status"] = std::move(review_status);
    if (!writeNodeMetaV3(store, refined_run_path, run_meta, &error_message)) {
        return false;
    }

    json refined_group_meta = normalizeGroupMetadataV3(
        readNodeMetaV3(store, "refined_detect_runs")
            .value_or(makeEmptyGroupMetadataV3()));
    refined_group_meta["attributes"]["detect_review_status_latest"] = refined_run;
    if (!writeNodeMetaV3(store,
                         "refined_detect_runs",
                         refined_group_meta,
                         &error_message)) {
        return false;
    }

    return true;
}

