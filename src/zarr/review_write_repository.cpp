#include "zarr/review_write_repository.h"

#include <utility>

namespace crimson::zarr {

ReviewWriteResult
ExecuteReviewWrite(const ReviewWriteRepositoryFactory &repository_factory,
                   const std::string &archive_path,
                   const ReviewWriteOperation &operation) {
  ReviewWriteResult result;
  if (archive_path.empty()) {
    result.error = "No loaded Zarr archive.";
    return result;
  }
  if (!repository_factory) {
    result.error = "No review-write repository factory.";
    return result;
  }

  std::string open_error;
  std::unique_ptr<ReviewWriteRepository> repository =
      repository_factory(archive_path, open_error);
  if (!repository) {
    result.error = open_error.empty() ? "Review-write repository open failed."
                                      : std::move(open_error);
    return result;
  }
  return repository->write(operation);
}

} // namespace crimson::zarr
