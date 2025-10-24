#include <iostream>
#include <tensorstore/tensorstore.h>   // Spec, Context
#include <tensorstore/open.h>          // Open(...)
#include <nlohmann/json.hpp>           // json

int main() {
  auto ctx = tensorstore::Context::Default();

  // Create a tiny Zarr v3 array entirely in-memory.
  // Note: v3 array metadata requires shape, data_type, chunk_grid,
  // chunk_key_encoding, codecs, and fill_value.
  const char* kSpec = R"json(
    {
      "driver": "zarr3",
      "kvstore": {"driver": "memory"},
      "metadata": {
        "shape": [1],
        "data_type": "uint8",
        "chunk_grid": {
          "name": "regular",
          "configuration": { "chunk_shape": [1] }
        },
        "chunk_key_encoding": { "name": "default" },
        "codecs": [ { "name": "bytes" } ],
        "fill_value": 0
      },
      "create": true,
      "open": true
    }
  )json";

  ::nlohmann::json j = ::nlohmann::json::parse(kSpec);

  auto spec_result = tensorstore::Spec::FromJson(j);
  if (!spec_result.ok()) {
    std::cerr << "Spec parse failed: " << spec_result.status() << "\n";
    return 2;
  }

  auto open_result = tensorstore::Open(*spec_result, ctx).result();
  if (!open_result.ok()) {
    std::cerr << "Open failed: " << open_result.status() << "\n";
    return 3;
  }

  std::cout << "Zarr3: OK\n";
  return 0;
}
