#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>

#include <absl/strings/cord.h>
#include <nlohmann/json.hpp>
#include <tensorstore/array.h>
#include <tensorstore/cast.h>
#include <tensorstore/open.h>
#include <tensorstore/open_mode.h>
#include <tensorstore/spec.h>
#include <tensorstore/tensorstore.h>
#include <tensorstore/kvstore/operations.h>
#include <tensorstore/data_type.h>
#include <tensorstore/context.h>

namespace ts = tensorstore;
using json = nlohmann::json;

namespace {

/**
 * Reads Zarr attributes from a specified group path (v3/v2 compatible).
 */
json ReadAttributes(const ts::kvstore::KvStore& store, const std::string& group_path) {
    auto load_json = [&](const std::string& key) -> std::optional<json> {
        auto result = ts::kvstore::Read(store, key).result();
        if (!result.ok()) {
            // This will catch NOT_FOUND, which is fine, we return nullopt
            return std::nullopt;
        }

        // --- FIX 1 ---
        // The compiler error showed us that result.value() returns the
        // ReadResult struct directly, not an optional.
        const auto& read_result = result.value();

        // The '!result.ok()' check above already handles non-existence.
        // We can now access the 'value' member directly with '.'
        std::string payload;
        absl::CopyCordToString(read_result.value, &payload);
        // --- END FIX 1 ---

        if (payload.empty()) {
            return json::object();
        }
        try {
            return json::parse(payload);
        } catch (const json::parse_error&) {
            return std::nullopt;
        }
    };

    // Ensure path has a trailing slash if it's not the root
    std::string base_path = group_path;
    if (!base_path.empty() && base_path.back() != '/') {
        base_path += "/";
    }
    // Handle case where parent path is "." (current dir), which means root
    if (base_path == "./") {
        base_path = "";
    }

    // Try Zarr v3
    if (auto zarr_json = load_json(base_path + "zarr.json")) {
        if (zarr_json->contains("attributes") && (*zarr_json)["attributes"].is_object()) {
            return (*zarr_json)["attributes"];
        }
        return *zarr_json;
    }

    // Fallback to Zarr v2
    if (auto zattrs = load_json(base_path + ".zattrs")) {
        return *zattrs;
    }

    return json::object();
}

/**
 * Prints a 2D matrix from a dynamic array by accessing raw bytes.
 */
void PrintMatrix(const ts::SharedOffsetArray<void>& array) {
    const auto shape = array.shape();
    if (array.rank() != 2) {
        std::cerr << "Array must be rank 2" << std::endl;
        return;
    }

    std::cout << "Homography matrix (" << shape[0] << " x " << shape[1] << ")\n";
    std::cout << "dtype: " << array.dtype().name() << "\n";
    std::cout << std::fixed << std::setprecision(6);

    auto dtype = array.dtype();
    const auto strides = array.byte_strides();
    const void* base_ptr = array.byte_strided_pointer();

    for (ts::Index i = 0; i < shape[0]; ++i) {
        std::cout << "  ";
        for (ts::Index j = 0; j < shape[1]; ++j) {
            const void* elem_ptr = static_cast<const char*>(base_ptr) +
                                   i * strides[0] + j * strides[1];
            double value = 0.0;
            if (dtype == ts::dtype_v<double>) {
                value = *static_cast<const double*>(elem_ptr);
            } else if (dtype == ts::dtype_v<float>) {
                value = static_cast<double>(*static_cast<const float*>(elem_ptr));
            }
            std::cout << std::setw(12) << value;
            if (j + 1 < shape[1]) {
                std::cout << ' ';
            }
        }
        std::cout << '\n';
    }
    std::cout.flush();
}

/**
 * Opens a TensorStore dataset, reads it, casts to a 2D double array,
 * and prints it.
 */
bool LoadAndPrintMatrix(const ts::Spec& spec) {
    // Open the dataset
    auto open_result = ts::Open(spec, ts::OpenMode::open, ts::ReadWriteMode::read).result();
    if (!open_result.ok()) {
        std::cerr << "Failed to open dataset: " << open_result.status() << std::endl;
        return false;
    }

    // Read the array
    auto read_result = ts::Read(open_result.value()).result();
    if (!read_result.ok()) {
        std::cerr << "Failed to read dataset: " << read_result.status() << std::endl;
        return false;
    }

    auto array = read_result.value();

    // Check rank
    if (array.rank() != 2) {
        std::cerr << "Dataset rank is " << array.rank() << " (expected 2)." << std::endl;
        return false;
    }

    // Print the matrix
    PrintMatrix(array);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::cerr << "Usage: " << argv[0]
                  << " <zarr_root> [dataset_path]\n"
                  << "  zarr_root    Directory containing the Zarr store\n"
                  << "  dataset_path Optional dataset inside the zarr store\n"
                  << "               (default: calibration/homography_matrix)\n";
        return 1;
    }

    const std::string zarr_path = argv[1];
    const std::string full_dataset_path =
        (argc == 3) ? argv[2] : "calibration/homography_matrix";

    // Determine the group path (parent directory) from the full dataset path
    std::string group_path = std::filesystem::path(full_dataset_path).parent_path().string();

    auto kv_spec_result = ts::kvstore::Spec::FromJson({
        {"driver", "file"},
        {"path", zarr_path}
    });
    if (!kv_spec_result.ok()) {
        std::cerr << "Failed to create kvstore spec: "
                  << kv_spec_result.status() << std::endl;
        return 2;
    }

    auto store_result = ts::kvstore::Open(kv_spec_result.value()).result();
    if (!store_result.ok()) {
        std::cerr << "Failed to open kvstore: "
                  << store_result.status() << std::endl;
        return 3;
    }

    // Read attributes from the auto-determined group path
    auto attrs = ReadAttributes(store_result.value(), group_path);
    std::string group_name = group_path.empty() ? "[root]" : group_path;

    if (!attrs.empty()) {
        std::cout << "Attributes for group '" << group_name << "':\n";
        for (auto& [key, value] : attrs.items()) {
            std::cout << "  " << key << ": " << value.dump() << '\n';
        }
    } else {
        std::cout << "Attributes for group '" << group_name << "': (none found)\n";
    }

    // Build the spec for the *full dataset path*
    auto spec_result = ts::Spec::FromJson({
        {"driver", "zarr3"},
        {"kvstore", {{"driver", "file"}, {"path", zarr_path}}},
        {"path", full_dataset_path}
    });
    if (!spec_result.ok()) {
        std::cerr << "Failed to build TensorStore spec: "
                  << spec_result.status() << std::endl;
        return 4;
    }

    if (!LoadAndPrintMatrix(spec_result.value())) {
        return 5;
    }

    return 0;
}