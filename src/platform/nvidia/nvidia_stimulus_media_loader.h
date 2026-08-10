#pragma once

#include "stimulus_media_open.h"

#include <atomic>
#include <string>
#include <unordered_map>

struct StimulusPlayback;
class ZarrDetectionLoader;

namespace crimson::platform::nvidia {

struct NvidiaStimulusMediaOpenContext {
  StimulusPlayback *stimulus_player = nullptr;
  ZarrDetectionLoader *zarr_loader = nullptr;
  std::unordered_map<std::string, std::atomic<bool>> *window_need_decoding =
      nullptr;
  std::unordered_map<std::string, bool> *window_was_decoding = nullptr;
  int cuda_device_index = 0;
};

media::StimulusMediaOpenResult
openStimulusMedia(const media::StimulusMediaOpenRequest &request,
                  const NvidiaStimulusMediaOpenContext &context);

} // namespace crimson::platform::nvidia
