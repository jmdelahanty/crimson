#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;

struct NodeInfo {
  fs::path dir;
  bool has_zjson = false;
  bool has_zattrs = false;
  bool has_zarray = false;

  // For v3 nodes
  std::optional<json> zjson;
  std::string node_type;                   // "group" | "array" | ""
  std::vector<std::string> attr_keys;      // attribute names (sample)
  std::vector<int64_t> shape;              // if array metadata present
  std::string data_type;                   // if array metadata present
  std::string chunk_grid_name;             // if present
};

static std::optional<json> ReadJsonFile(const fs::path& p) {
  std::ifstream in(p);
  if (!in) return std::nullopt;
  try { json j; in >> j; return j; }
  catch (...) { return std::nullopt; }
}

static void SummarizeV3(NodeInfo& n, int max_attr_keys) {
  if (!n.has_zjson || !n.zjson) return;
  const json& J = *n.zjson;

  if (J.contains("node_type") && J["node_type"].is_string()) {
    n.node_type = J["node_type"].get<std::string>();
  }

  if (J.contains("attributes") && J["attributes"].is_object()) {
    for (auto it = J["attributes"].begin(); it != J["attributes"].end(); ++it) {
      n.attr_keys.push_back(it.key());
      if ((int)n.attr_keys.size() >= max_attr_keys) break;
    }
  }

  // Zarr v3 array metadata normally under "metadata"
  const json* meta = nullptr;
  if (J.contains("metadata") && J["metadata"].is_object()) {
    meta = &J["metadata"];
  } else {
    meta = &J;  // fall back: some tools surface fields top-level
  }

  if (meta->contains("shape") && (*meta)["shape"].is_array()) {
    n.shape.clear();
    for (auto& v : (*meta)["shape"]) {
      if (v.is_number_integer()) n.shape.push_back(v.get<int64_t>());
    }
  }
  if (meta->contains("data_type") && (*meta)["data_type"].is_string()) {
    n.data_type = (*meta)["data_type"].get<std::string>();
  }
  if (meta->contains("chunk_grid") && (*meta)["chunk_grid"].is_object()) {
    auto& cg = (*meta)["chunk_grid"];
    if (cg.contains("name") && cg["name"].is_string()) {
      n.chunk_grid_name = cg["name"].get<std::string>();
    }
  }
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <zarr-root> [--show-attrs N] [--json-out PATH]\n";
    return 1;
  }

  fs::path root = fs::path(argv[1]);
  if (!fs::exists(root) || !fs::is_directory(root)) {
    std::cerr << "Not a directory: " << root << "\n";
    return 1;
  }

  int show_attrs = 5;
  std::optional<fs::path> json_out_path;
  for (int i = 2; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--show-attrs" && i + 1 < argc) {
      show_attrs = std::max(0, std::stoi(argv[++i]));
    } else if (arg == "--json-out" && i + 1 < argc) {
      json_out_path = fs::path(argv[++i]);
    }
  }

  std::vector<NodeInfo> v2_nodes;
  std::vector<NodeInfo> v3_nodes;

  // Walk once
  for (auto it = fs::recursive_directory_iterator(root);
       it != fs::recursive_directory_iterator(); ++it) {
    if (!it->is_directory()) continue;
    const fs::path dir = it->path();

    NodeInfo n;
    n.dir        = dir;
    n.has_zjson  = fs::exists(dir / "zarr.json");
    n.has_zattrs = fs::exists(dir / ".zattrs");
    n.has_zarray = fs::exists(dir / ".zarray");

    if (!n.has_zjson && (n.has_zattrs || n.has_zarray)) {
      v2_nodes.push_back(std::move(n));
      continue;
    }
    if (n.has_zjson) {
      n.zjson = ReadJsonFile(dir / "zarr.json");
      if (n.zjson) SummarizeV3(n, show_attrs);
      v3_nodes.push_back(std::move(n));
    }
  }

  // Human-readable sections
  std::cout << "=========================\n";
  std::cout << "PASS 1: V2-style nodes\n";
  std::cout << "=========================\n";
  if (v2_nodes.empty()) {
    std::cout << "(none)\n\n";
  } else {
    for (const auto& n : v2_nodes) {
      std::cout << n.dir.string()
                << "  [.zarray=" << (n.has_zarray ? "yes" : "no")
                << ", .zattrs="  << (n.has_zattrs ? "yes" : "no")
                << ", zarr.json=no]\n";
    }
    std::cout << "\n";
  }

  std::cout << "=========================\n";
  std::cout << "PASS 2: V3 nodes (zarr.json present)\n";
  std::cout << "=========================\n";
  if (v3_nodes.empty()) {
    std::cout << "(none)\n";
  } else {
    for (const auto& n : v3_nodes) {
      std::cout << n.dir.string()
                << "  [node_type=" << (n.node_type.empty() ? "?" : n.node_type)
                << ", attrs_shown=" << n.attr_keys.size() << "]\n";
      if (!n.attr_keys.empty()) {
        std::cout << "  attributes: ";
        for (size_t i = 0; i < n.attr_keys.size(); ++i) {
          if (i) std::cout << ", ";
          std::cout << n.attr_keys[i];
        }
        std::cout << (n.attr_keys.size() == (size_t)show_attrs ? " ..." : "") << "\n";
      }
      if (!n.shape.empty() || !n.data_type.empty() || !n.chunk_grid_name.empty()) {
        std::cout << "  array_meta:";
        if (!n.shape.empty()) {
          std::cout << " shape=[";
          for (size_t i = 0; i < n.shape.size(); ++i) {
            if (i) std::cout << ",";
            std::cout << n.shape[i];
          }
          std::cout << "]";
        }
        if (!n.data_type.empty()) {
          std::cout << " data_type=" << n.data_type;
        }
        if (!n.chunk_grid_name.empty()) {
          std::cout << " chunk_grid=" << n.chunk_grid_name;
        }
        std::cout << "\n";
      }
    }
  }

  // JSON summary
  json j;
  j["root"] = root.string();
  j["totals"] = {
      {"v2_nodes", v2_nodes.size()},
      {"v3_nodes", v3_nodes.size()}
  };

  // Emit compact info for each node; keep it reasonably small but useful
  j["v2_nodes"] = json::array();
  for (const auto& n : v2_nodes) {
    j["v2_nodes"].push_back({
        {"path", n.dir.string()},
        {"has_zarray", n.has_zarray},
        {"has_zattrs", n.has_zattrs}
    });
  }

  j["v3_nodes"] = json::array();
  for (const auto& n : v3_nodes) {
    json entry = {
        {"path", n.dir.string()},
        {"node_type", n.node_type.empty() ? json(nullptr) : json(n.node_type)},
        {"attr_keys_sample", n.attr_keys},
    };
    // Add array metadata if present
    if (!n.shape.empty() || !n.data_type.empty() || !n.chunk_grid_name.empty()) {
      entry["array_meta"] = json::object();
      if (!n.shape.empty()) entry["array_meta"]["shape"] = n.shape;
      if (!n.data_type.empty()) entry["array_meta"]["data_type"] = n.data_type;
      if (!n.chunk_grid_name.empty()) entry["array_meta"]["chunk_grid"] = n.chunk_grid_name;
    }
    j["v3_nodes"].push_back(std::move(entry));
  }

  std::cout << "\n----- JSON Summary (pretty) -----\n";
  std::cout << j.dump(2) << "\n";

  if (json_out_path) {
    std::ofstream out(*json_out_path);
    if (!out) {
      std::cerr << "Failed to write JSON summary to " << *json_out_path << "\n";
      return 2;
    }
    out << j.dump(2) << "\n";
    std::cout << "Wrote JSON summary to: " << *json_out_path << "\n";
  }

  return 0;
}
