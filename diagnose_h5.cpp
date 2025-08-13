// src/diagnose_h5.cpp

#include <iostream>
#include <string>
#include <vector>
#include "h5_loader.h"
#include "camera.h"

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <path_to_h5_file> <camera_id>" << std::endl;
        return 1;
    }

    std::string h5_filepath = argv[1];
    std::string camera_id = argv[2];

    std::cout << "=== H5 Homography Loading Diagnostic ===" << std::endl;
    std::cout << "File: " << h5_filepath << std::endl;
    std::cout << "Camera ID: " << camera_id << std::endl;

    H5SessionLoader loader;
    H5SessionData data;
    std::string error_msg;

    if (loader.loadH5File(h5_filepath, data, error_msg)) {
        std::cout << "H5 file loaded successfully" << std::endl;
        
        CameraParams params;
        if (camera_load_calibration_from_h5_enhanced(data, camera_id, params, error_msg)) {
            std::cout << "\nSUCCESS: Calibration loaded for camera " << camera_id << std::endl;
            camera_print_calibration_details(params, camera_id);
        } else {
            std::cerr << "\nFAILURE: " << error_msg << std::endl;
        }
    } else {
        std::cerr << "Failed to load H5 file: " << error_msg << std::endl;
        return 1;
    }
    
    std::cout << "\n=== End Diagnostic ===" << std::endl;

    return 0;
}