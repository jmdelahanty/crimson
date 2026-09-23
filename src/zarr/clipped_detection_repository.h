#pragma once

#include "zarr/palette_clipped_resolver.h"
#include "zarr_loader.h"

#include <cstddef>
#include <string>

#include <tensorstore/context.h>
#include <tensorstore/kvstore/kvstore.h>

struct ClippedDetectionLoadTiming {
    double total_ms = 0.0;
    double read_frame_indices_ms = 0.0;
    double read_attrs_ms = 0.0;
    double read_bbox_img_ms = 0.0;
    double read_bbox_norm_ms = 0.0;
    double read_scores_ms = 0.0;
    double read_class_ids_ms = 0.0;
    double read_source_kind_ms = 0.0;
    double append_rows_ms = 0.0;
    double sort_reorder_ms = 0.0;
    double offsets_ms = 0.0;
};

struct ClippedDetectionLoadResult {
    InterpolationRunData stage;
    int image_width = 0;
    int image_height = 0;
    size_t loaded_runs = 0;
    size_t selected_runs = 0;
    size_t total_clip_rows = 0;
    size_t scaled_bbox_runs = 0;
    std::string first_scaled_geometry;
    ClippedDetectionLoadTiming timing;
};

bool loadClippedRefinedCollectionDetections(
    const ts::kvstore::KvStore& store,
    const ts::Context& context,
    const PaletteClippedResolver& resolver,
    int current_image_width,
    int current_image_height,
    ClippedDetectionLoadResult& result);
