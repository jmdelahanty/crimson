#include <tensorstore/context.h>
#include <tensorstore/index_space/dim_expression.h>
#include <tensorstore/open.h>
#include <tensorstore/tensorstore.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <sys/resource.h>
#include <vector>

namespace ts = tensorstore;
using json = nlohmann::json;

namespace {

struct IoCounters {
    uint64_t rchar = 0;
    uint64_t syscr = 0;
    uint64_t read_bytes = 0;
};

IoCounters readIoCounters() {
    IoCounters result;
    std::ifstream stream("/proc/self/io");
    std::string key;
    uint64_t value = 0;
    while (stream >> key >> value) {
        if (key == "rchar:") result.rchar = value;
        if (key == "syscr:") result.syscr = value;
        if (key == "read_bytes:") result.read_bytes = value;
    }
    return result;
}

uint64_t maxRssBytes() {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) return 0;
#if defined(__APPLE__)
    return static_cast<uint64_t>(usage.ru_maxrss);
#else
    return static_cast<uint64_t>(usage.ru_maxrss) * 1024ULL;
#endif
}

double percentile(std::vector<double> values, double quantile) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const double position = quantile * static_cast<double>(values.size() - 1);
    const size_t lower = static_cast<size_t>(std::floor(position));
    const size_t upper = static_cast<size_t>(std::ceil(position));
    const double fraction = position - static_cast<double>(lower);
    return values[lower] * (1.0 - fraction) + values[upper] * fraction;
}

template <typename Array>
double checksumArray(const Array& array) {
    double checksum = 0.0;
    // Reads preserve the requested non-zero row origin.  Iterating from
    // Array::data() would therefore apply the origin offset twice and may
    // point outside the allocation.  TensorStore's iterator starts at the
    // byte-strided origin and also handles non-contiguous layouts.
    ts::IterateOverArrays(
        [&](const double* value) {
            if (std::isfinite(*value)) checksum += *value;
        },
        ts::ContiguousLayoutOrder::c, array);
    return checksum;
}

struct Operation {
    ts::Index start = 0;
    ts::Index rows = 1;
};

std::vector<Operation> makeOperations(const std::string& workload,
                                      ts::Index total_rows,
                                      uint64_t seed) {
    constexpr ts::Index kInnerRows = 1024;
    std::mt19937_64 rng(seed);
    std::vector<Operation> operations;
    if (workload == "random_row") {
        std::uniform_int_distribution<ts::Index> distribution(0, total_rows - 1);
        for (size_t i = 0; i < 256; ++i) {
            operations.push_back({distribution(rng), 1});
        }
    } else if (workload == "random_1024") {
        const ts::Index chunk_count = (total_rows + kInnerRows - 1) / kInnerRows;
        std::uniform_int_distribution<ts::Index> distribution(0, chunk_count - 1);
        for (size_t i = 0; i < 64; ++i) {
            const ts::Index start = distribution(rng) * kInnerRows;
            operations.push_back(
                {start, std::min(kInnerRows, total_rows - start)});
        }
    } else if (workload == "shuffled_repeat") {
        const ts::Index chunk_count = (total_rows + kInnerRows - 1) / kInnerRows;
        const size_t sample_count =
            std::min<size_t>(128, static_cast<size_t>(chunk_count));
        std::vector<ts::Index> chunks(static_cast<size_t>(chunk_count));
        std::iota(chunks.begin(), chunks.end(), ts::Index{0});
        std::shuffle(chunks.begin(), chunks.end(), rng);
        chunks.resize(sample_count);
        for (int repeat = 0; repeat < 2; ++repeat) {
            std::shuffle(chunks.begin(), chunks.end(), rng);
            for (ts::Index chunk : chunks) {
                const ts::Index start = chunk * kInnerRows;
                operations.push_back(
                    {start, std::min(kInnerRows, total_rows - start)});
            }
        }
    } else if (workload == "sequential_scan") {
        for (ts::Index start = 0; start < total_rows; start += kInnerRows) {
            operations.push_back(
                {start, std::min(kInnerRows, total_rows - start)});
        }
    } else {
        throw std::runtime_error("unknown workload: " + workload);
    }
    return operations;
}

json runPass(const ts::TensorStore<double, 3>& array,
             const std::vector<Operation>& operations,
             const std::string& phase) {
    std::vector<double> latencies_ms;
    latencies_ms.reserve(operations.size());
    const IoCounters io_before = readIoCounters();
    const auto wall_started = std::chrono::steady_clock::now();
    double checksum = 0.0;
    uint64_t logical_bytes = 0;
    size_t operation_index = 0;
    for (const Operation& operation : operations) {
        if (operation_index == 0) {
            std::cerr << "[benchmark] first operation start=" << operation.start
                      << " rows=" << operation.rows << std::endl;
        }
        const auto started = std::chrono::steady_clock::now();
        auto slice = array | ts::Dims(0).HalfOpenInterval(
                                 operation.start,
                                 operation.start + operation.rows);
        auto read = ts::Read(slice).result();
        if (!read.ok()) {
            throw std::runtime_error(read.status().ToString());
        }
        if (operation_index == 0) std::cerr << "[benchmark] first read complete" << std::endl;
        checksum += checksumArray(read.value());
        if (operation_index == 0) std::cerr << "[benchmark] first checksum complete" << std::endl;
        const auto stopped = std::chrono::steady_clock::now();
        latencies_ms.push_back(
            std::chrono::duration<double, std::milli>(stopped - started).count());
        logical_bytes += static_cast<uint64_t>(operation.rows) * 5ULL * 2ULL *
                         sizeof(double);
        ++operation_index;
    }
    const auto wall_stopped = std::chrono::steady_clock::now();
    const IoCounters io_after = readIoCounters();
    return json{
        {"phase", phase},
        {"operations", operations.size()},
        {"logical_bytes", logical_bytes},
        {"wall_seconds",
         std::chrono::duration<double>(wall_stopped - wall_started).count()},
        {"latency_p50_ms", percentile(latencies_ms, 0.50)},
        {"latency_p95_ms", percentile(latencies_ms, 0.95)},
        {"latency_p99_ms", percentile(latencies_ms, 0.99)},
        {"latency_max_ms",
         latencies_ms.empty()
             ? 0.0
             : *std::max_element(latencies_ms.begin(), latencies_ms.end())},
        {"rchar_delta", io_after.rchar - io_before.rchar},
        {"syscr_delta", io_after.syscr - io_before.syscr},
        {"read_bytes_delta", io_after.read_bytes - io_before.read_bytes},
        {"max_rss_bytes", maxRssBytes()},
        {"checksum", checksum}};
}

void usage(const char* argv0) {
    std::cerr << "Usage: " << argv0
              << " ARRAY_PATH LAYOUT WORKLOAD REPETITION [SEED]\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 5 || argc > 6) {
        usage(argv[0]);
        return 2;
    }
    const std::string array_path = argv[1];
    const std::string layout = argv[2];
    const std::string workload = argv[3];
    const int repetition = std::stoi(argv[4]);
    const uint64_t seed = argc == 6 ? std::stoull(argv[5]) : 20260714ULL;

    std::cerr << "[benchmark] opening " << array_path << std::endl;

    json spec = {
        {"driver", "zarr3"},
        {"kvstore", {{"driver", "file"}, {"path", array_path + "/"}}}};
    const ts::Context context = ts::Context::Default();
    auto opened = ts::Open<double, 3>(spec, ts::OpenMode::open,
                                      ts::ReadWriteMode::read, context)
                      .result();
    if (!opened.ok()) {
        std::cerr << "Open failed: " << opened.status() << std::endl;
        return 3;
    }
    std::cerr << "[benchmark] opened" << std::endl;
    // IndexDomain::shape() is a view into the domain object.  Keep the domain
    // alive instead of retaining a dangling view from a temporary.
    const auto domain = opened->domain();
    const auto shape = domain.shape();
    if (shape[0] <= 0 || shape[1] != 5 || shape[2] != 2) {
        std::cerr << "Unexpected keypoints_img shape: [" << shape[0] << ", "
                  << shape[1] << ", " << shape[2] << "]" << std::endl;
        return 4;
    }

    try {
        const auto operations = makeOperations(workload, shape[0], seed);
        std::cerr << "[benchmark] operations=" << operations.size() << std::endl;
        json result = {
            {"schema", "crimson.tensorstore_keypoint_read_benchmark.v1"},
            {"array_path", array_path},
            {"layout", layout},
            {"workload", workload},
            {"repetition", repetition},
            {"seed", seed},
            {"rows", shape[0]},
            {"inner_chunk_rows", 1024},
            {"passes", json::array()}};
        std::cerr << "[benchmark] fresh_process" << std::endl;
        result["passes"].push_back(runPass(opened.value(), operations, "fresh_process"));
        std::cerr << "[benchmark] warm_repeat" << std::endl;
        result["passes"].push_back(runPass(opened.value(), operations, "warm_repeat"));
        std::cout << "RESULT_JSON=" << result.dump() << std::endl;
    } catch (const std::exception& error) {
        std::cerr << "Benchmark failed: " << error.what() << std::endl;
        return 5;
    }
    return 0;
}
