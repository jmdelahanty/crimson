// minimal_zarr_lister.cpp
#include <iostream>
#include <string>
#include <vector>
#include <tensorstore/kvstore/kvstore.h>
#include <tensorstore/kvstore/operations.h>
#include <nlohmann/json.hpp>

namespace ts = tensorstore;

// Function to list the contents of a given prefix (directory) in the Zarr store
void list_directory_contents(const ts::kvstore::KvStore& store, const std::string& prefix) {
    std::cout << "\n--- Listing contents of: '" << (prefix.empty() ? "/" : prefix) << "' ---" << std::endl;
    try {
        ts::kvstore::ListOptions options;
        if (!prefix.empty()) {
            options.range = ts::KeyRange::Prefix(prefix);
        }

        auto list_result = ts::kvstore::ListFuture(store, options).result();

        if (!list_result.ok()) {
            std::cerr << "  Error listing directory: " << list_result.status() << std::endl;
            return;
        }

        const auto& entries = list_result.value();
        if (entries.empty()) {
            std::cout << "  Directory appears to be empty or does not exist according to TensorStore." << std::endl;
        } else {
            std::cout << "  Found " << entries.size() << " entries:" << std::endl;
            for (const auto& entry : entries) {
                std::cout << "    - " << entry.key << std::endl;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "  An exception occurred: " << e.what() << std::endl;
    }
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <path_to_zarr_file>" << std::endl;
        return 1;
    }

    std::string zarr_path = argv[1];
    std::cout << "Attempting to open Zarr store: " << zarr_path << std::endl;

    // Create a TensorStore context
    auto context = ts::Context::Default();

    // Specify the key-value store driver (file-based)
    nlohmann::json kvstore_spec = {
        {"driver", "file"},
        {"path", zarr_path}
    };

    // Open the store
    auto store_result = ts::kvstore::Open(kvstore_spec, context).result();
    if (!store_result.ok()) {
        std::cerr << "Failed to open Zarr store: " << store_result.status() << std::endl;
        return 1;
    }
    
    auto store = store_result.value();
    std::cout << "Zarr store opened successfully." << std::endl;

    // --- Perform the diagnostic listings ---
    list_directory_contents(store, ""); // Root directory
    list_directory_contents(store, "detection_runs/");
    list_directory_contents(store, "refined_detect_runs/");
    list_directory_contents(store, "analysis/stimulus_runs/");
    list_directory_contents(store, "interpolation_runs/"); // Legacy fallback

    return 0;
}
