#include "gui/world_projection_overlay.h"

#include "imgui.h"
#include "implot.h"

#include <cmath>
#include <string>
#include <vector>

void world_coordinates_projection_points(CameraParams *cvp,
                                         int image_height,
                                         double *axis_x_values,
                                         double *axis_y_values,
                                         float scale) {
    std::vector<cv::Point3f> world_coordinates;
    world_coordinates.push_back(cv::Point3f(0.0f, 0.0f, 0.0f));
    world_coordinates.push_back(cv::Point3f(scale * 1.0f, 0.0f, 0.0f));
    world_coordinates.push_back(cv::Point3f(0.0f, scale * 1.0f, 0.0f));
    world_coordinates.push_back(cv::Point3f(0.0f, 0.0f, scale * 1.0f));

    std::vector<cv::Point2f> img_pts;
    cv::projectPoints(world_coordinates, cvp->rvec, cvp->tvec, cvp->k,
                      cvp->dist_coeffs, img_pts);

    for (int i = 0; i < 4; i++) {
        axis_x_values[i] = img_pts.at(i).x;
        axis_y_values[i] = image_height - img_pts.at(i).y;
    }
}

void gui_plot_world_coordinates(CameraParams *cvp,
                                int cam_id,
                                int image_height) {
    (void)cam_id;
    double axis_x_values[4];
    double axis_y_values[4];
    world_coordinates_projection_points(cvp, image_height, axis_x_values,
                                        axis_y_values, 50);
    ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 6.0,
                               ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    ImPlot::SetNextLineStyle(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), 3.0f);
    std::string name = "World Origin";

    std::vector<ImVec4> node_colors = {
        ImVec4(1.0f, 1.0f, 1.0f, 1.0f),
        ImVec4(1.0f, 0.0f, 0.0f, 1.0f),
        ImVec4(0.0f, 1.0f, 0.0f, 1.0f),
        ImVec4(0.0f, 0.0f, 1.0f, 1.0f)};

    for (int edge = 0; edge < 3; edge++) {
        double xs[2]{axis_x_values[0], axis_x_values[edge + 1]};
        double ys[2]{axis_y_values[0], axis_y_values[edge + 1]};

        const double vec2_x = axis_x_values[edge + 1] - axis_x_values[0];
        const double vec2_y = axis_y_values[edge + 1] - axis_y_values[0];

        const double vec2_norm_x = -vec2_y;
        const double vec2_norm_y = vec2_x;

        const double arrow_end_1_x =
            axis_x_values[edge + 1] - vec2_x / 2 + vec2_norm_x / 2;
        const double arrow_end_1_y =
            axis_y_values[edge + 1] - vec2_y / 2 + vec2_norm_y / 2;

        const double arrow_end_2_x =
            axis_x_values[edge + 1] - vec2_x / 2 - vec2_norm_x / 2;
        const double arrow_end_2_y =
            axis_y_values[edge + 1] - vec2_y / 2 - vec2_norm_y / 2;

        ImVec4 my_color = node_colors[edge + 1];
        my_color.w = 0.8f;

        ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 6.0, my_color);
        ImPlot::SetNextLineStyle(my_color, 3.0);
        ImPlot::PlotLine(name.c_str(), xs, ys, 2, ImPlotLineFlags_Segments);

        xs[0] = axis_x_values[edge + 1];
        xs[1] = arrow_end_1_x;
        ys[0] = axis_y_values[edge + 1];
        ys[1] = arrow_end_1_y;
        ImPlot::PlotLine(name.c_str(), xs, ys, 2, ImPlotLineFlags_Segments);

        xs[0] = axis_x_values[edge + 1];
        xs[1] = arrow_end_2_x;
        ys[0] = axis_y_values[edge + 1];
        ys[1] = arrow_end_2_y;
        ImPlot::PlotLine(name.c_str(), xs, ys, 2, ImPlotLineFlags_Segments);
    }
}

void gui_arena_projection_points(CameraParams *cvp,
                                 int image_height,
                                 float *arena_x,
                                 float *arena_y,
                                 int n) {
    std::vector<float> x;
    std::vector<float> y;
    std::vector<float> z;

    const float radius = 1473.0f;
    std::vector<cv::Point3f> inPts;

    for (int i = 0; i <= n; i++) {
        const float angle =
            (3.14159265358979323846f * 2.0f) * (static_cast<float>(i) /
                                                static_cast<float>(n - 1));
        x.push_back(std::sin(angle) * radius);
        y.push_back(std::cos(angle) * radius);
        z.push_back(0.0f);
    }

    for (int i = 0; i < n; i++) {
        cv::Point3f p;
        p.x = x[i];
        p.y = y[i];
        p.z = z[i];
        inPts.push_back(p);
    }

    std::vector<cv::Point2f> img_pts;
    cv::projectPoints(inPts, cvp->rvec, cvp->tvec, cvp->k, cvp->dist_coeffs,
                      img_pts);

    for (int i = 0; i < n; i++) {
        arena_x[i] = img_pts.at(i).x;
        arena_y[i] = image_height - img_pts.at(i).y;
    }
}

void gui_plot_perimeter(CameraParams *cvp, int image_height) {
    float arena_x[100];
    float arena_y[100];
    gui_arena_projection_points(cvp, image_height, arena_x, arena_y, 100);
    ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 6.0,
                               ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    ImPlot::SetNextLineStyle(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), 3.0f);
    std::string name = "arena";
    ImPlot::PlotLine(name.c_str(), arena_x, arena_y, 100);
}
