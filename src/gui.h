// src/gui.h

#ifndef RED_GUI
#define RED_GUI
#include "camera.h"
#include "render.h"
#include "h5_loader.h"
#include "skeleton.h"
#include "keypoint_io.h"
#include "keypoint_reprojection.h"
#include "gui/manual_keypoint_overlay.h"
#include "gui/world_projection_overlay.h"
#include <filesystem>
#include <fstream>
#include <future>
#include <regex>
#include <thread>
#include <vector>
#include <iostream>

struct ProjectContext {
    std::string root_dir;
    std::vector<std::string> input_file_names;
    std::vector<std::string> camera_names;
};

static void gui_draw_chaser_state(const std::vector<LoggedChaserState>& states, int image_height, const CameraParams& cam_params) {
    if (!cam_params.has_valid_homography) {
        return;
    }

    float offsetX = cam_params.stimulus_offset_x;
    float offsetY = cam_params.stimulus_offset_y;

    for (const auto& state : states) {
        // We are temporarily ignoring the 'is_chasing' flag for debugging.
        // if (state.is_chasing) {

            // --- START: ADDED DEBUG PRINT ---
            // std::cout << "Frame: " << state.stimulus_frame_num
            //           << " | Chaser: (" << state.chaser_pos_x << ", " << state.chaser_pos_y << ")"
            //           << " | Target: (" << state.target_pos_x << ", " << state.target_pos_y << ")" << std::endl;
            // --- END: ADDED DEBUG PRINT ---

            std::vector<cv::Point2f> src_points;
            std::vector<cv::Point2f> dst_points;
            
            // Apply the offset to the stimulus coordinates
            src_points.push_back(cv::Point2f(state.chaser_pos_x + offsetX, state.chaser_pos_y + offsetY));
            
            // Use the INVERSE homography matrix on the corrected coordinates
            cv::perspectiveTransform(src_points, dst_points, cam_params.inverse_homography_matrix);

            if (!dst_points.empty()) {
                double x = dst_points[0].x;
                double y = (double)image_height - dst_points[0].y;
                
                ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 8.0f, ImVec4(1.0f, 0.0f, 0.0f, 1.0f));
                ImPlot::PlotScatter("Chaser", &x, &y, 1);

                // Apply the same offset to the target coordinates
                src_points[0] = cv::Point2f(state.target_pos_x + offsetX, state.target_pos_y + offsetY);
                cv::perspectiveTransform(src_points, dst_points, cam_params.inverse_homography_matrix);

                if (!dst_points.empty()) {
                    x = dst_points[0].x;
                    y = (double)image_height - dst_points[0].y;
                    ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 8.0f, ImVec4(0.0f, 1.0f, 0.0f, 1.0f));
                    ImPlot::PlotScatter("Target", &x, &y, 1);
                }
            }
        // } 
    }
}


static void draw_cv_contours(std::vector<cv::Rect> boxes,
                             std::vector<std::string> labels,
                             std::vector<int> class_ids, int image_height) {
    for (int i = 0; i < boxes.size(); i++) {
        double x[5] = {(double)boxes[i].x, (double)boxes[i].x,
                       (double)boxes[i].x + boxes[i].width,
                       (double)boxes[i].x + boxes[i].width, (double)boxes[i].x};
        double y[5] = {(double)image_height - boxes[i].y,
                       (double)image_height - boxes[i].y - boxes[i].height,
                       (double)image_height - boxes[i].y - boxes[i].height,
                       (double)image_height - boxes[i].y,
                       (double)image_height - boxes[i].y};

        if (class_ids[i] == 0) {
            ImPlot::SetNextLineStyle(ImVec4(1.0, 0.0, 1.0, 1.0), 3.0);
        } else {
            ImPlot::SetNextLineStyle(ImVec4(0.5, 1.0, 1.0, 1.0), 3.0);
        }

        ImPlot::PlotLine(labels[i].c_str(), &x[0], &y[0], 5);
    }
}

static void gui_draw_bounding_boxes(const std::vector<LoggedBoundingBox>& boxes, int image_width, int image_height) {
    (void)image_width;

    for (const auto& box : boxes) {
        double x_coords[5] = {box.x_min, box.x_min + box.width, box.x_min + box.width, box.x_min, box.x_min};
        double y_coords[5] = {
            (double)image_height - box.y_min,
            (double)image_height - box.y_min,
            (double)image_height - (box.y_min + box.height),
            (double)image_height - (box.y_min + box.height),
            (double)image_height - box.y_min
        };

        ImPlot::SetNextLineStyle(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), 2.0f);

        std::string label = "ID: " + std::to_string(box.class_id);
        ImPlot::PlotLine(label.c_str(), x_coords, y_coords, 5);
    }
}

static void gui_draw_zarr_bounding_boxes(const std::vector<LoggedBoundingBox>& boxes, int image_width, int image_height) {
    (void)image_width;

    for (const auto& box : boxes) {
        double x_coords[5] = {box.x_min, box.x_min + box.width, box.x_min + box.width, box.x_min, box.x_min};
        double y_coords[5] = {
            (double)image_height - box.y_min,
            (double)image_height - box.y_min,
            (double)image_height - (box.y_min + box.height),
            (double)image_height - (box.y_min + box.height),
            (double)image_height - box.y_min
        };

        // BLUE color instead of green
        ImPlot::SetNextLineStyle(ImVec4(0.2f, 0.2f, 1.0f, 1.0f), 2.0f);  // Blue (R,G,B,A)

        std::string label = "Zarr ID: " + std::to_string(box.class_id);
        ImPlot::PlotLine(label.c_str(), x_coords, y_coords, 5);
    }
}

void diagnose_h5_homography_loading(const std::string& h5_filepath) {
    std::cout << "\n=== H5 Homography Loading Diagnostic ===" << std::endl;
    std::cout << "File: " << h5_filepath << std::endl;
    
    H5SessionLoader loader;
    H5SessionData data;
    std::string error_msg;
    
    if (loader.loadH5File(h5_filepath, data, error_msg)) {
        std::cout << "H5 file loaded successfully" << std::endl;
        
        if (data.camera_calibrations.empty()) {
            std::cout << "WARNING: No camera calibrations found in H5 file!" << std::endl;
            std::cout << "The H5 file may be using an older format or missing calibration data." << std::endl;
        } else {
            std::cout << "Found " << data.camera_calibrations.size() << " camera calibrations:" << std::endl;
            
            for (const auto& [cam_id, calib] : data.camera_calibrations) {
                std::cout << "\nCamera: " << cam_id << std::endl;
                
                if (calib.has_homography) {
                    std::cout << "  ✓ Homography matrix loaded (3x3)" << std::endl;
                    std::cout << "  Timestamp: " << calib.calibration_timestamp_utc << std::endl;
                    std::cout << "  Matrix values:" << std::endl;
                    std::cout << calib.homography_matrix << std::endl;
                } else {
                    std::cout << "  ✗ No homography matrix found" << std::endl;
                }
                
                if (calib.has_attributes) {
                    std::cout << "  ✓ Calibration attributes loaded:" << std::endl;
                    std::cout << "    - pixels_per_mm_projector: " << calib.pixels_per_mm_projector << std::endl;
                    std::cout << "    - pixels_per_mm_camera: " << calib.pixels_per_mm_camera << std::endl;
                    std::cout << "    - real_world_ref_mm: " << calib.real_world_ref_mm << std::endl;
                } else {
                    std::cout << "  ✗ No calibration attributes found" << std::endl;
                }
            }
        }
        
        // Also check the JSON fallback
        if (!data.arena_config_json.empty()) {
            std::cout << "\nJSON arena_config is available (fallback method)" << std::endl;
            try {
                json config = json::parse(data.arena_config_json);
                if (config.contains("camera_calibrations")) {
                    std::cout << "  JSON contains " << config["camera_calibrations"].size() 
                              << " camera calibrations" << std::endl;
                }
            } catch (const json::exception& e) {
                std::cout << "  Failed to parse JSON: " << e.what() << std::endl;
            }
        }
    } else {
        std::cout << "Failed to load H5 file: " << error_msg << std::endl;
    }
    
    std::cout << "=== End Diagnostic ===" << std::endl;
}


#endif
