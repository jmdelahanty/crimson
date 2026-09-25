#pragma once

#include "stimulus_media_open.h"

#include <atomic>
#include <functional>
#include <string>
#include <unordered_map>

struct StimulusPlayback;
struct PreparedStimulusPlayback;

namespace crimson::zarr {
class StimulusRepository;
}

namespace crimson::platform::nvidia {

struct NvidiaStimulusMediaOpenContext {
  StimulusPlayback *stimulus_player = nullptr;
  const crimson::zarr::StimulusRepository *stimulus_repository = nullptr;
  std::unordered_map<std::string, std::atomic<bool>> *window_need_decoding =
      nullptr;
  std::unordered_map<std::string, bool> *window_was_decoding = nullptr;
  int cuda_device_index = 0;
  PreparedStimulusPlayback *prepared = nullptr;
  std::function<void(StimulusPlayback &)> join_decoder;
  std::function<bool()> opening_cancelled;
  std::function<void()> poll_owner_events;
};

media::StimulusMediaOpenResult
openStimulusMedia(const media::StimulusMediaOpenRequest &request,
                  const NvidiaStimulusMediaOpenContext &context);

} // namespace crimson::platform::nvidia
