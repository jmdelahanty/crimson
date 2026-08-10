#include "platform/nvidia/nvidia_stimulus_media_loader.h"

#include "stimulus_playback.h"

#include <algorithm>

namespace crimson::platform::nvidia {

media::StimulusMediaOpenResult
openStimulusMedia(const media::StimulusMediaOpenRequest &request,
                  const NvidiaStimulusMediaOpenContext &context) {
  media::StimulusMediaOpenResult result;
  result.path = request.path;
  if (request.path.empty()) {
    result.error = "No stimulus media path was selected";
    return result;
  }
  if (context.stimulus_player == nullptr || context.zarr_loader == nullptr ||
      context.window_need_decoding == nullptr ||
      context.window_was_decoding == nullptr) {
    result.error = "Stimulus media loader is not configured";
    return result;
  }

  if (!initializeStimulusPlayback(
          *context.stimulus_player, request.path,
          std::max(1, request.buffer_capacity), request.use_cpu_buffer,
          request.use_software_decode, context.cuda_device_index)) {
    result.error = "Failed to load stimulus video: " + request.path;
    return result;
  }

  (*context.window_was_decoding)[context.stimulus_player->window_name] = false;
  (*context.window_need_decoding)[context.stimulus_player->window_name].store(
      false);

  if (request.schedule_initial_seek) {
    scheduleStimulusSeek(*context.stimulus_player, context.zarr_loader,
                         request.initial_camera_frame,
                         request.initial_seek_accurate);
    result.initial_seek_scheduled = true;
  }
  result.ready = true;
  return result;
}

} // namespace crimson::platform::nvidia
