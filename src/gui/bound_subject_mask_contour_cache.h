#pragma once

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace crimson::gui {

struct BoundSubjectMaskContourCache {
  std::string run;
  std::string digest;
};

// This is a bounded candidate search, not cache admission. The repository
// validates the complete manifest, source receipt and Zarr declarations.
inline std::optional<BoundSubjectMaskContourCache> findBoundSubjectMaskContourCache(
    const std::filesystem::path& archive_path, const std::string& source_run,
    const std::string& source_digest, std::string* diagnostic) {
  const auto group = archive_path / "subject_mask_cache_runs";
  std::error_code error;
  std::filesystem::directory_iterator iterator(group, error);
  if (error) {
    if (diagnostic) *diagnostic =
        "Sampled contour cache group is unavailable: " + error.message();
    return std::nullopt;
  }
  const std::filesystem::directory_iterator end;
  std::optional<BoundSubjectMaskContourCache> match;
  while (iterator != end) {
    const auto entry = *iterator;
    iterator.increment(error);
    if (error) {
      if (diagnostic) *diagnostic =
          "Sampled contour cache inventory is unreadable: " + error.message();
      return std::nullopt;
    }
    const bool symlink = entry.is_symlink(error);
    if (error) {
      if (diagnostic) *diagnostic = "Sampled contour cache inventory is unreadable";
      return std::nullopt;
    }
    if (symlink) continue;
    const bool directory = entry.is_directory(error);
    if (error) {
      if (diagnostic) *diagnostic = "Sampled contour cache inventory is unreadable";
      return std::nullopt;
    }
    if (!directory) continue; // The group has its own zarr.json.
    const auto run = entry.path().filename().string();
    if (run.empty() || run == "." || run == "..") continue;
    const auto metadata_path = entry.path() / "zarr.json";
    const auto bytes = std::filesystem::file_size(metadata_path, error);
    if (error || bytes > 16ULL * 1024ULL * 1024ULL) {
      error.clear();
      continue;
    }
    std::ifstream stream(metadata_path);
    if (!stream) continue;
    try {
      const auto metadata = nlohmann::json::parse(stream);
      const auto& manifest = metadata.at("attributes").at("run_manifest");
      const auto& payload = manifest.at("payload");
      const auto& source = payload.at("source_refined_subject_mask_snapshot");
      if (payload.at("run_id") != run || source.at("run_name") != source_run ||
          source.at("manifest_payload_digest") != source_digest) {
        continue;
      }
      const auto cache_digest = manifest.at("payload_digest").get<std::string>();
      if (cache_digest.size() != 64) {
        if (diagnostic) *diagnostic = "Bound sampled contour cache digest is invalid";
        return std::nullopt;
      }
      if (match) {
        if (diagnostic) *diagnostic =
            "More than one sampled contour cache matches the bound mask";
        return std::nullopt;
      }
      match = BoundSubjectMaskContourCache{run, cache_digest};
    } catch (const std::exception&) {
      continue;
    }
  }
  if (!match && diagnostic) {
    *diagnostic = "No sampled contour cache matches the bound mask run and digest";
  }
  return match;
}

} // namespace crimson::gui
