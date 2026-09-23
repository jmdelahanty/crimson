#include "review_frame_index.h"

#include <iostream>

namespace {

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__           \
                << ": " #condition << '\n';                                    \
      return false;                                                            \
    }                                                                          \
  } while (false)

class FakeDetectionRepository final
    : public crimson::zarr::DetectionRepository {
public:
  FakeDetectionRepository() {
    descriptor_.archive_path = "fixture.zarr";
    descriptor_.run_name = "refined_fixture";
    descriptor_.total_frames = 4;
    descriptor_.available = true;
    descriptor_.active_dataset = crimson::zarr::DetectionDataset::RefinedRoot;
    frames_.resize(4);
    for (size_t frame_id = 0; frame_id < frames_.size(); ++frame_id) {
      frames_[frame_id].status = crimson::zarr::DetectionFrameStatus::Ready;
      frames_[frame_id].frame_id = frame_id;
    }
    frames_[0].observations.resize(2);
    frames_[2].observations.resize(1);
    frames_[2].interpolated = true;
    frames_[3].observations.resize(3);
    frames_[3].observations[1].source_kind = 1;
    frames_[3].observations[1].reason = "Manual";
  }

  crimson::zarr::DetectionRepositoryDescriptor descriptor() const override {
    return descriptor_;
  }
  std::vector<crimson::zarr::DetectionDatasetOption>
  availableDatasets() const override {
    return {{descriptor_.active_dataset, "fixture"}};
  }
  bool selectDataset(crimson::zarr::DetectionDataset dataset) override {
    descriptor_.active_dataset = dataset;
    return true;
  }
  bool isDatasetAvailable(crimson::zarr::DetectionDataset) const override {
    return true;
  }
  size_t observationCount(size_t frame_id) const override {
    return frame_id < frames_.size() ? frames_[frame_id].observations.size()
                                     : 0;
  }
  bool isFrameInterpolated(size_t frame_id) const override {
    return frame_id == 2;
  }
  crimson::zarr::DetectionFrame resolveFrame(size_t frame_id,
                                             bool) const override {
    ++resolve_calls;
    return frames_.at(frame_id);
  }

  mutable size_t resolve_calls = 0;

private:
  crimson::zarr::DetectionRepositoryDescriptor descriptor_;
  std::vector<crimson::zarr::DetectionFrame> frames_;
};

bool testIndependentFiltersAndCompleteRows() {
  FakeDetectionRepository repository;
  ReviewFrameCache cache;
  ReviewFrameFilters filters;
  filters.include_interpolated = false;
  filters.include_non_clean = false;
  filters.include_empty = true;
  ensureReviewFrameIndex(true, repository, filters, cache);
  CHECK(cache.frames == std::vector<int>{1});

  invalidateReviewFrameCache(cache);
  filters = {};
  filters.include_interpolated = true;
  filters.include_non_clean = false;
  filters.include_empty = false;
  ensureReviewFrameIndex(true, repository, filters, cache);
  CHECK(cache.frames == std::vector<int>{2});

  invalidateReviewFrameCache(cache);
  filters.include_interpolated = false;
  filters.include_non_clean = true;
  ensureReviewFrameIndex(true, repository, filters, cache);
  CHECK(cache.frames == std::vector<int>{3});
  CHECK(repository.resolve_calls == 3);
  return true;
}

bool testCacheAndWrappedNavigation() {
  FakeDetectionRepository repository;
  ReviewFrameCache cache;
  ReviewFrameFilters filters;
  ensureReviewFrameIndex(true, repository, filters, cache);
  CHECK(cache.frames == std::vector<int>({1, 2, 3}));
  const size_t first_resolve_calls = repository.resolve_calls;
  ensureReviewFrameIndex(true, repository, filters, cache);
  CHECK(repository.resolve_calls == first_resolve_calls);

  const auto next =
      computeReviewFrameJump(true, repository, filters, cache, 3, true);
  CHECK(next.target_frame == 1);
  const auto previous =
      computeReviewFrameJump(true, repository, filters, cache, 1, false);
  CHECK(previous.target_frame == 3);

  CHECK(repository.selectDataset(
      crimson::zarr::DetectionDataset::RefinedFiltered));
  ensureReviewFrameIndex(true, repository, filters, cache);
  CHECK(cache.dataset == crimson::zarr::DetectionDataset::RefinedFiltered);
  return true;
}

} // namespace

int main() {
  if (!testIndependentFiltersAndCompleteRows() ||
      !testCacheAndWrappedNavigation()) {
    return 1;
  }
  std::cout << "review_frame_index_tests: PASS\n";
  return 0;
}
