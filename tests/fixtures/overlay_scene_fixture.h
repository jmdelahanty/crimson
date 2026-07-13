#pragma once

#include "crop_source_contract.h"
#include "overlay_scene_contract.h"

namespace crimson::overlay::fixture {

inline constexpr int kView = 1;
inline constexpr int64_t kFrame = 137;
inline constexpr Rect kVisibleSource{320.0, 180.0, 960.0, 540.0};
inline constexpr Rect kDisplay{40.0, 20.0, 1280.0, 720.0};
inline constexpr Point kSourcePoint{560.0, 315.0};
inline constexpr Point kExpectedDisplayPoint{360.0, 200.0};
inline constexpr Rect kPartiallyVisibleRect{200.0, 120.0, 300.0, 200.0};
inline constexpr Rect kExpectedClippedRect{320.0, 180.0, 180.0, 140.0};
inline constexpr Rect kExpectedDisplayRect{40.0,
                                           20.0,
                                           240.0,
                                           186.66666666666666};

inline crop::CropFrameGeometry makeCropGeometry() {
    crop::CropFrameGeometry geometry;
    geometry.camera_frame = kFrame;
    geometry.source_width = 1920;
    geometry.source_height = 1080;
    geometry.output_width = 640;
    geometry.output_height = 320;
    geometry.full_frame_crop = {400.0, 200.0, 320.0, 160.0};
    geometry.full_frame_detection = crop::CropRect{440.0, 220.0, 80.0, 40.0};
    geometry.geometry_available = true;
    geometry.has_detection = true;
    return geometry;
}

inline constexpr crop::CropPoint kFullFramePoint{480.0, 240.0};
inline constexpr crop::CropPoint kExpectedCropPoint{160.0, 80.0};
inline constexpr crop::CropRect kExpectedCropDetection{80.0, 40.0, 160.0, 80.0};

inline constexpr HeadingNormalizedCropTransform kRotation{
    640.0, 320.0, 320.0, 30.0};
inline constexpr Point kCropRotationPoint{400.0, 200.0};
inline constexpr Point kExpectedRotatedPoint{249.28203230275508,
                                             154.64101615137756};

}  // namespace crimson::overlay::fixture
