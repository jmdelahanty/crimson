// gui_interpolation.h - GUI helper functions for displaying interpolation status
#ifndef GUI_INTERPOLATION_H
#define GUI_INTERPOLATION_H

#include "imgui.h"
#include "implot.h"
#include "zarr_loader.h"
#include "h5_loader.h"
#include <vector>

// Structure to track interpolation status across sources
struct InterpolationStatus {
    bool has_zarr_interpolation = false;
    bool has_h5_interpolation = false;
    bool current_frame_interpolated = false;
    std::string interpolation_method;
    std::string interpolation_timestamp;
};

// Draw bounding boxes with interpolation indication
static void gui_draw_bounding_boxes_with_interpolation(
    const std::vector<LoggedBoundingBox>& boxes, 
    int image_width, 
    int image_height,
    bool is_interpolated,
    bool use_interpolated_color = true) {
    
    (void)image_width;

    for (const auto& box : boxes) {
        double x_coords[5] = {
            box.x_min, 
            box.x_min + box.width, 
            box.x_min + box.width, 
            box.x_min, 
            box.x_min
        };
        
        double y_coords[5] = {
            (double)image_height - box.y_min,
            (double)image_height - box.y_min,
            (double)image_height - (box.y_min + box.height),
            (double)image_height - (box.y_min + box.height),
            (double)image_height - box.y_min
        };

        // Color based on interpolation status
        ImVec4 box_color;
        float line_width;
        
        if (is_interpolated && use_interpolated_color) {
            // Orange/yellow for interpolated frames
            box_color = ImVec4(1.0f, 0.7f, 0.0f, 0.9f);
            line_width = 2.5f;
        } else {
            // Green for original detections
            box_color = ImVec4(0.2f, 1.0f, 0.2f, 1.0f);
            line_width = 2.0f;
        }
        
        ImPlot::SetNextLineStyle(box_color, line_width);

        std::string label = "ID: " + std::to_string(box.class_id);
        if (is_interpolated) {
            label += " [I]";  // Mark as interpolated
        }
        
        ImPlot::PlotLine(label.c_str(), x_coords, y_coords, 5);
    }
}

// Display interpolation info in a debug window
static void gui_show_interpolation_debug_window(
    const ZarrDetectionLoader* zarr_loader,
    const H5SessionData* h5_data,
    size_t current_frame,
    bool* p_open = nullptr) {
    
    if (!ImGui::Begin("Interpolation Debug", p_open)) {
        ImGui::End();
        return;
    }
    
    ImGui::Text("Current Frame: %zu", current_frame);
    ImGui::Separator();
    
    // Zarr interpolation status
    if (zarr_loader && zarr_loader->hasInterpolation()) {
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Zarr Interpolation Available");
        
        bool is_interpolated = zarr_loader->isFrameInterpolated(current_frame);
        bool has_refined = zarr_loader->hasRefinedDetections();
        bool has_stimulus = zarr_loader->hasStimulusAlignment();
        
        if (is_interpolated) {
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.0f, 1.0f), 
                             "Status: INTERPOLATED");
        } else {
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), 
                             "Status: ORIGINAL");
        }
        
        ImGui::Text("Method: %s", zarr_loader->getInterpolationMethod().c_str());
        ImGui::Text("Created: %s", zarr_loader->getInterpolationCreatedAt().c_str());
        ImGui::Text("Refined detections: %s", has_refined ? "Yes" : "No");
        ImGui::Text("Stimulus alignment: %s", has_stimulus ? "Yes" : "No");

        std::string source_run = zarr_loader->getInterpolationSourceRun();
        if (!source_run.empty()) {
            ImGui::Text("Source detection run: %s", source_run.c_str());
        }

        std::string stimulus_run = zarr_loader->getStimulusRunName();
        if (!stimulus_run.empty()) {
            ImGui::Text("Stimulus run: %s", stimulus_run.c_str());
        }
        
        // Show detection count
        int32_t det_count = zarr_loader->getDetectionsForFrame(current_frame);
        ImGui::Text("Detections: %d", det_count);
        
    } else if (zarr_loader) {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), 
                         "No Zarr interpolation data");
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), 
                         "No Zarr loader available");
    }
    
    ImGui::Separator();
    
    // H5 interpolation status (if analysis file)
    if (h5_data && h5_data->is_analysis_file) {
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "H5 Analysis File");
        
        bool is_interpolated = h5_data->isFrameInterpolated(current_frame);
        
        if (is_interpolated) {
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.0f, 1.0f), 
                             "H5 Frame: INTERPOLATED");
        } else {
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), 
                             "H5 Frame: ORIGINAL");
        }
        
        ImGui::Text("Original frames: %zu", h5_data->getOriginalFrameCount());
        ImGui::Text("Interpolated frames: %zu", h5_data->getInterpolatedFrameCount());
        
    } else if (h5_data) {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), 
                         "Standard H5 file (no interpolation data)");
    }
    
    ImGui::End();
}

// Display frame status in the main video window
static void gui_show_frame_interpolation_status(
    const ZarrDetectionLoader* zarr_loader,
    const H5SessionData* h5_data,
    size_t current_frame,
    ImVec2 position = ImVec2(10, 10)) {
    
    // Create overlay text
    std::string status_text;
    ImVec4 status_color;
    
    bool zarr_interpolated = zarr_loader && zarr_loader->hasInterpolation() && 
                             zarr_loader->isFrameInterpolated(current_frame);
    bool h5_interpolated = h5_data && h5_data->is_analysis_file && 
                          h5_data->isFrameInterpolated(current_frame);
    
    if (zarr_interpolated || h5_interpolated) {
        status_text = "INTERPOLATED";
        status_color = ImVec4(1.0f, 0.7f, 0.0f, 0.9f);  // Orange
    } else if ((zarr_loader && zarr_loader->hasInterpolation()) || 
               (h5_data && h5_data->is_analysis_file)) {
        status_text = "ORIGINAL";
        status_color = ImVec4(0.2f, 1.0f, 0.2f, 0.9f);  // Green
    } else {
        // No interpolation data available
        return;
    }
    
    // Draw status overlay
    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    ImVec2 text_size = ImGui::CalcTextSize(status_text.c_str());
    
    // Background box
    ImVec2 box_min = ImPlot::PlotToPixels(ImPlotPoint(position.x, position.y));
    ImVec2 box_max = ImVec2(box_min.x + text_size.x + 10, 
                            box_min.y + text_size.y + 6);
    
    draw_list->AddRectFilled(box_min, box_max, 
                             IM_COL32(0, 0, 0, 200), 3.0f);
    draw_list->AddRect(box_min, box_max, 
                       ImGui::ColorConvertFloat4ToU32(status_color), 3.0f);
    
    // Text
    draw_list->AddText(ImVec2(box_min.x + 5, box_min.y + 3), 
                       ImGui::ColorConvertFloat4ToU32(status_color), 
                       status_text.c_str());
}

#endif // GUI_INTERPOLATION_H
