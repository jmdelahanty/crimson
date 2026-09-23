#include "zarr/subject_mask_bundle_v2_contract.h"

#include <filesystem>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

namespace {

struct Options {
  std::filesystem::path store;
  std::string bundle;
  std::string digest;
};

bool ParseOptions(int argc, char **argv, Options *options) {
  if (!options) {
    return false;
  }
  for (int index = 1; index < argc; ++index) {
    if (index + 1 >= argc) {
      return false;
    }
    const std::string argument = argv[index];
    const std::string value = argv[++index];
    if (argument == "--store") {
      options->store = value;
    } else if (argument == "--bundle") {
      options->bundle = value;
    } else if (argument == "--manifest-payload-digest") {
      options->digest = value;
    } else {
      return false;
    }
  }
  return !options->store.empty() && !options->bundle.empty() &&
         !options->digest.empty();
}

} // namespace

int main(int argc, char **argv) {
  Options options;
  if (!ParseOptions(argc, argv, &options)) {
    std::cerr << "Usage: subject_mask_bundle_v2_probe --store PATH "
                 "--bundle RUN --manifest-payload-digest SHA256\n";
    return 2;
  }

  crimson::zarr::SubjectMaskBundleV2Summary summary;
  std::string error;
  const bool valid = crimson::zarr::ValidateSubjectMaskBundleV2Archive(
      options.store, options.bundle, options.digest, &summary, &error);
  nlohmann::json result = {
      {"schema_id", "crimson.subject_mask.bundle_v2_probe"},
      {"schema_version", 1},
      {"status", valid ? "pass" : "fail"},
      {"store", options.store.string()},
      {"bundle_id", summary.bundle_id},
      {"manifest_payload_digest", summary.payload_digest},
      {"recording_identity", summary.recording_identity},
      {"members",
       {{"raw", summary.raw_run},
        {"refined", summary.refined_run},
        {"quality", summary.quality_run}}},
      {"frame_count", summary.frame_count},
      {"row_count", summary.row_count},
      {"raw_channel_count", summary.raw_channel_count},
      {"refined_channel_count", summary.refined_channel_count},
      {"mask_shape", {summary.mask_height, summary.mask_width}},
      {"raw_component_labels", summary.raw_component_labels},
      {"refined_component_labels", summary.refined_component_labels},
      {"validated_array_declarations", summary.validated_array_declarations},
      {"selector_eligible", summary.selector_eligible},
      {"activation_deferred", summary.activation_deferred},
      {"quality_payload_opened", summary.quality_payload_opened},
      {"error", error},
  };
  std::cout << result.dump(2) << '\n';
  return valid ? 0 : 1;
}
