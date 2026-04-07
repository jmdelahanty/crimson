#include "yolo_detection.h"
#include "kernel.cuh"
#include <cstdlib>
#include <memory>

namespace {

std::string makeYoloWorkerName(const char* worker_kind, int camera_id) {
    return std::string(worker_kind) + " cam " + std::to_string(camera_id);
}

void clearYoloErrorMessage(const std::string& worker_name) {
    std::lock_guard<std::mutex> lock(g_decoder_error_mutex);
    g_decoder_error_messages.erase(worker_name);
}

void recordYoloErrorMessage(const std::string& worker_name,
                            int camera_id,
                            const std::string& message) {
    {
        std::lock_guard<std::mutex> lock(g_decoder_error_mutex);
        g_decoder_error_messages[worker_name] = message;
    }
    if (camera_id >= 0 && camera_id < static_cast<int>(g_ready.size())) {
        g_ready[camera_id] = false;
    }
    if (camera_id >= 0 && camera_id < static_cast<int>(yolo_boxes.size())) {
        yolo_boxes[camera_id].clear();
        yolo_labels[camera_id].clear();
        yolo_classid[camera_id].clear();
    }
    std::cerr << "[YOLO] Fatal error in " << worker_name << ": "
              << message << std::endl;
}

} // namespace

void read_yolo_labels(std::string label_names_file, yolo_param* post_setting)
{
    std::ifstream ifs(label_names_file);
    std::vector<std::string> class_list;
    std::string line;    

    while (std::getline(ifs, line))
    {
        class_list.push_back(line);
    }
    post_setting->size_class_list = class_list.size();
    post_setting->class_names = class_list;
}



void yolo_detection(cv::dnn::Net yolo_net, yolo_param* post_setting, unsigned char* yolo_input_frame, int camera_id)
{
    int length_image = 3208 * 2200;
    rgba_to_bgr_cpu(yolo_input_frames_rgba[camera_id], yolo_input_frame, length_image);

    // SimdBgraToBgr(yolo_input_frame_rgba[cam_idx], 3208, 2200, 3208 * 4, yolo_input_frame[cam_idx], 3208 * 3);

    cv::Mat image = cv::Mat(3208 * 2200 * 3, 1, CV_8U, yolo_input_frame).reshape(3, 2200);
    double x_factor = image.cols / 640.0;
    double y_factor = image.rows / 640.0;
    cv::Mat blob;
    cv::dnn::blobFromImage(image, blob, 1./255.,  cv::Size(640, 640),  cv::Scalar(), true, false);
    yolo_net.setInput(blob);
    std::vector<cv::Mat> outs;
    yolo_net.forward(outs, yolo_net.getUnconnectedOutLayersNames());

    std::vector<int> classIds;
    std::vector<float> confidences;
    std::vector<cv::Rect> boxes;
    const int rows = 25200;
    float *data = (float *)outs[0].data;

    for (int i = 0; i < rows; ++i)
    {
        float confidence = data[4];
        if (confidence > post_setting->conf_threshold)
        {
            float *classes_scores = data + 5;
            // Create a 1x85 Mat and store class scores of 80 classes.
            cv::Mat scores(1, post_setting->size_class_list, CV_32FC1, classes_scores);
            // Perform minMaxLoc and acquire the index of best class  score.
            cv::Point class_id;
            double max_class_score;
            minMaxLoc(scores, 0, &max_class_score, 0, &class_id);
            if (max_class_score > post_setting->conf_threshold)
            {
                float cx = data[0];
                float cy = data[1];
                float w = data[2];
                float h = data[3];
                int left = int((cx - 0.5 * w) * x_factor);
                int top = int((cy - 0.5 * h) * y_factor);
                int width = int(w * x_factor);
                int height = int(h * y_factor);
                confidences.push_back((float)confidence);
                classIds.push_back(class_id.x);
                boxes.push_back(cv::Rect(left, top, width, height));
            }
        }
        data += 7;
    }

    std::vector<int> indices;
    std::vector<cv::Rect> final_boxes;
    std::vector<std::string> final_labels;
    std::vector<int> final_class_ids;
    cv::dnn::NMSBoxes(boxes, confidences, post_setting->conf_threshold, post_setting->nma_threshold, indices);
    
    for (size_t i = 0; i < indices.size(); ++i)
    {
        int idx = indices[i];
        cv::Rect box = boxes[idx];
        final_boxes.push_back(box);
        std::stringstream stream;
        stream << " " << std::fixed << std::setprecision(2) << confidences[idx];
        std::string s = post_setting->class_names[classIds[idx]] + stream.str();
        final_labels.push_back(s);
        final_class_ids.push_back((int)classIds[idx]);
    }

    yolo_boxes.at(camera_id) = final_boxes;
    yolo_labels.at(camera_id) = final_labels;
    yolo_classid.at(camera_id) = final_class_ids;
}


void yolo_process(std::string onnx_file, yolo_param* post_setting, int camera_id)
{
    const std::string worker_name = makeYoloWorkerName("YOLO", camera_id);
    clearYoloErrorMessage(worker_name);
    try {
        cv::dnn::Net yolo_net;
        yolo_net = cv::dnn::readNet(onnx_file);
        yolo_net.setPreferableBackend(cv::dnn::DNN_BACKEND_CUDA);
        yolo_net.setPreferableTarget(cv::dnn::DNN_TARGET_CUDA);
        std::cout << "model loaded" << std::endl;

        auto yolo_input_frame = std::unique_ptr<unsigned char, decltype(&std::free)>(
            static_cast<unsigned char*>(std::malloc(3208 * 2200 * 3 * sizeof(uint8_t) + 4)),
            &std::free);
        if (!yolo_input_frame) {
            throw std::runtime_error("Failed to allocate YOLO input frame buffer");
        }

        while (true) {
            std::unique_lock<std::mutex> ul(g_mutexes[camera_id]);
            g_cvs[camera_id].wait(ul, [&]() {return g_ready[camera_id];});
            yolo_detection(yolo_net, post_setting, yolo_input_frame.get(), camera_id);
            g_ready[camera_id] = false;
        }
    } catch (const std::exception& e) {
        recordYoloErrorMessage(worker_name, camera_id, e.what());
    } catch (...) {
        recordYoloErrorMessage(worker_name, camera_id,
                               "Unknown non-standard YOLO worker exception");
    }
}

void yolo_process_v8pose(std::string engine_file, int camera_id, int width, int height)
{
    const std::string worker_name = makeYoloWorkerName("YOLOv8Pose", camera_id);
    clearYoloErrorMessage(worker_name);
    unsigned char *d_convert = nullptr;
    float *d_points = nullptr;
    unsigned int *d_skeleton = nullptr;
    YOLOv8_pose* yolov8_pose = nullptr;
    auto cleanup = [&]() {
        if (d_convert) {
            cudaFree(d_convert);
            d_convert = nullptr;
        }
        if (d_points) {
            cudaFree(d_points);
            d_points = nullptr;
        }
        if (d_skeleton) {
            cudaFree(d_skeleton);
            d_skeleton = nullptr;
        }
        delete yolov8_pose;
        yolov8_pose = nullptr;
    };
    try {
        unsigned int skeleton[8] = {0, 2, 1, 2, 2, 3};

        CHECK(cudaMalloc((void **)&d_convert, width * height * 3));
        yolov8_pose = new YOLOv8_pose(engine_file, width, height);
        yolov8_pose->make_pipe(true);

        cudaMalloc((void **)&d_points, sizeof(float) * 8);
        cudaMalloc((void **)&d_skeleton, sizeof(unsigned int) * 8);
        CHECK(cudaMemcpy(d_skeleton, skeleton, sizeof(unsigned int) * 8, cudaMemcpyHostToDevice));

        std::vector<Object> objs;
        float    score_thres = 0.3f;
        float    iou_thres   = 0.5f;
        int      topk        = 1;

        while (true) {
            std::unique_lock<std::mutex> ul(g_mutexes[camera_id]);
            g_cvs[camera_id].wait(ul, [&]() {return g_ready[camera_id];});

            rgba2rgb_convert(d_convert, yolo_input_frames_rgba[camera_id], width, height, 0);
            yolov8_pose->preprocess_gpu(d_convert);
            yolov8_pose->infer();
            yolov8_pose->postprocess(objs, score_thres, iou_thres, topk);
            yolov8_pose->copy_keypoints_gpu(d_points, objs);
            gpu_draw_rat_pose(yolo_input_frames_rgba[camera_id], width, height, d_points, d_skeleton, yolov8_pose->stream);
            g_ready[camera_id] = false;
        }
    } catch (const std::exception& e) {
        cleanup();
        recordYoloErrorMessage(worker_name, camera_id, e.what());
    } catch (...) {
        cleanup();
        recordYoloErrorMessage(worker_name, camera_id,
                               "Unknown non-standard YOLOv8Pose worker exception");
    }
}


void yolo_process_trt(std::string engine_file, int camera_id, int width, int height)
{
    const std::string worker_name = makeYoloWorkerName("YOLOv8TRT", camera_id);
    clearYoloErrorMessage(worker_name);
    unsigned char *d_convert = nullptr;
    float *d_points = nullptr;
    unsigned int *d_skeleton = nullptr;
    YOLOv8* yolov8 = nullptr;
    auto cleanup = [&]() {
        if (d_convert) {
            cudaFree(d_convert);
            d_convert = nullptr;
        }
        if (d_points) {
            cudaFree(d_points);
            d_points = nullptr;
        }
        if (d_skeleton) {
            cudaFree(d_skeleton);
            d_skeleton = nullptr;
        }
        delete yolov8;
        yolov8 = nullptr;
    };
    try {
        unsigned int skeleton[8] = {0, 1, 1, 2, 2, 3, 3, 0};

        CHECK(cudaMalloc((void **)&d_convert, width * height * 3));
        yolov8 = new YOLOv8(engine_file, width, height);
        yolov8->make_pipe(true);

        cudaMalloc((void **)&d_points, sizeof(float) * 8);
        cudaMalloc((void **)&d_skeleton, sizeof(unsigned int) * 8);
        CHECK(cudaMemcpy(d_skeleton, skeleton, sizeof(unsigned int) * 8, cudaMemcpyHostToDevice));

        std::vector<Object> objs;
        float    score_thres = 0.3f;
        float    iou_thres   = 0.5f;
        int      topk        = 1;

        while (true) {
            std::unique_lock<std::mutex> ul(g_mutexes[camera_id]);
            g_cvs[camera_id].wait(ul, [&]() {return g_ready[camera_id];});

            rgba2rgb_convert(d_convert, yolo_input_frames_rgba[camera_id], width, height, yolov8->stream);
            yolov8->preprocess_gpu(d_convert);
            yolov8->infer();
            yolov8->postprocess(objs);
            yolov8->copy_keypoints_gpu(d_points, objs);
            gpu_draw_rat_pose(yolo_input_frames_rgba[camera_id], width, height, d_points, d_skeleton, yolov8->stream);
            g_ready[camera_id] = false;
        }
    } catch (const std::exception& e) {
        cleanup();
        recordYoloErrorMessage(worker_name, camera_id, e.what());
    } catch (...) {
        cleanup();
        recordYoloErrorMessage(worker_name, camera_id,
                               "Unknown non-standard YOLOv8 TRT worker exception");
    }
}
