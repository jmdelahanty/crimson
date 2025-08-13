#ifndef RED_CAMERA
#define RED_CAMERA
#include "json.hpp"
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
};

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