#include "zarr_persisted_crop_provider.h"

bool ZarrPersistedCropProvider::getCropImageForIndex(
    int32_t roi_index,
    CropImageView& out_view) const {
    ZarrDetectionLoader::CropImageView loader_view;
    if (!loader_.getCropImageForIndex(roi_index, loader_view)) {
        out_view = {};
        return false;
    }

    out_view.data = loader_view.data;
    out_view.width = loader_view.width;
    out_view.height = loader_view.height;
    out_view.channels = loader_view.channels;
    out_view.origin = CropImageView::Origin::PersistedZarr;
    return true;
}

const std::vector<int32_t>& ZarrPersistedCropProvider::getCropFrameIndices()
    const {
    return loader_.getCropFrameIndices();
}
