#include "keypoint_io.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <regex>
#include <thread>

namespace {

std::string current_date_time() {
    time_t now = time(0);
    struct tm tstruct;
    char buf[80];
    tstruct = *localtime(&now);
    strftime(buf, sizeof(buf), "%Y:%m:%d:%X", &tstruct);

    std::string delimiter = ":";
    std::string s(buf);
    size_t pos_start = 0;
    size_t pos_end = 0;
    const size_t delim_len = delimiter.length();
    std::string token;
    std::vector<std::string> res;

    while ((pos_end = s.find(delimiter, pos_start)) != std::string::npos) {
        token = s.substr(pos_start, pos_end - pos_start);
        pos_start = pos_end + delim_len;
        res.push_back(token);
    }

    res.push_back(s.substr(pos_start));
    std::string final_string;

    for (int i = 0; i < static_cast<int>(res.size()); i++) {
        if (i != 0) {
            final_string += "_";
        }
        final_string += res[i];
    }
    return final_string;
}

void load_2d_keypoints_depreciated(std::map<u32, KeyPoints *> &keypoints_map,
                                   SkeletonContext *skeleton,
                                   std::string root_dir,
                                   int cam_idx,
                                   std::string camera_name,
                                   render_scene *scene) {
    std::string labeled_data_dir = root_dir + "/" + camera_name;
    std::vector<std::string> filenames;

    for (const auto &entry :
         std::filesystem::directory_iterator(labeled_data_dir)) {
        filenames.push_back(entry.path().string());
    }

    if (filenames.empty()) {
        std::cout << "No files in directory for " << camera_name << std::endl;
        return;
    }

    sort(filenames.begin(), filenames.end());
    std::string mostRecentFile = filenames.back();
    std::cout << "mostRecentFile: " << mostRecentFile << std::endl;

    std::ifstream fin;
    fin.open(mostRecentFile);
    if (fin.fail()) {
        throw mostRecentFile;
    }

    std::string line;
    std::string delimeter = ",";
    size_t pos = 0;
    std::string token;

    int lineNum = 0;
    while (!fin.eof()) {
        fin >> line;
        while ((pos = line.find(delimeter)) != std::string::npos) {
            token = line.substr(0, pos);
            if (lineNum == 0) {
                if (token.compare(skeleton->name) != 0) {
                    std::cout << "Failed loading, skeleton doesn't match.\n"
                              << skeleton->name << ":" << token << std::endl;
                    return;
                }
                line.erase(0, pos + delimeter.length());
            } else {
                uint frame_num = stoul(token);
                if (keypoints_map.find(frame_num) == keypoints_map.end()) {
                    KeyPoints *keypoints =
                        (KeyPoints *)malloc(sizeof(KeyPoints));
                    allocate_keypoints(keypoints, scene, skeleton);
                    keypoints_map[frame_num] = keypoints;
                }
                line.erase(0, pos + delimeter.length());

                while ((pos = line.find(delimeter)) != std::string::npos) {
                    token = line.substr(0, pos);
                    int node = stoi(token);
                    line.erase(0, pos + delimeter.length());

                    pos = line.find(delimeter);
                    token = line.substr(0, pos);
                    double x = stod(token);
                    line.erase(0, pos + delimeter.length());

                    pos = line.find(delimeter);
                    token = line.substr(0, pos);
                    double y = stod(token);
                    line.erase(0, pos + delimeter.length());

                    keypoints_map[frame_num]
                        ->keypoints2d[cam_idx][node]
                        .position.x = x;
                    keypoints_map[frame_num]
                        ->keypoints2d[cam_idx][node]
                        .position.y = y;

                    keypoints_map[frame_num]
                        ->keypoints2d[cam_idx][node]
                        .is_labeled = !(x == 1E7 || y == 1E7);
                }
            }
        }
        lineNum++;
    }
    fin.close();
}

int load_2d_keypoints(std::map<u32, KeyPoints *> &keypoints_map,
                      SkeletonContext *skeleton,
                      std::string kp2d_file,
                      int cam_idx,
                      render_scene *scene,
                      std::string &error_message) {
    std::ifstream fin(kp2d_file);
    if (!fin) {
        error_message = "Failed to open: " + kp2d_file;
        return 1;
    }

    std::string line;
    std::string delimeter = ",";
    size_t pos = 0;
    std::string token;

    int lineNum = 0;
    while (!fin.eof()) {
        fin >> line;
        while ((pos = line.find(delimeter)) != std::string::npos) {
            token = line.substr(0, pos);
            if (lineNum == 0) {
                std::cout << token << std::endl;
                std::cout << skeleton->name << std::endl;
                if (token.compare(skeleton->name) != 0) {
                    error_message = "Failed loading, skeleton doesn't match.\n";
                    error_message += skeleton->name + ":" + token;
                    return 1;
                }
                line.erase(0, pos + delimeter.length());
            } else {
                uint frame_num = stoul(token);
                if (keypoints_map.find(frame_num) == keypoints_map.end()) {
                    KeyPoints *keypoints =
                        (KeyPoints *)malloc(sizeof(KeyPoints));
                    allocate_keypoints(keypoints, scene, skeleton);
                    keypoints_map[frame_num] = keypoints;
                }
                line.erase(0, pos + delimeter.length());

                while ((pos = line.find(delimeter)) != std::string::npos) {
                    token = line.substr(0, pos);
                    int node = stoi(token);
                    line.erase(0, pos + delimeter.length());

                    pos = line.find(delimeter);
                    token = line.substr(0, pos);
                    double x = stod(token);
                    line.erase(0, pos + delimeter.length());

                    pos = line.find(delimeter);
                    token = line.substr(0, pos);
                    double y = stod(token);
                    line.erase(0, pos + delimeter.length());

                    keypoints_map[frame_num]
                        ->keypoints2d[cam_idx][node]
                        .position.x = x;
                    keypoints_map[frame_num]
                        ->keypoints2d[cam_idx][node]
                        .position.y = y;

                    keypoints_map[frame_num]
                        ->keypoints2d[cam_idx][node]
                        .is_labeled = !(x == 1E7 || y == 1E7);
                }
            }
        }
        lineNum++;
    }
    fin.close();
    return 0;
}

}  // namespace

void save_keypoints_depreciated(std::map<u32, KeyPoints *> keypoints_map,
                                SkeletonContext *skeleton,
                                std::string root_dir,
                                int num_cameras,
                                std::vector<std::string> &camera_names,
                                bool *input_is_imgs,
                                const std::vector<std::string> &input_files) {
    std::string now = current_date_time();
    std::string filename =
        root_dir + "/worldKeyPoints/keypoints_" + now + ".csv";
    std::ofstream output_file(filename);
    std::vector<std::ofstream> output2d_files;

    for (uint i = 0; i < static_cast<uint>(num_cameras); i++) {
        std::string filename_cam = root_dir + "/" + camera_names[i] + "/" +
                                   camera_names[i] + "_" + now + ".csv";
        std::ofstream output_file_cam(filename_cam);
        output2d_files.push_back(std::move(output_file_cam));
    }

    output_file << skeleton->name << ",\n";
    for (uint i = 0; i < static_cast<uint>(num_cameras); i++) {
        output2d_files[i] << skeleton->name << ",\n";
    }

    auto it = keypoints_map.begin();
    while (it != keypoints_map.end()) {
        uint frame = it->first;
        KeyPoints *keypoints = it->second;
        if (*input_is_imgs) {
            output_file << input_files[frame] << ",";
        } else {
            output_file << frame << ",";
        }
        for (uint i = 0; i < static_cast<uint>(skeleton->num_nodes); i++) {
            output_file << i << "," << keypoints->keypoints3d[i].x << ","
                        << keypoints->keypoints3d[i].y << ","
                        << keypoints->keypoints3d[i].z << ",";
        }
        output_file << "\n";

        for (int cam = 0; cam < num_cameras; cam++) {
            if (*input_is_imgs) {
                output2d_files[cam] << input_files[frame] << ",";
            } else {
                output2d_files[cam] << frame << ",";
            }
            for (int node = 0; node < skeleton->num_nodes; node++) {
                output2d_files[cam]
                    << node << ","
                    << keypoints->keypoints2d[cam][node].position.x << ","
                    << keypoints->keypoints2d[cam][node].position.y << ",";
            }
            output2d_files[cam] << "\n";
        }

        it++;
    }

    output_file.close();
    std::cout << filename << " created" << std::endl;

    for (uint i = 0; i < static_cast<uint>(num_cameras); i++) {
        output2d_files[i].close();
    }
}

void save_keypoints(std::map<u32, KeyPoints *> keypoints_map,
                    SkeletonContext *skeleton,
                    std::string root_dir,
                    int num_cameras,
                    std::vector<std::string> &camera_names,
                    bool *input_is_imgs,
                    const std::vector<std::string> &input_files) {
    std::string now = current_date_time();
    std::string save_folder = root_dir + "/" + now;
    std::filesystem::create_directories(save_folder);
    std::string filename = save_folder + "/keypoints3d.csv";

    std::ofstream output3d_file(filename);
    std::vector<std::ofstream> output2d_files;

    for (uint i = 0; i < static_cast<uint>(num_cameras); i++) {
        std::string filename_cam = save_folder + "/" + camera_names[i] + ".csv";
        std::ofstream output_file_cam(filename_cam);
        output2d_files.push_back(std::move(output_file_cam));
    }

    output3d_file << skeleton->name << "\n";
    for (uint i = 0; i < static_cast<uint>(num_cameras); i++) {
        output2d_files[i] << skeleton->name << "\n";
    }

    auto it = keypoints_map.begin();
    while (it != keypoints_map.end()) {
        uint frame = it->first;
        KeyPoints *keypoints = it->second;

        if (*input_is_imgs) {
            output3d_file << input_files[frame];
        } else {
            output3d_file << frame;
        }

        for (uint i = 0; i < static_cast<uint>(skeleton->num_nodes); i++) {
            output3d_file << "," << i << "," << keypoints->keypoints3d[i].x
                          << "," << keypoints->keypoints3d[i].y << ","
                          << keypoints->keypoints3d[i].z;
        }
        output3d_file << "\n";

        for (int cam = 0; cam < num_cameras; cam++) {
            if (*input_is_imgs) {
                output2d_files[cam] << input_files[frame];
            } else {
                output2d_files[cam] << frame;
            }
            for (int node = 0; node < skeleton->num_nodes; node++) {
                output2d_files[cam]
                    << "," << node << ","
                    << keypoints->keypoints2d[cam][node].position.x << ","
                    << keypoints->keypoints2d[cam][node].position.y;
            }
            output2d_files[cam] << "\n";
        }

        it++;
    }
    output3d_file.close();
    for (uint i = 0; i < static_cast<uint>(num_cameras); i++) {
        output2d_files[i].close();
    }
}

int load_keypoints_depreciated(std::map<u32, KeyPoints *> &keypoints_map,
                               SkeletonContext *skeleton,
                               std::string root_dir,
                               render_scene *scene,
                               std::vector<std::string> &camera_names,
                               std::string &error_message) {
    if (scene->num_cams > 1) {
        if (!std::filesystem::exists(root_dir + "/worldKeyPoints")) {
            error_message =
                "'worldKeyPoints' directory is missing from: " + root_dir;
            return 1;
        }

        std::string label3d_dir = root_dir + "/worldKeyPoints/";
        std::vector<std::string> filenames;

        for (const auto &entry :
             std::filesystem::directory_iterator(label3d_dir)) {
            filenames.push_back(entry.path().string());
        }

        if (filenames.empty()) {
            error_message = "Failed loading, no files in directory.";
            return 1;
        }

        sort(filenames.begin(), filenames.end());
        std::string mostRecentFile = filenames.back();
        std::cout << "mostRecentFile: " << mostRecentFile << std::endl;

        std::ifstream fin;
        fin.open(mostRecentFile);
        if (fin.fail()) {
            throw mostRecentFile;
        }
        std::string line;
        std::string delimeter = ",";
        size_t pos = 0;
        std::string token;

        int lineNum = 0;
        while (!fin.eof()) {
            fin >> line;
            while ((pos = line.find(delimeter)) != std::string::npos) {
                token = line.substr(0, pos);
                if (lineNum == 0) {
                    if (token.compare(skeleton->name) != 0) {
                        error_message = "Failed loading 3d keypoints, skeleton "
                                        "doesn't match.\n";
                        error_message += skeleton->name + ":" + token;
                        return 1;
                    }
                    line.erase(0, pos + delimeter.length());
                } else {
                    uint frame_num = stoul(token);
                    if (keypoints_map.find(frame_num) == keypoints_map.end()) {
                        KeyPoints *keypoints =
                            (KeyPoints *)malloc(sizeof(KeyPoints));
                        allocate_keypoints(keypoints, scene, skeleton);
                        keypoints_map[frame_num] = keypoints;
                    }
                    line.erase(0, pos + delimeter.length());

                    while ((pos = line.find(delimeter)) != std::string::npos) {
                        token = line.substr(0, pos);
                        int node = stoi(token);
                        line.erase(0, pos + delimeter.length());

                        pos = line.find(delimeter);
                        token = line.substr(0, pos);
                        double x = stod(token);
                        line.erase(0, pos + delimeter.length());

                        pos = line.find(delimeter);
                        token = line.substr(0, pos);
                        double y = stod(token);
                        line.erase(0, pos + delimeter.length());

                        pos = line.find(delimeter);
                        token = line.substr(0, pos);
                        double z = stod(token);
                        line.erase(0, pos + delimeter.length());

                        keypoints_map[frame_num]->keypoints3d[node].x = x;
                        keypoints_map[frame_num]->keypoints3d[node].y = y;
                        keypoints_map[frame_num]->keypoints3d[node].z = z;

                        const bool triangulated = !(x == 1E7 || y == 1E7 || z == 1E7);
                        for (int cam_idx = 0; cam_idx < scene->num_cams;
                             cam_idx++) {
                            keypoints_map[frame_num]
                                ->keypoints2d[cam_idx][node]
                                .is_triangulated = triangulated;
                        }
                    }
                }
            }
            lineNum++;
        }
        fin.close();
    }

    auto handles = std::vector<std::thread>();
    for (int i = 0; i < scene->num_cams; i++) {
        handles.push_back(std::thread(&load_2d_keypoints_depreciated,
                                      std::ref(keypoints_map), skeleton,
                                      root_dir, i, camera_names[i], scene));
    }

    for (auto &handle : handles) {
        handle.join();
    }
    return 0;
}

int find_most_recent_labels(std::string root_dir,
                            std::string &most_recent_file,
                            std::string &error_message) {
    std::regex datetime_regex(R"(^\d{4}_\d{2}_\d{2}_\d{2}_\d{2}_\d{2}$)");

    std::vector<std::string> filenames;
    for (const auto &entry : std::filesystem::directory_iterator(root_dir)) {
        if (!entry.is_directory()) {
            continue;
        }

        std::string folder_name = entry.path().filename().string();
        if (std::regex_match(folder_name, datetime_regex)) {
            filenames.push_back(entry.path().string());
        }
    }

    if (filenames.empty()) {
        error_message = "Failed loading, no date-time named folders found.";
        error_message +=
            "\nIf you are loading an old format, please check 'Old Format'. "
            "Once loaded, please save it to convert to the new format.";
        return 1;
    }
    sort(filenames.begin(), filenames.end());
    most_recent_file = filenames.back();
    std::cout << most_recent_file << std::endl;
    return 0;
}

int load_keypoints(std::string keypoints_folder,
                   std::map<u32, KeyPoints *> &keypoints_map,
                   SkeletonContext *skeleton,
                   render_scene *scene,
                   std::vector<std::string> &camera_names,
                   std::string &error_message) {
    if (scene->num_cams > 1) {
        std::string kp_3d = keypoints_folder + "/keypoints3d.csv";
        std::ifstream fin(kp_3d);
        if (!fin) {
            error_message = "Failed to open: " + kp_3d;
            return 1;
        }

        std::string line;
        std::string delimeter = ",";
        size_t pos = 0;
        std::string token;

        int line_num = 0;
        while (!fin.eof()) {
            fin >> line;
            while ((pos = line.find(delimeter)) != std::string::npos) {
                token = line.substr(0, pos);
                if (line_num == 0) {
                    if (token.compare(skeleton->name) != 0) {
                        error_message = "3D keypoints failed loading, skeleton "
                                        "doesn't match.";
                        error_message += skeleton->name + ":" + token;
                        return 1;
                    }
                    line.erase(0, pos + delimeter.length());
                } else {
                    uint frame_num = stoul(token);
                    if (keypoints_map.find(frame_num) == keypoints_map.end()) {
                        KeyPoints *keypoints =
                            (KeyPoints *)malloc(sizeof(KeyPoints));
                        allocate_keypoints(keypoints, scene, skeleton);
                        keypoints_map[frame_num] = keypoints;
                    }
                    line.erase(0, pos + delimeter.length());

                    while ((pos = line.find(delimeter)) != std::string::npos) {
                        token = line.substr(0, pos);
                        int node = stoi(token);
                        line.erase(0, pos + delimeter.length());

                        pos = line.find(delimeter);
                        token = line.substr(0, pos);
                        double x = stod(token);
                        line.erase(0, pos + delimeter.length());

                        pos = line.find(delimeter);
                        token = line.substr(0, pos);
                        double y = stod(token);
                        line.erase(0, pos + delimeter.length());

                        pos = line.find(delimeter);
                        token = line.substr(0, pos);
                        double z = stod(token);
                        line.erase(0, pos + delimeter.length());

                        keypoints_map[frame_num]->keypoints3d[node].x = x;
                        keypoints_map[frame_num]->keypoints3d[node].y = y;
                        keypoints_map[frame_num]->keypoints3d[node].z = z;

                        const bool triangulated = !(x == 1E7 || y == 1E7 || z == 1E7);
                        for (int cam_idx = 0; cam_idx < scene->num_cams;
                             cam_idx++) {
                            keypoints_map[frame_num]
                                ->keypoints2d[cam_idx][node]
                                .is_triangulated = triangulated;
                        }
                    }
                }
            }
            line_num++;
        }
        fin.close();
    }

    std::vector<std::thread> handles;
    std::vector<std::promise<int>> promises(scene->num_cams);
    std::vector<std::future<int>> results;
    std::vector<std::string> error_messages(scene->num_cams);

    for (int i = 0; i < scene->num_cams; i++) {
        std::string kp2d = keypoints_folder + "/" + camera_names[i] + ".csv";
        results.push_back(promises[i].get_future());

        handles.emplace_back(
            [&keypoints_map, skeleton, kp2d, i, scene, &error_messages,
             &promises](int cam_idx) {
                int ret = load_2d_keypoints(keypoints_map, skeleton, kp2d, i,
                                            scene, error_messages[i]);
                promises[cam_idx].set_value(ret);
            },
            i);
    }

    for (auto &t : handles) {
        t.join();
    }

    bool has_error = false;
    for (int i = 0; i < static_cast<int>(results.size()); ++i) {
        int status = results[i].get();
        if (status != 0) {
            error_message +=
                camera_names[i] + " error: " + error_messages[i] + "\n";
            has_error = true;
        }
    }

    return has_error ? 1 : 0;
}
