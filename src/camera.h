#ifndef RED_CAMERA
#define RED_CAMERA

#include "json.hpp"
#include "h5_loader.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <opencv2/calib3d.hpp>
#include <opencv2/core.hpp>
#include "/opt/orange/lib/opencv/include/opencv4/opencv2/sfm.hpp"
#include <string>
#include <vector>

using json = nlohmann::json;

struct CameraParams {
    cv::Mat k;
    cv::Mat dist_coeffs;
    cv::Mat r;
    cv::Mat rvec;
    cv::Mat tvec;
    cv::Mat projection_mat;
    cv::Mat homography_matrix;
    cv::Mat inverse_homography_matrix;
    
    // Additional calibration metadata
    std::string calibration_timestamp;
    float pixels_per_mm_projector = 0.0f;
    float pixels_per_mm_camera = 0.0f;
    float real_world_ref_mm = 0.0f;
    bool has_valid_homography = false;
};

bool camera_load_calibration_from_h5_enhanced(const H5SessionData& h5_data,
                                              const std::string& camera_id_str,
                                              CameraParams& camera_params,
                                              std::string& error_message) {
    error_message.clear();
    
    try {
        // First, try to load from the enhanced camera calibrations
        auto it = h5_data.camera_calibrations.find(camera_id_str);
        if (it != h5_data.camera_calibrations.end() && it->second.has_homography) {
            const CameraCalibrationData& calib_data = it->second;
            
            std::cout << "Loading homography from YAML for camera: " << camera_id_str << std::endl;
            
            // Copy the homography matrix
            camera_params.homography_matrix = calib_data.homography_matrix.clone();
            
            // Calculate and store the inverse
            if (cv::invert(camera_params.homography_matrix, camera_params.inverse_homography_matrix)) {
                camera_params.has_valid_homography = true;
                camera_params.calibration_timestamp = calib_data.calibration_timestamp_utc;
                
                // Copy additional attributes if available
                if (calib_data.has_attributes) {
                    camera_params.pixels_per_mm_projector = calib_data.pixels_per_mm_projector;
                    camera_params.pixels_per_mm_camera = calib_data.pixels_per_mm_camera;
                    camera_params.real_world_ref_mm = calib_data.real_world_ref_mm;
                }
                
                std::cout << "  Successfully loaded homography matrix (3x3):" << std::endl;
                std::cout << camera_params.homography_matrix << std::endl;
                std::cout << "  Calibration timestamp: " << camera_params.calibration_timestamp << std::endl;
                
                return true;
            } else {
                error_message = "Failed to invert homography matrix for camera ID " + camera_id_str;
                std::cerr << error_message << std::endl;
            }
        }
        
        // Fall back to JSON method if YAML loading failed
        std::cout << "Falling back to JSON arena_config for camera: " << camera_id_str << std::endl;
        
        if (h5_data.arena_config_json.empty()) {
            error_message = "No arena configuration found in H5 file";
            return false;
        }
        
        json config = json::parse(h5_data.arena_config_json);
        
        if (config.contains("camera_calibrations")) {
            for (const auto& calib : config["camera_calibrations"]) {
                std::string current_cam_id_str;
                if (calib["camera_id"].is_string()) {
                    current_cam_id_str = calib["camera_id"].get<std::string>();
                } else if (calib["camera_id"].is_number()) {
                    current_cam_id_str = std::to_string(calib["camera_id"].get<uint32_t>());
                }
                
                if (current_cam_id_str == camera_id_str) {
                    if (calib.contains("homography_matrix") && calib["homography_matrix"].contains("data")) {
                        std::vector<double> h_vec = calib["homography_matrix"]["data"].get<std::vector<double>>();
                        if (h_vec.size() == 9) {
                            camera_params.homography_matrix = cv::Mat(h_vec, true).reshape(1, 3);
                            
                            // Calculate and store the inverse
                            if (cv::invert(camera_params.homography_matrix, camera_params.inverse_homography_matrix)) {
                                camera_params.has_valid_homography = true;
                                std::cout << "  Loaded homography from JSON (fallback method)" << std::endl;
                                return true;
                            }
                            error_message = "Failed to invert homography matrix for camera ID " + camera_id_str;
                            return false;
                        }
                    }
                }
            }
        }
        
        error_message = "Homography matrix not found for camera ID " + camera_id_str + " in H5 data";
        return false;
        
    } catch (const json::exception& e) {
        error_message = "Failed to parse H5 calibration JSON: " + std::string(e.what());
        return false;
    } catch (const cv::Exception& e) {
        error_message = "OpenCV error: " + std::string(e.what());
        return false;
    } catch (const std::exception& e) {
        error_message = "Error loading calibration: " + std::string(e.what());
        return false;
    }
}

// Keep the original function for backward compatibility but have it call the enhanced version
bool camera_load_calibration_from_h5_json(const std::string& arena_config_json,
                                          const std::string& camera_id_str,
                                          CameraParams& camera_params,
                                          std::string& error_message) {
    // Create a temporary H5SessionData with just the JSON
    H5SessionData temp_data;
    temp_data.arena_config_json = arena_config_json;
    
    // Call the enhanced function which will fall back to JSON parsing
    return camera_load_calibration_from_h5_enhanced(temp_data, camera_id_str, camera_params, error_message);
}

// Debug function to print calibration details
void camera_print_calibration_details(const CameraParams& params, const std::string& camera_id) {
    std::cout << "\n=== Calibration Details for Camera: " << camera_id << " ===" << std::endl;
    
    if (params.has_valid_homography) {
        std::cout << "Homography Status: VALID" << std::endl;
        std::cout << "Homography Matrix:" << std::endl;
        std::cout << params.homography_matrix << std::endl;
        
        std::cout << "Inverse Homography Matrix:" << std::endl;
        std::cout << params.inverse_homography_matrix << std::endl;
        
        if (!params.calibration_timestamp.empty()) {
            std::cout << "Calibration Timestamp: " << params.calibration_timestamp << std::endl;
        }
        
        if (params.pixels_per_mm_projector > 0) {
            std::cout << "Pixels per mm (projector): " << params.pixels_per_mm_projector << std::endl;
        }
        if (params.pixels_per_mm_camera > 0) {
            std::cout << "Pixels per mm (camera): " << params.pixels_per_mm_camera << std::endl;
        }
        if (params.real_world_ref_mm > 0) {
            std::cout << "Real world reference (mm): " << params.real_world_ref_mm << std::endl;
        }
    } else {
        std::cout << "Homography Status: NOT LOADED" << std::endl;
    }
    
    if (!params.k.empty()) {
        std::cout << "\nCamera Matrix K:" << std::endl;
        std::cout << params.k << std::endl;
    }
    
    if (!params.dist_coeffs.empty()) {
        std::cout << "\nDistortion Coefficients:" << std::endl;
        std::cout << params.dist_coeffs << std::endl;
    }
    
    std::cout << "=================================" << std::endl;
}


void camera_print_parameters(CameraParams *cvp) {
    std::cout << "k = " << std::endl
              << cv::format(cvp->k, cv::Formatter::FMT_PYTHON) << std::endl
              << std::endl;
    std::cout << "dist_coeffs  = " << std::endl
              << cv::format(cvp->dist_coeffs, cv::Formatter::FMT_PYTHON)
              << std::endl
              << std::endl;
    std::cout << "r = " << std::endl
              << cv::format(cvp->r, cv::Formatter::FMT_PYTHON) << std::endl
              << std::endl;
    std::cout << "tvec = " << std::endl
              << cv::format(cvp->tvec, cv::Formatter::FMT_PYTHON) << std::endl
              << std::endl;
    std::cout << "rvec = " << std::endl
              << cv::format(cvp->rvec, cv::Formatter::FMT_PYTHON) << std::endl
              << std::endl;
    std::cout << "projection_mat = " << std::endl
              << cv::format(cvp->projection_mat, cv::Formatter::FMT_PYTHON)
              << std::endl
              << std::endl;
}

bool camera_load_params_from_yaml(const std::string &calibration_file,
                                  CameraParams &camera_params,
                                  std::string &error_message) {
    error_message.clear();

    if (!std::filesystem::exists(calibration_file)) {
        error_message = "File does not exist: " + calibration_file;
        return false;
    }

    cv::FileStorage fs(calibration_file, cv::FileStorage::READ);
    if (!fs.isOpened()) {
        error_message = "Could not open file: " + calibration_file;
        return false;
    }

    fs["camera_matrix"] >> camera_params.k;
    fs["distortion_coefficients"] >> camera_params.dist_coeffs;
    fs["tc_ext"] >> camera_params.tvec;
    fs["rc_ext"] >> camera_params.r;
    fs.release();

    if (camera_params.k.empty() || camera_params.r.empty() ||
        camera_params.tvec.empty()) {
        error_message = "Missing fields in: " + calibration_file;
        return false;
    }

    cv::Rodrigues(camera_params.r, camera_params.rvec);
    cv::sfm::projectionFromKRt(camera_params.k, camera_params.r,
                               camera_params.tvec,
                               camera_params.projection_mat);
    return true;
}

bool camera_load_calibration_from_h5_json(const std::string& arena_config_json,
                                          const std::string& camera_id_str,
                                          CameraParams& camera_params,
                                          std::string& error_message) {
    error_message.clear();
    try {
        json config = json::parse(arena_config_json);

        if (config.contains("camera_calibrations")) {
            for (const auto& calib : config["camera_calibrations"]) {
                std::string current_cam_id_str;
                 if (calib["camera_id"].is_string()) {
                    current_cam_id_str = calib["camera_id"].get<std::string>();
                } else if (calib["camera_id"].is_number()) {
                    current_cam_id_str = std::to_string(calib["camera_id"].get<uint32_t>());
                }


                if (current_cam_id_str == camera_id_str) {
                    if (calib.contains("homography_matrix") && calib["homography_matrix"].contains("data")) {
                        std::vector<double> h_vec = calib["homography_matrix"]["data"].get<std::vector<double>>();
                        if (h_vec.size() == 9) {
                            camera_params.homography_matrix = cv::Mat(h_vec, true).reshape(1, 3);
                            // Calculate and store the inverse
                            if(cv::invert(camera_params.homography_matrix, camera_params.inverse_homography_matrix)) {
                                return true;
                            }
                            error_message = "Failed to invert homography matrix for camera ID " + camera_id_str;
                            return false;
                        }
                    }
                }
            }
        }
        error_message = "Homography matrix not found for camera ID " + camera_id_str + " in H5 calibration data.";
        return false;
    } catch (const json::exception& e) {
        error_message = "Failed to parse H5 calibration JSON: " + std::string(e.what());
        return false;
    } catch (const std::invalid_argument& e) {
        error_message = "Invalid camera ID format: " + std::string(e.what());
        return false;
    }
    return false;
}


CameraParams camera_load_params_from_csv(std::string csv_filename,
                                         int cam_idx) {
    std::cout << csv_filename << std::endl;
    CameraParams cvp;

    std::ifstream fin;
    fin.open(csv_filename);
    if (fin.fail())
        throw csv_filename;

    std::string line;
    std::string delimeter = ",";
    size_t pos = 0;
    std::string token;

    // read csv file with cam parameters and tokenize line for this camera
    int lineNum = 0;
    std::vector<float> csvCamValues;

    while (!fin.eof()) {
        fin >> line;

        while ((pos = line.find(delimeter)) != std::string::npos) {
            token = line.substr(0, pos);
            if (lineNum == cam_idx) {
                csvCamValues.push_back(stof(token));
            }
            line.erase(0, pos + delimeter.length());
        }
        lineNum++;
    }

    std::vector<float> k;   // 9
    std::vector<float> r_m; // 9
    std::vector<float> t;   // 3
    std::vector<float> d;   // 4

    for (int i = 0; i < 9; i++) {
        k.push_back(csvCamValues[i]);
    }
    for (int i = 9; i < 18; i++) {
        r_m.push_back(csvCamValues[i]);
    }
    for (int i = 18; i < 21; i++) {
        t.push_back(csvCamValues[i]);
    }
    for (int i = 21; i < 25; i++) {
        d.push_back(csvCamValues[i]);
    }

    cvp.k = cv::Mat_<float>(k, true).reshape(0, 3);
    cvp.dist_coeffs = cv::Mat_<float>(d, true);
    cvp.r = cv::Mat_<float>(r_m, true).reshape(0, 3);
    cvp.tvec = cv::Mat_<float>(t, true);
    cv::Rodrigues(cvp.r, cvp.rvec);
    cv::sfm::projectionFromKRt(cvp.k, cvp.r, cvp.tvec, cvp.projection_mat);
    return cvp;
}

#endif