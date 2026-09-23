#include "zarr/review_write_repository.h"

#include <iostream>
#include <memory>

namespace {

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__           \
                << ": " #condition << '\n';                                    \
      return false;                                                            \
    }                                                                          \
  } while (false)

class FakeReviewWriteRepository final
    : public crimson::zarr::ReviewWriteRepository {
public:
  explicit FakeReviewWriteRepository(
      crimson::zarr::ReviewWriteOperation &received)
      : received_(received) {}

  crimson::zarr::ReviewWriteResult response;

  crimson::zarr::ReviewWriteResult
  write(const crimson::zarr::ReviewWriteOperation &operation) override {
    received_ = operation;
    return response;
  }

private:
  crimson::zarr::ReviewWriteOperation &received_;
};

bool testValidationAndOpenFailure() {
  using namespace crimson::zarr;
  ReviewWriteOperation operation;
  CHECK(ExecuteReviewWrite({}, "archive.zarr", operation).error ==
        "No review-write repository factory.");
  CHECK(ExecuteReviewWrite({}, "", operation).error ==
        "No loaded Zarr archive.");

  auto failed = ExecuteReviewWrite(
      [](const std::string &,
         std::string &error) -> std::unique_ptr<ReviewWriteRepository> {
        error = "fixture unavailable";
        return nullptr;
      },
      "archive.zarr", operation);
  CHECK(!failed.ok);
  CHECK(failed.error == "fixture unavailable");
  return true;
}

bool testExactOperationAndResult() {
  using namespace crimson::zarr;
  ReviewWriteOperation received;
  std::string opened_path;
  ReviewWriteRepositoryFactory factory = [&](const std::string &archive_path,
                                             std::string &error) {
    opened_path = archive_path;
    error.clear();
    auto repository = std::make_unique<FakeReviewWriteRepository>(received);
    repository->response.ok = true;
    repository->response.edit_result.changed = true;
    repository->response.edit_result.cache_update.valid = true;
    repository->response.edit_result.cache_update.roi_index = 31;
    return repository;
  };

  ReviewWriteOperation operation;
  operation.kind = ReviewWriteOperationKind::ManualKeypointCorrection;
  operation.selection.valid = true;
  operation.selection.frame_id = 44;
  operation.selection.detection_index = 2;
  operation.selection.roi_index = 31;
  operation.selection.run_name = "refined_run";
  operation.keypoints_roi = {{{1.25, 2.5}}, {{3.75, 4.0}}};

  const ReviewWriteResult result =
      ExecuteReviewWrite(factory, "fixture.zarr", operation);
  CHECK(result.ok);
  CHECK(result.edit_result.changed);
  CHECK(result.edit_result.cache_update.valid);
  CHECK(result.edit_result.cache_update.roi_index == 31);
  CHECK(opened_path == "fixture.zarr");
  CHECK(received.kind == ReviewWriteOperationKind::ManualKeypointCorrection);
  CHECK(received.selection.frame_id == 44);
  CHECK(received.selection.detection_index == 2);
  CHECK(received.selection.roi_index == 31);
  CHECK(received.selection.run_name == "refined_run");
  CHECK(received.keypoints_roi == operation.keypoints_roi);
  return true;
}

} // namespace

int main() {
  if (!testValidationAndOpenFailure() || !testExactOperationAndResult()) {
    return 1;
  }
  std::cout << "review_write_repository_tests: PASS\n";
  return 0;
}
