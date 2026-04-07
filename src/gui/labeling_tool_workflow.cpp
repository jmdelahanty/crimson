#include "gui/labeling_tool_workflow.h"

#include "keypoint_io.h"
#include "keypoint_reprojection.h"

LabelingToolWorkflowResult applyLabelingToolWindowActions(
    const LabelingToolWindowResult& window_result,
    const LabelingToolWorkflowContext& context) {
    LabelingToolWorkflowResult result;
    result.jump_target_frame = window_result.jump_target_frame;

    if (window_result.request_triangulate) {
        auto keypoint_it = context.legacy_state.keypoints_map.find(
            context.current_frame_num);
        if (keypoint_it != context.legacy_state.keypoints_map.end()) {
            reprojection(keypoint_it->second, context.legacy_state.skeleton.get(),
                         context.camera_params, context.scene);
        }
    }

    if (window_result.request_save) {
        save_keypoints(context.legacy_state.keypoints_map,
                       context.legacy_state.skeleton.get(),
                       context.legacy_state.keypoints_root_folder,
                       context.scene->num_cams,
                       context.camera_names, context.input_is_imgs,
                       context.imgs_names);
        context.legacy_state.last_saved = time(NULL);
    }

    if (window_result.request_load_most_recent) {
        free_all_keypoints(context.legacy_state.keypoints_map, context.scene);
        if (window_result.load_old_format) {
            if (load_keypoints_depreciated(
                    context.legacy_state.keypoints_map,
                    context.legacy_state.skeleton.get(),
                    context.legacy_state.keypoints_root_folder, context.scene,
                    context.camera_names, context.error_message)) {
                free_all_keypoints(context.legacy_state.keypoints_map,
                                   context.scene);
                context.show_error = true;
            }
        } else {
            std::string most_recent_folder;
            if (find_most_recent_labels(
                    context.legacy_state.keypoints_root_folder,
                    most_recent_folder,
                    context.error_message)) {
                context.show_error = true;
            } else if (load_keypoints(
                           most_recent_folder, context.legacy_state.keypoints_map,
                           context.legacy_state.skeleton.get(), context.scene,
                           context.camera_names, context.error_message)) {
                free_all_keypoints(context.legacy_state.keypoints_map,
                                   context.scene);
                context.show_error = true;
            }
        }
    }

    if (window_result.selected_load_folder.has_value()) {
        free_all_keypoints(context.legacy_state.keypoints_map, context.scene);
        if (load_keypoints(*window_result.selected_load_folder,
                           context.legacy_state.keypoints_map,
                           context.legacy_state.skeleton.get(),
                           context.scene, context.camera_names,
                           context.error_message)) {
            free_all_keypoints(context.legacy_state.keypoints_map,
                               context.scene);
            context.show_error = true;
        }
    }

    return result;
}
