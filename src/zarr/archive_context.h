#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>

namespace crimson::timeline {
class AnalysisSeriesTimelineRepository;
class DetectionQualityTimelineRepository;
class EyeAngleTimelineRepository;
class StimulusContextTimelineRepository;
class SwimBoutTimelineRepository;
} // namespace crimson::timeline

namespace crimson::data {
struct SmallSeriesPreloadPolicy;
}

namespace crimson::zarr {

struct KeypointRepositoryOpenMetrics;
struct KeypointV2RepositoryOpenRequest;
struct KeypointV2RepositoryOpenMetrics;
struct CanonicalDetectionRepositoryOpenMetrics;
struct RefinedDetectionRepositoryOpenMetrics;
struct RefinedDetectionRepositoryOpenOptions;
struct DetectionRepositorySelectionRequest;
struct DetectionRepositorySelectionMetrics;
struct DetectionQualityTimelineOpenRequest;
struct DetectionQualityTimelineOpenMetrics;
struct TensorStoreChaserDistancePolarOptions;
class TensorStoreChaserDistancePolarRepository;

class ArchiveContext {
public:
  struct Impl;

  ~ArchiveContext();

  ArchiveContext(const ArchiveContext &) = delete;
  ArchiveContext &operator=(const ArchiveContext &) = delete;

  static std::shared_ptr<ArchiveContext>
  Open(const std::filesystem::path &root_path,
       std::string *error_message = nullptr);

  const std::filesystem::path &rootPath() const;
  const std::filesystem::path &recordingRootPath() const;
  size_t cachePoolBytes() const;
  std::filesystem::path
  resolveStoredPath(const std::filesystem::path &stored_path) const;

private:
  explicit ArchiveContext(std::shared_ptr<Impl> impl);

  std::shared_ptr<Impl> impl_;

  friend class TensorStoreAffiliatedVideoRepository;
  friend std::unique_ptr<class StimulusRepository>
  OpenStimulusRepository(const std::shared_ptr<ArchiveContext> &archive,
                         const std::string &requested_run,
                         std::string *error_message,
                         const std::string &source_video_override);
  friend std::unique_ptr<class AcquisitionCropRepository>
  OpenAcquisitionCropRepository(const std::shared_ptr<ArchiveContext> &archive,
                                std::string *error_message);
  friend std::unique_ptr<class AnalysisCropGeometryRepository>
  OpenAnalysisCropGeometryRepository(
      const std::shared_ptr<ArchiveContext> &archive,
      const std::string &requested_run, std::string *error_message);
  friend std::unique_ptr<class KeypointOverlayRepository>
  OpenKeypointOverlayRepository(const std::shared_ptr<ArchiveContext> &archive,
                                const std::string &requested_run,
                                std::string *error_message,
                                KeypointRepositoryOpenMetrics *open_metrics);
  friend std::unique_ptr<class KeypointV2Repository>
  OpenKeypointV2Repository(const KeypointV2RepositoryOpenRequest &request,
                           std::string *error_message,
                           KeypointV2RepositoryOpenMetrics *open_metrics);
  friend std::unique_ptr<class CanonicalDetectionRepository>
  OpenCanonicalDetectionRepository(
      const std::shared_ptr<ArchiveContext> &archive,
      const std::string &requested_run, std::string *error_message,
      CanonicalDetectionRepositoryOpenMetrics *open_metrics);
  friend std::unique_ptr<class CanonicalDetectionRepository>
  OpenRefinedDetectionRepository(
      const std::shared_ptr<ArchiveContext> &archive,
      const std::string &requested_run,
      const RefinedDetectionRepositoryOpenOptions &options,
      std::string *error_message,
      RefinedDetectionRepositoryOpenMetrics *open_metrics);
  friend std::unique_ptr<class CanonicalDetectionRepository>
  OpenSelectedDetectionRepository(
      const std::shared_ptr<ArchiveContext> &archive,
      const DetectionRepositorySelectionRequest &request,
      std::string *error_message, DetectionRepositorySelectionMetrics *metrics);
  friend std::unique_ptr<crimson::timeline::DetectionQualityTimelineRepository>
  OpenDetectionQualityTimelineRepository(
      const std::shared_ptr<ArchiveContext> &archive,
      const DetectionQualityTimelineOpenRequest &request,
      std::string *error_message,
      DetectionQualityTimelineOpenMetrics *open_metrics);
  friend std::unique_ptr<class SubjectMaskOverlayRepository>
  OpenSubjectMaskOverlayRepository(
      const std::shared_ptr<ArchiveContext> &archive,
      const std::string &requested_run, std::string *error_message);
  friend std::unique_ptr<class SubjectMaskOverlayRepository>
  OpenSubjectMaskOverlayRepository(
      const std::shared_ptr<ArchiveContext> &archive,
      const struct SubjectMaskOverlayOpenOptions &options,
      std::string *error_message);
  friend std::unique_ptr<class SubjectShapeOverlayRepository>
  OpenSubjectShapeOverlayRepository(
      const std::shared_ptr<ArchiveContext> &archive,
      const std::string &requested_run, std::string *error_message);
  friend std::unique_ptr<class EyeGeometryOverlayRepository>
  OpenEyeGeometryOverlayRepository(
      const std::shared_ptr<ArchiveContext> &archive,
      const std::string &requested_run, std::string *error_message);
  friend std::unique_ptr<crimson::timeline::EyeAngleTimelineRepository>
  OpenEyeAngleTimelineRepository(
      const std::shared_ptr<ArchiveContext> &archive,
      const std::string &requested_run, std::string *error_message,
      crimson::data::SmallSeriesPreloadPolicy preload_policy);
  friend std::unique_ptr<crimson::timeline::AnalysisSeriesTimelineRepository>
  OpenMotionSeriesTimelineRepository(
      const std::shared_ptr<ArchiveContext> &archive,
      std::size_t frame_count_hint, std::string *error_message,
      crimson::data::SmallSeriesPreloadPolicy preload_policy);
  friend std::unique_ptr<crimson::timeline::AnalysisSeriesTimelineRepository>
  OpenTailKinematicsTimelineRepository(
      const std::shared_ptr<ArchiveContext> &archive,
      std::size_t frame_count_hint, const std::string &requested_run,
      std::string *error_message,
      crimson::data::SmallSeriesPreloadPolicy preload_policy);
  friend std::unique_ptr<crimson::timeline::StimulusContextTimelineRepository>
  OpenStimulusContextTimelineRepository(
      const std::shared_ptr<ArchiveContext> &archive,
      std::size_t frame_count_hint, const std::string &requested_run,
      std::string *error_message);
  friend std::unique_ptr<crimson::timeline::SwimBoutTimelineRepository>
  OpenSwimBoutTimelineRepository(const std::shared_ptr<ArchiveContext> &archive,
                                 std::size_t frame_count_hint,
                                 const std::string &requested_run,
                                 std::string *error_message);
  friend std::unique_ptr<TensorStoreChaserDistancePolarRepository>
  OpenTensorStoreChaserDistancePolarRepository(
      const std::shared_ptr<ArchiveContext> &archive,
      const TensorStoreChaserDistancePolarOptions &options,
      std::string *error_message);
};

} // namespace crimson::zarr
