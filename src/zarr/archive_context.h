#pragma once

#include <filesystem>
#include <memory>
#include <string>

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
  std::filesystem::path resolveStoredPath(
      const std::filesystem::path& stored_path) const;

 private:
  explicit ArchiveContext(std::shared_ptr<Impl> impl);

  std::shared_ptr<Impl> impl_;

  friend std::unique_ptr<class StimulusRepository> OpenStimulusRepository(
      const std::shared_ptr<ArchiveContext>& archive,
      const std::string& requested_run,
      std::string* error_message);
};

}  // namespace crimson::zarr
