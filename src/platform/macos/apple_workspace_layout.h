#pragma once

namespace crimson::macos::workspace {

enum class LayoutProfile {
  Standard,
  CropReference,
  StimulusReference,
};

struct Rect {
  double x = 0.0;
  double y = 0.0;
  double width = 0.0;
  double height = 0.0;

  bool valid() const;
  double right() const { return x + width; }
  double bottom() const { return y + height; }
};

struct MaintainedWorkspaceLayout {
  double scale = 1.0;
  Rect file_browser;
  Rect frame_inspect;
  Rect diagnostics;
  Rect frames_in_buffer;
  Rect camera;
  Rect stimulus_event_timeline;
  Rect analysis_timeline;
  Rect advanced_crop_preview;
  Rect stimulus;
  Rect stimulus_frames_in_buffer;
};

struct CameraInsetLayout {
  Rect crop;
  Rect stimulus;
};

MaintainedWorkspaceLayout makeMaintainedWorkspaceLayout(double width,
                                                        double height);
MaintainedWorkspaceLayout makeMaintainedWorkspaceLayout(double width,
                                                        double height,
                                                        LayoutProfile profile);
Rect fitMedia(Rect bounds, double media_width, double media_height);
Rect constrainToBounds(Rect rect, Rect bounds);
CameraInsetLayout makeCameraInsetLayout(Rect camera_media,
                                        bool include_crop,
                                        bool include_stimulus);

} // namespace crimson::macos::workspace
