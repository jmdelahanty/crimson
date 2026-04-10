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
    (void)manual_group;

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
        review_options.method != "hybrid" && review_options.method != "spotcheck" &&
        review_options.method != "retune") {
        error_message = "review_options.method must be one of manual|algorithmic|hybrid|spotcheck|retune, got \"" +
                        review_options.method + "\".";
        return false;
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

    if (data_.image_width <= 0 || data_.image_height <= 0) {
        error_message = "Image dimensions are unavailable for bbox_img_xyxy writes.";
        return false;
    }

    std::vector<double> bbox_flat;
    std::vector<double> bbox_img_xyxy_flat;
    bbox_flat.reserve(n_detections * 4);
    bbox_img_xyxy_flat.reserve(n_detections * 4);
    for (const auto& row : bbox_norm_coords) {
        const double cx = std::clamp(row[0], 0.0, 1.0);
        const double cy = std::clamp(row[1], 0.0, 1.0);
        const double w = std::clamp(row[2], 0.0, 1.0);
        const double h = std::clamp(row[3], 0.0, 1.0);
        const double x1 =
            std::clamp((cx - 0.5 * w) * static_cast<double>(data_.image_width),
                       0.0,
                       static_cast<double>(data_.image_width));
        const double y1 =
            std::clamp((cy - 0.5 * h) * static_cast<double>(data_.image_height),
                       0.0,
                       static_cast<double>(data_.image_height));
        const double x2 =
            std::clamp((cx + 0.5 * w) * static_cast<double>(data_.image_width),
                       0.0,
                       static_cast<double>(data_.image_width));
        const double y2 =
            std::clamp((cy + 0.5 * h) * static_cast<double>(data_.image_height),
                       0.0,
                       static_cast<double>(data_.image_height));

        for (double value : {cx, cy, w, h}) {
            if (!std::isfinite(value)) {
                error_message = "bbox_norm_coords contains non-finite values.";
                return false;
            }
            bbox_flat.push_back(std::clamp(value, 0.0, 1.0));
        }
        bbox_img_xyxy_flat.push_back(x1);
        bbox_img_xyxy_flat.push_back(y1);
        bbox_img_xyxy_flat.push_back(x2);
        bbox_img_xyxy_flat.push_back(y2);
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
    std::vector<int8_t> source_kind_codes;
    std::vector<bool> manual_edit_flags;
    source_kind_codes.reserve(n_detections);
    manual_edit_flags.reserve(n_detections);
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
            source_kind_codes.push_back(3);
            manual_edit_flags.push_back(true);
        } else if (lowered.find("interp") != std::string::npos) {
            reasons[i] = "interpolated";
            detection_source_sanitized[i] = 1;
            ++interpolated_rows;
            source_kind_codes.push_back(2);
            manual_edit_flags.push_back(false);
        } else {
            reasons[i] = "clean";
            detection_source_sanitized[i] = 0;
            ++clean_rows;
            source_kind_codes.push_back(1);
            manual_edit_flags.push_back(false);
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

    std::vector<int64_t> frame_offsets;
    frame_offsets.reserve(frame_counts_sanitized.size() + 1);
    frame_offsets.push_back(0);
    int64_t running_offset = 0;
    for (int32_t count : frame_counts_sanitized) {
        running_offset += static_cast<int64_t>(count);
        frame_offsets.push_back(running_offset);
    }

    std::vector<int64_t> refined_row_ids;
    refined_row_ids.reserve(n_detections);
    std::vector<int32_t> source_detect_row_index(n_detections, -1);
    std::unordered_map<int32_t, int32_t> ordinal_by_frame;
    for (size_t i = 0; i < n_detections; ++i) {
        const int32_t frame_id = frame_indices[i];
        int32_t ordinal = ordinal_by_frame[frame_id]++;
        refined_row_ids.push_back((static_cast<int64_t>(frame_id) << 32) |
                                  static_cast<uint32_t>(ordinal));
    }

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
        source_variant_value != "interpolated" &&
        source_variant_value != "refined") {
        source_variant_value = (active_dataset_ == DetectionDataset::RefinedFiltered)
                                   ? "filtered"
                                   : "refined";
    }

    const std::string instances_path = refined_run_path + "instances/";
    auto delete_result =
        ts::kvstore::DeleteRange(store, ts::KeyRange::Prefix(instances_path))
            .result();
    if (!delete_result.ok()) {
        error_message =
            "Failed to clear existing instances subgroup: " +
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

    if (!writeNumericArray1D<int64_t>(
            store,
            context_,
            instances_path + "refined_row_ids",
            "int64",
            refined_row_ids,
            det_chunk,
            0,
            true,
            &error_message)) {
        return false;
    }
    if (!writeNumericArray1D<int32_t>(
            store,
            context_,
            instances_path + "frame_indices",
            "int32",
            frame_indices,
            det_chunk,
            0,
            true,
            &error_message)) {
        return false;
    }
    if (!writeNumericArray1D<int64_t>(
            store,
            context_,
            instances_path + "frame_offsets",
            "int64",
            frame_offsets,
            frame_chunk,
            0,
            true,
            &error_message)) {
        return false;
    }
    if (!writeNumericArray2DFlat<double>(
            store,
            context_,
            instances_path + "bbox_img_xyxy",
            "float64",
            bbox_img_xyxy_flat,
            static_cast<ts::Index>(n_detections),
            4,
            det_chunk,
            4,
            0.0,
            true,
            &error_message)) {
        return false;
    }
    if (!writeNumericArray2DFlat<double>(
            store,
            context_,
            instances_path + "bbox_norm_coords",
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
            instances_path + "confidence_scores",
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
            instances_path + "class_ids",
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
            instances_path + "frame_counts",
            "int32",
            frame_counts_sanitized,
            frame_chunk,
            0,
            true,
            &error_message)) {
        return false;
    }
    if (!writeNumericArray1D<int8_t>(
            store,
            context_,
            instances_path + "source_kind_codes",
            "int8",
            source_kind_codes,
            det_chunk,
            0,
            false,
            &error_message)) {
        return false;
    }
    if (!writeNumericArray1D<bool>(
            store,
            context_,
            instances_path + "manual_edit_flags",
            "bool",
            manual_edit_flags,
            det_chunk,
            false,
            false,
            &error_message)) {
        return false;
    }
    if (!writeNumericArray1D<int32_t>(
            store,
            context_,
            instances_path + "source_detect_row_index",
            "int32",
            source_detect_row_index,
            det_chunk,
            -1,
            true,
            &error_message)) {
        return false;
    }
    if (!writeNumericArray2DFlat<uint8_t>(
            store,
            context_,
            instances_path + "reason_bytes",
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
    if (!writeNumericArray1D<int8_t>(
            store,
            context_,
            instances_path + "detection_source",
            "int8",
            detection_source_sanitized,
            det_chunk,
            0,
            false,
            &error_message)) {
        return false;
    }

    const std::string timestamp = currentUtcIsoTimestamp();
    json instances_meta = makeEmptyGroupMetadataV3();
    auto& instances_attrs = instances_meta["attributes"];
    instances_attrs["reason_encoding"] = "utf8-null-terminated";
    instances_attrs["reason_bytes_width"] = static_cast<int64_t>(reason_bytes_width);
    instances_attrs["reason_bytes_null_terminated"] = true;
    instances_attrs["reason_fallback_order"] =
        json::array({"reason_bytes", "reason", "detection_source"});
    instances_attrs["storage_layout"] = "sparse_instances_v1";
    const json instance_fields = json::array(
        {"refined_row_ids",
         "frame_indices",
         "frame_offsets",
         "bbox_img_xyxy",
         "bbox_norm_coords",
         "source_kind_codes",
         "manual_edit_flags",
         "source_detect_row_index",
         "confidence_scores",
         "class_ids",
         "frame_counts",
         "detection_source",
         "reason_bytes"});
    instances_attrs["column_fields"] = instance_fields;
    instances_attrs["field_names"] = instance_fields;
    instances_attrs["total_detections"] = static_cast<int64_t>(n_detections);
    instances_attrs["clean_detections"] = static_cast<int64_t>(clean_rows);
    instances_attrs["interpolated_detections"] =
        static_cast<int64_t>(interpolated_rows);
    instances_attrs["manual_detections"] = static_cast<int64_t>(manual_rows);
    instances_attrs["source_refined_run"] = refined_run;
    instances_attrs["source_variant"] = source_variant_value;
    instances_attrs["curated_surface"] = "instances";
    if (!writeNodeMetaV3(store, instances_path, instances_meta, &error_message)) {
        return false;
    }

    json run_meta = normalizeGroupMetadataV3(*refined_run_meta);
    auto& run_attrs = run_meta["attributes"];
    run_attrs.erase("manual_review_latest");
    const json source_kind_code_map = json::object({
        {"none", 0},
        {"raw_detect", 1},
        {"interpolated", 2},
        {"manual", 3},
    });
    run_attrs["refined_storage_semantics"] = "sparse_instances_v1";
    run_attrs["curated_primary_surface"] = "instances";
    run_attrs["curated_row_storage"] = "sparse_instances_v1";
    run_attrs["row_sort_order"] = json::array({"frame_indices", "refined_row_ids"});
    run_attrs["source_kind_code_map"] = source_kind_code_map;
    run_attrs["summary_statistics"] = json::object({
        {"total_detections", static_cast<int64_t>(n_detections)},
        {"clean_detections", static_cast<int64_t>(clean_rows)},
        {"interpolated_detections", static_cast<int64_t>(interpolated_rows)},
        {"manual_detections", static_cast<int64_t>(manual_rows)},
    });
    json review_status = json{
        {"state", review_options.state},
        {"method", review_options.method},
        {"intended_use", review_options.intended_use},
        {"timestamp", timestamp},
        {"timestamp_utc", timestamp},
        {"resolved_group", "refined"},
        {"target_group", "refined"},
        {"preference_chain",
         json::array({"refined", "manual", "interpolated", "filtered", "raw"})}};
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
