#include "keypoint_reprojection.h"

#ifndef CRIMSON_ENABLE_SFM
#define CRIMSON_ENABLE_SFM 1
#endif

#if CRIMSON_ENABLE_SFM
#include <opencv2/sfm.hpp>
#endif

namespace {

bool is_in_camera_fov(cv::Mat point_world,
                      const cv::Mat &rvec,
                      const cv::Mat &tvec,
                      const cv::Mat &K,
                      int image_width,
                      int image_height) {
    cv::Mat image_pts;
    cv::Mat dist_coeffs = cv::Mat::zeros(5, 1, CV_64F);
    cv::projectPoints(point_world, rvec, tvec, K, dist_coeffs, image_pts);
    const double x = image_pts.at<double>(0, 0);
    const double y = image_height - image_pts.at<double>(0, 1);
    return x > 0 && x < image_width && y > 0 && y < image_height;
}

}  // namespace

#if CRIMSON_ENABLE_SFM
void reprojection(KeyPoints *keypoints,
                  SkeletonContext *skeleton,
                  std::vector<CameraParams> camera_params,
                  render_scene *scene) {
    for (u32 node = 0; node < static_cast<u32>(skeleton->num_nodes); node++) {
        u32 num_views_labeled{0};
        for (u32 view_idx = 0; view_idx < static_cast<u32>(scene->num_cams);
             view_idx++) {
            if (keypoints->keypoints2d[view_idx][node].is_labeled) {
                num_views_labeled++;
            }
        }

        if (num_views_labeled < 2) {
            continue;
        }

        std::vector<cv::Mat> sfmPoints2d;
        std::vector<cv::Mat> projection_matrices;
        cv::Mat output;

        for (u32 view_idx = 0; view_idx < static_cast<u32>(scene->num_cams);
             view_idx++) {
            if (!keypoints->keypoints2d[view_idx][node].is_labeled) {
                continue;
            }

            cv::Mat point =
                (cv::Mat_<double>(2, 1)
                     << keypoints->keypoints2d[view_idx][node].position.x,
                 static_cast<double>(scene->cameras[view_idx].image_height) -
                     keypoints->keypoints2d[view_idx][node].position.y);
            cv::Mat pointUndistort;
            cv::undistortPoints(point, pointUndistort, camera_params[view_idx].k,
                                camera_params[view_idx].dist_coeffs,
                                cv::noArray(), camera_params[view_idx].k);

            sfmPoints2d.push_back(pointUndistort.reshape(1, 2));
            projection_matrices.push_back(camera_params[view_idx].projection_mat);
        }

        cv::sfm::triangulatePoints(sfmPoints2d, projection_matrices, output);

        keypoints->keypoints3d[node].x = output.at<double>(0);
        keypoints->keypoints3d[node].y = output.at<double>(1);
        keypoints->keypoints3d[node].z = output.at<double>(2);

        for (u32 view_idx = 0; view_idx < static_cast<u32>(scene->num_cams);
             view_idx++) {
            if (!is_in_camera_fov(output, camera_params[view_idx].rvec,
                                  camera_params[view_idx].tvec,
                                  camera_params[view_idx].k,
                                  scene->cameras[view_idx].image_width,
                                  scene->cameras[view_idx].image_height)) {
                continue;
            }

            cv::Mat imagePts;
            cv::projectPoints(output, camera_params[view_idx].rvec,
                              camera_params[view_idx].tvec,
                              camera_params[view_idx].k,
                              camera_params[view_idx].dist_coeffs, imagePts);
            const double x = imagePts.at<double>(0, 0);
            const double y =
                static_cast<double>(scene->cameras[view_idx].image_height) -
                imagePts.at<double>(0, 1);
            if (x > 0 && x < scene->cameras[view_idx].image_width && y > 0 &&
                y < scene->cameras[view_idx].image_height) {
                keypoints->keypoints2d[view_idx][node].position.x = x;
                keypoints->keypoints2d[view_idx][node].position.y = y;
                keypoints->keypoints2d[view_idx][node].is_labeled = true;
                keypoints->keypoints2d[view_idx][node].is_triangulated = true;
            }
        }
    }
}
#else
void reprojection(KeyPoints *keypoints,
                  SkeletonContext *skeleton,
                  std::vector<CameraParams> camera_params,
                  render_scene *scene) {
    (void)keypoints;
    (void)skeleton;
    (void)camera_params;
    (void)scene;
}
#endif
