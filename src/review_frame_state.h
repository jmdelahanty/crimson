#pragma once

#include "zarr/detection_repository.h"

#include <string>
#include <vector>

struct ReviewFrameFilters {
    bool include_interpolated = true;
    bool include_non_clean = true;
    bool include_empty = true;
};

struct ReviewFrameCache {
    bool valid = false;
    std::string archive_path;
    crimson::zarr::DetectionDataset dataset =
        crimson::zarr::DetectionDataset::RawDetect;
    size_t total_frames = 0;
    ReviewFrameFilters filters;
    std::vector<int> frames;
};
