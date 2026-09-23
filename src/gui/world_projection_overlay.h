#pragma once

#include "camera.h"

void world_coordinates_projection_points(CameraParams *cvp,
                                         int image_height,
                                         double *axis_x_values,
                                         double *axis_y_values,
                                         float scale);

void gui_plot_world_coordinates(CameraParams *cvp,
                                int cam_id,
                                int image_height);

void gui_arena_projection_points(CameraParams *cvp,
                                 int image_height,
                                 float *arena_x,
                                 float *arena_y,
                                 int n);

void gui_plot_perimeter(CameraParams *cvp, int image_height);
