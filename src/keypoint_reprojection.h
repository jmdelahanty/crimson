#pragma once

#include "camera.h"
#include "render.h"
#include "skeleton.h"

#include <vector>

void reprojection(KeyPoints *keypoints,
                  SkeletonContext *skeleton,
                  std::vector<CameraParams> camera_params,
                  render_scene *scene);
