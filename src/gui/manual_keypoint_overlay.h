#pragma once

#include <fstream>

#include "render.h"
#include "skeleton.h"

void gui_plot_keypoints(KeyPoints *keypoints,
                        SkeletonContext *skeleton,
                        int view_idx,
                        int num_cams);

void gui_plot_bbox_from_keypoints(KeyPoints *keypoints,
                                  SkeletonContext *skeleton,
                                  int view_idx,
                                  int top_left_idx,
                                  int bottom_right_idx);
