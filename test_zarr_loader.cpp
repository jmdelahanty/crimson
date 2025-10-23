// test_zarr_loader.cpp
// Standalone test program for the ZarrDetectionLoader
// Compile with: g++ -o test_zarr test_zarr_loader.cpp zarr_loader.cpp -ltensorstore -lnlohmann_json -std=c++17

#include <iostream>
#include <iomanip>
#include <filesystem>
#include <chrono>
#include "zarr_loader.h"

// Color codes for terminal output
#define GREEN "\033[32m"
#define RED "\033[31m"
#define YELLOW "\033[33m"
#define BLUE "\033[34m"
#define RESET "\033[0m"

void printSeparator() {
    std::cout << "================================================" << std::endl;
}

void testBasicLoading(const std::string& zarr_path) {
    std::cout << BLUE << "\n=== TEST 1: Basic Loading ===" << RESET << std::endl;
    
    ZarrDetectionLoader loader;
    std::string error_msg;
    
    auto start = std::chrono::high_resolution_clock::now();
    bool success = loader.loadZarrFile(zarr_path, error_msg);
    auto end = std::chrono::high_resolution_clock::now();
    
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    if (success) {
        std::cout << GREEN << "✓ Successfully loaded zarr file" << RESET << std::endl;
        std::cout << "  Load time: " << duration.count() << " ms" << std::endl;
        std::cout << "  Total frames: " << loader.getTotalFrames() << std::endl;
        std::cout << "  Max detections per frame: " << loader.getMaxDetections() << std::endl;
        std::cout << "  FPS: " << loader.getFPS() << std::endl;
        std::cout << "  Resolution: " << loader.getImageWidth() << "x" << loader.getImageHeight() << std::endl;
        if (!loader.getDetectRunName().empty()) {
            std::cout << "  Detect run: " << loader.getDetectRunName() << std::endl;
            if (!loader.getDetectRunMethod().empty()) {
                std::cout << "    Method: " << loader.getDetectRunMethod() << std::endl;
            }
            if (!loader.getDetectRunCreatedAt().empty()) {
                std::cout << "    Created: " << loader.getDetectRunCreatedAt() << std::endl;
            }
        }
        std::cout << "  Has scores: " << (loader.hasScores() ? "Yes" : "No") << std::endl;
        std::cout << "  Has class IDs: " << (loader.hasClassIDs() ? "Yes" : "No") << std::endl;
        if (loader.hasInterpolation()) {
            std::cout << "  Interpolation method: " << loader.getInterpolationMethod() << std::endl;
            std::cout << "  Interpolation created: " << loader.getInterpolationCreatedAt() << std::endl;
            std::string source_run = loader.getInterpolationSourceRun();
            if (!source_run.empty()) {
                std::cout << "  Interpolation source run: " << source_run << std::endl;
            }
            std::string stimulus_run = loader.getStimulusRunName();
            if (!stimulus_run.empty()) {
                std::cout << "  Stimulus run: " << stimulus_run << std::endl;
            }
        }
    } else {
        std::cout << RED << "✗ Failed to load zarr file: " << error_msg << RESET << std::endl;
    }
}

void testFrameAccess(const std::string& zarr_path) {
    std::cout << BLUE << "\n=== TEST 2: Frame Access ===" << RESET << std::endl;
    
    ZarrDetectionLoader loader;
    std::string error_msg;
    
    if (!loader.loadZarrFile(zarr_path, error_msg)) {
        std::cout << RED << "✗ Cannot test frame access - file loading failed" << RESET << std::endl;
        return;
    }
    
    // Test first frame
    std::cout << "\nTesting frame 0:" << std::endl;
    auto boxes_frame0 = loader.getBoundingBoxesForFrame(0);
    std::cout << "  Detections: " << boxes_frame0.size() << std::endl;
    
    if (!boxes_frame0.empty()) {
        std::cout << "  First detection:" << std::endl;
        const auto& box = boxes_frame0[0];
        std::cout << "    Position: [" << box.x_min << ", " << box.y_min 
                  << ", " << box.width << ", " << box.height << "]" << std::endl;
        std::cout << "    Confidence: " << box.confidence << std::endl;
        std::cout << "    Class ID: " << box.class_id << std::endl;
    }
    
    // Test middle frame
    size_t middle_frame = loader.getTotalFrames() / 2;
    std::cout << "\nTesting frame " << middle_frame << ":" << std::endl;
    auto boxes_middle = loader.getBoundingBoxesForFrame(middle_frame);
    std::cout << "  Detections: " << boxes_middle.size() << std::endl;
    
    // Test last frame
    size_t last_frame = loader.getTotalFrames() - 1;
    std::cout << "\nTesting frame " << last_frame << ":" << std::endl;
    auto boxes_last = loader.getBoundingBoxesForFrame(last_frame);
    std::cout << "  Detections: " << boxes_last.size() << std::endl;
    
    // Test out of bounds
    std::cout << "\nTesting out-of-bounds frame " << loader.getTotalFrames() << ":" << std::endl;
    auto boxes_oob = loader.getBoundingBoxesForFrame(loader.getTotalFrames());
    if (boxes_oob.empty()) {
        std::cout << GREEN << "  ✓ Correctly returned empty for out-of-bounds" << RESET << std::endl;
    } else {
        std::cout << RED << "  ✗ Should return empty for out-of-bounds!" << RESET << std::endl;
    }
}

void testPerformance(const std::string& zarr_path) {
    std::cout << BLUE << "\n=== TEST 3: Performance ===" << RESET << std::endl;
    
    ZarrDetectionLoader loader;
    std::string error_msg;
    
    if (!loader.loadZarrFile(zarr_path, error_msg)) {
        std::cout << RED << "✗ Cannot test performance - file loading failed" << RESET << std::endl;
        return;
    }
    
    const int num_test_frames = std::min(100, static_cast<int>(loader.getTotalFrames()));
    
    std::cout << "Reading " << num_test_frames << " frames sequentially..." << std::endl;
    
    auto start = std::chrono::high_resolution_clock::now();
    
    size_t total_detections = 0;
    for (int i = 0; i < num_test_frames; ++i) {
        auto boxes = loader.getBoundingBoxesForFrame(i);
        total_detections += boxes.size();
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    double ms_per_frame = static_cast<double>(duration.count()) / num_test_frames;
    double fps = 1000.0 / ms_per_frame;
    
    std::cout << GREEN << "  ✓ Read " << num_test_frames << " frames" << RESET << std::endl;
    std::cout << "  Total time: " << duration.count() << " ms" << std::endl;
    std::cout << "  Average time per frame: " << std::fixed << std::setprecision(2) 
              << ms_per_frame << " ms" << std::endl;
    std::cout << "  Equivalent FPS: " << std::fixed << std::setprecision(1) 
              << fps << " fps" << std::endl;
    std::cout << "  Total detections processed: " << total_detections << std::endl;
}

void testRawDetections(const std::string& zarr_path) {
    std::cout << BLUE << "\n=== TEST 4: Raw Detections Access ===" << RESET << std::endl;
    
    ZarrDetectionLoader loader;
    std::string error_msg;
    
    if (!loader.loadZarrFile(zarr_path, error_msg)) {
        std::cout << RED << "✗ Cannot test raw detections - file loading failed" << RESET << std::endl;
        return;
    }
    
    // Find a frame with detections
    size_t test_frame = 0;
    for (size_t i = 0; i < std::min(size_t(10), loader.getTotalFrames()); ++i) {
        if (loader.getDetectionsForFrame(i) > 0) {
            test_frame = i;
            break;
        }
    }
    
    std::cout << "Testing raw detections for frame " << test_frame << ":" << std::endl;
    auto raw_dets = loader.getRawDetections(test_frame);
    
    std::cout << "  Boxes: " << raw_dets.boxes.size() << std::endl;
    std::cout << "  Scores: " << raw_dets.scores.size() << std::endl;
    std::cout << "  Class IDs: " << raw_dets.class_ids.size() << std::endl;
    
    if (!raw_dets.boxes.empty()) {
        std::cout << "\n  First 3 detections (or less):" << std::endl;
        for (size_t i = 0; i < std::min(size_t(3), raw_dets.boxes.size()); ++i) {
            std::cout << "    Detection " << i << ":" << std::endl;
            std::cout << "      Box: [" << raw_dets.boxes[i][0] << ", " 
                      << raw_dets.boxes[i][1] << ", "
                      << raw_dets.boxes[i][2] << ", "
                      << raw_dets.boxes[i][3] << "]" << std::endl;
            
            if (i < raw_dets.scores.size()) {
                std::cout << "      Score: " << raw_dets.scores[i] << std::endl;
            }
            
            if (i < raw_dets.class_ids.size()) {
                std::cout << "      Class: " << raw_dets.class_ids[i] << std::endl;
            }
        }
    }

    if (loader.hasInterpolation()) {
        auto interp_dets = loader.getRawDetections(test_frame, true);
        std::cout << "\n  Interpolated detections for frame " << test_frame
                  << ": " << interp_dets.boxes.size() << std::endl;
        std::cout << "  Has refined detections: "
                  << (loader.hasRefinedDetections() ? "Yes" : "No") << std::endl;
        std::cout << "  Has stimulus mask: "
                  << (loader.hasStimulusAlignment() ? "Yes" : "No") << std::endl;
    }
}

void testDirectorySearch(const std::string& directory) {
    std::cout << BLUE << "\n=== TEST 5: Directory Search ===" << RESET << std::endl;
    
    auto zarr_file = ZarrDetectionLoader::findZarrDetectionFile(directory);
    
    if (zarr_file.has_value()) {
        std::cout << GREEN << "✓ Found zarr file: " << zarr_file.value() << RESET << std::endl;
        
        // Try to load it
        ZarrDetectionLoader loader;
        std::string error_msg;
        if (loadZarrDetectionFromDirectory(directory, loader, error_msg)) {
            std::cout << GREEN << "✓ Successfully loaded from directory" << RESET << std::endl;
            std::cout << "  Frames: " << loader.getTotalFrames() << std::endl;
        } else {
            std::cout << RED << "✗ Failed to load: " << error_msg << RESET << std::endl;
        }
    } else {
        std::cout << YELLOW << "No zarr detection files found in directory" << RESET << std::endl;
    }
}

void testStatistics(const std::string& zarr_path) {
    std::cout << BLUE << "\n=== TEST 6: Detection Statistics ===" << RESET << std::endl;
    
    ZarrDetectionLoader loader;
    std::string error_msg;
    
    if (!loader.loadZarrFile(zarr_path, error_msg)) {
        std::cout << RED << "✗ Cannot compute statistics - file loading failed" << RESET << std::endl;
        return;
    }
    
    // Compute statistics across all frames
    int frames_with_detections = 0;
    int total_detections = 0;
    int max_detections_in_frame = 0;
    int min_detections_in_frame = INT_MAX;
    std::vector<int> detection_counts;
    
    for (size_t i = 0; i < loader.getTotalFrames(); ++i) {
        int n_dets = loader.getDetectionsForFrame(i);
        detection_counts.push_back(n_dets);
        
        if (n_dets > 0) {
            frames_with_detections++;
            total_detections += n_dets;
            max_detections_in_frame = std::max(max_detections_in_frame, n_dets);
            min_detections_in_frame = std::min(min_detections_in_frame, n_dets);
        }
    }
    
    if (min_detections_in_frame == INT_MAX) {
        min_detections_in_frame = 0;
    }
    
    double avg_detections = static_cast<double>(total_detections) / loader.getTotalFrames();
    double detection_rate = static_cast<double>(frames_with_detections) * 100.0 / loader.getTotalFrames();
    
    std::cout << "Detection Statistics:" << std::endl;
    std::cout << "  Total frames: " << loader.getTotalFrames() << std::endl;
    std::cout << "  Frames with detections: " << frames_with_detections 
              << " (" << std::fixed << std::setprecision(1) << detection_rate << "%)" << std::endl;
    std::cout << "  Total detections: " << total_detections << std::endl;
    std::cout << "  Average detections per frame: " << std::fixed << std::setprecision(2) 
              << avg_detections << std::endl;
    std::cout << "  Max detections in a frame: " << max_detections_in_frame << std::endl;
    std::cout << "  Min detections in a frame (non-zero): " << min_detections_in_frame << std::endl;
    
    // Show distribution
    std::cout << "\n  Detection count distribution:" << std::endl;
    std::map<int, int> distribution;
    for (int count : detection_counts) {
        distribution[count]++;
    }
    
    for (const auto& [count, frequency] : distribution) {
        if (frequency > 5) {  // Only show counts that appear more than 5 times
            std::cout << "    " << count << " detections: " << frequency << " frames" << std::endl;
        }
    }
}

int main(int argc, char** argv) {
    std::cout << "=====================================" << std::endl;
    std::cout << "    Zarr Detection Loader Test      " << std::endl;
    std::cout << "=====================================" << std::endl;
    
    if (argc < 2) {
        std::cout << "Usage: " << argv[0] << " <zarr_file_or_directory>" << std::endl;
        std::cout << "\nExamples:" << std::endl;
        std::cout << "  " << argv[0] << " detections.zarr" << std::endl;
        std::cout << "  " << argv[0] << " /path/to/data/directory" << std::endl;
        return 1;
    }
    
    std::string input_path = argv[1];
    std::filesystem::path path(input_path);
    
    // Check if input exists
    if (!std::filesystem::exists(path)) {
        std::cout << RED << "Error: Path does not exist: " << input_path << RESET << std::endl;
        return 1;
    }
    
    std::string zarr_path;
    
    // Determine if it's a zarr file or a directory
    if (std::filesystem::is_directory(path)) {
        // Check if it's a zarr directory (has .zarr extension or contains .zarray)
        if (path.extension() == ".zarr" || path.extension() == ".zr3") {
            zarr_path = input_path;
            std::cout << "Input is a zarr directory: " << zarr_path << std::endl;
        } else {
            // Search for zarr files in the directory
            std::cout << "Searching for zarr files in directory: " << input_path << std::endl;
            testDirectorySearch(input_path);
            
            auto found = ZarrDetectionLoader::findZarrDetectionFile(input_path);
            if (found.has_value()) {
                zarr_path = found.value();
                std::cout << "Using found zarr file: " << zarr_path << std::endl;
            } else {
                std::cout << RED << "No zarr detection files found in directory" << RESET << std::endl;
                return 1;
            }
        }
    } else {
        std::cout << RED << "Error: Input is not a directory" << RESET << std::endl;
        return 1;
    }
    
    // Run all tests
    printSeparator();
    testBasicLoading(zarr_path);
    
    printSeparator();
    testFrameAccess(zarr_path);
    
    printSeparator();
    testPerformance(zarr_path);
    
    printSeparator();
    testRawDetections(zarr_path);
    
    printSeparator();
    testStatistics(zarr_path);
    
    printSeparator();
    std::cout << GREEN << "\n✓ All tests completed!" << RESET << std::endl;
    
    return 0;
}
