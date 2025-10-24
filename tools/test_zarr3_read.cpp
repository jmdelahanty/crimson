#include <tensorstore/tensorstore.h>
#include <tensorstore/open.h>
#include <nlohmann/json.hpp>
#include <iostream>

int main(int argc, char** argv) {
  if (argc < 2) { std::cerr << "Usage: " << argv[0] << " <archive.zarr>\n"; return 1; }
  auto ctx = tensorstore::Context::Default();

  // Helper: open & print shape
  auto probe = [&](const std::string& rel, const std::string& dtype) {
    nlohmann::json spec = {
      {"driver","zarr3"},
      {"kvstore", {{"driver","file"},{"path",argv[1]}}},
      {"path", rel}
    };
    auto open = tensorstore::Open(spec, tensorstore::OpenMode::open,
                                  tensorstore::ReadWriteMode::read, ctx).result();
    if (!open.ok()) { std::cerr << rel << " ❌  " << open.status() << "\n"; return; }
    auto dom = open->domain();
    std::cout << rel << " ✅  dtype=" << dtype << " shape=[";
    for (size_t i=0;i<dom.rank();++i){ if(i)std::cout<<","; std::cout<<dom.shape()[i]; }
    std::cout << "]\n";
  };

  probe("detect_runs/detect_2025-10-22_15-19-14/frame_indices","int32");
  probe("detect_runs/detect_2025-10-22_15-19-14/bbox_norm_coords","float64");
}
