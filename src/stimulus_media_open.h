#pragma once

#include <string>

namespace crimson::media {

struct StimulusMediaOpenRequest {
  std::string path;
  int buffer_capacity = 1;
  bool use_cpu_buffer = false;
  bool use_software_decode = false;
  bool schedule_initial_seek = false;
  int initial_camera_frame = 0;
  bool initial_seek_accurate = false;
};

struct StimulusMediaOpenResult {
  bool ready = false;
  bool initial_seek_scheduled = false;
  std::string path;
  std::string error;
};

} // namespace crimson::media
