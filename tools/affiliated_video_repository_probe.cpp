#include "zarr/affiliated_video_repository.h"
#include "zarr/archive_context.h"

#include <iostream>
#include <string>

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "Usage: affiliated_video_repository_probe <archive.zarr>\n";
    return 2;
  }

  std::string error;
  auto archive = crimson::zarr::ArchiveContext::Open(argv[1], &error);
  if (!archive) {
    std::cerr << error << '\n';
    return 1;
  }
  auto descriptor = crimson::zarr::DiscoverAffiliatedVideo(archive, &error);
  if (!descriptor) {
    std::cerr << (error.empty() ? "Archive has no affiliated video metadata"
                                : error)
              << '\n';
    return 1;
  }

  std::cout << "source="
            << crimson::zarr::AffiliatedVideoSourceName(descriptor->source)
            << '\n'
            << "stored=" << descriptor->stored_path.string() << '\n'
            << "resolved=" << descriptor->resolved_path.string() << '\n';
  return 0;
}
