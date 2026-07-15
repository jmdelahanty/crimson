#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>

namespace crimson::timeline {
class AnalysisSeriesTimelineRepository;
class EyeAngleTimelineRepository;
}

namespace crimson::zarr {

class ArchiveContext {
 public:
  struct Impl;

  ~ArchiveContext();

  ArchiveContext(const ArchiveContext&) = delete;
  ArchiveContext& operator=(const ArchiveContext&) = delete;

  static std::shared_ptr<ArchiveContext> Open(
      const std::filesystem::path& root_path,
      std::string* error_message = nullptr);

  const std::filesystem::path& rootPath() const;
  const std::filesystem::path& recordingRootPath() const;
  std::filesystem::path resolveStoredPath(
      const std::filesystem::path& stored_path) const;

 private:
  explicit ArchiveContext(std::shared_ptr<Impl> impl);

  std::shared_ptr<Impl> impl_;

  friend std::unique_ptr<class StimulusRepository> OpenStimulusRepository(
      const std::shared_ptr<ArchiveContext>& archive,
      const std::string& requested_run,
      std::string* error_message);
  friend std::unique_ptr<class AcquisitionCropRepository>
  OpenAcquisitionCropRepository(
      const std::shared_ptr<ArchiveContext>& archive,
      std::string* error_message);
  friend std::unique_ptr<class AnalysisCropGeometryRepository>
  OpenAnalysisCropGeometryRepository(
      const std::shared_ptr<ArchiveContext>& archive,
      const std::string& requested_run,
      std::string* error_message);
  friend std::unique_ptr<class KeypointOverlayRepository>
  OpenKeypointOverlayRepository(
      const std::shared_ptr<ArchiveContext>& archive,
      const std::string& requested_run,
      std::string* error_message);
  friend std::unique_ptr<class SubjectMaskOverlayRepository>
  OpenSubjectMaskOverlayRepository(
      const std::shared_ptr<ArchiveContext>& archive,
      const std::string& requested_run,
      std::string* error_message);
  friend std::unique_ptr<class SubjectShapeOverlayRepository>
  OpenSubjectShapeOverlayRepository(
      const std::shared_ptr<ArchiveContext>& archive,
      const std::string& requested_run,
      std::string* error_message);
  friend std::unique_ptr<class EyeGeometryOverlayRepository>
  OpenEyeGeometryOverlayRepository(
      const std::shared_ptr<ArchiveContext>& archive,
      const std::string& requested_run,
      std::string* error_message);
  friend std::unique_ptr<crimson::timeline::EyeAngleTimelineRepository>
  OpenEyeAngleTimelineRepository(
      const std::shared_ptr<ArchiveContext>& archive,
      const std::string& requested_run,
      std::string* error_message);
  friend std::unique_ptr<crimson::timeline::AnalysisSeriesTimelineRepository>
  OpenMotionSeriesTimelineRepository(
      const std::shared_ptr<ArchiveContext>& archive,
      std::size_t frame_count_hint,
      std::string* error_message);
  friend std::unique_ptr<crimson::timeline::AnalysisSeriesTimelineRepository>
  OpenTailKinematicsTimelineRepository(
      const std::shared_ptr<ArchiveContext>& archive,
      std::size_t frame_count_hint,
      const std::string& requested_run,
      std::string* error_message);
};

}  // namespace crimson::zarr
