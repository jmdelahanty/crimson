#include "subject_mask_writeback_client.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

using json = nlohmann::json;

SubjectMaskWritebackResult makeRejectedResult(
    const SubjectMaskWritebackRequest& request,
    std::string status,
    std::string message) {
    SubjectMaskWritebackResult result;
    result.ok = false;
    result.status = std::move(status);
    result.message = std::move(message);
    result.roi_index = request.roi_index;
    result.component_name = request.component_name;
    return result;
}

bool validateRequestShape(const SubjectMaskWritebackRequest& request,
                          std::string& error) {
    if (request.zarr_path.empty()) {
        error = "No Zarr archive path provided.";
        return false;
    }
    if (request.refined_run.empty()) {
        error = "No refined subject-mask run provided.";
        return false;
    }
    if (request.component_name.empty()) {
        error = "No subject-mask component provided.";
        return false;
    }
    if (request.roi_index < 0) {
        error = "No valid ROI row provided.";
        return false;
    }
    if (request.rows == 0 || request.cols == 0) {
        error = "Mask shape is empty.";
        return false;
    }
    const size_t expected_size = request.rows * request.cols;
    if (request.binary_mask.size() != expected_size) {
        std::ostringstream oss;
        oss << "Mask payload size mismatch: expected " << expected_size
            << " bytes, got " << request.binary_mask.size() << ".";
        error = oss.str();
        return false;
    }
    return true;
}

std::vector<uint8_t> binarizedMask(const std::vector<uint8_t>& mask) {
    std::vector<uint8_t> out(mask.size(), 0);
    std::transform(mask.begin(), mask.end(), out.begin(),
                   [](uint8_t value) -> uint8_t {
                       return value != 0 ? 1 : 0;
                   });
    return out;
}

std::string maskShapeArgument(size_t rows, size_t cols) {
    return std::to_string(rows) + "x" + std::to_string(cols);
}

#ifndef _WIN32
struct TempMaskFile {
    std::filesystem::path path;

    ~TempMaskFile() {
        if (!path.empty()) {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    }
};

bool writeTempRawMask(const std::vector<uint8_t>& mask,
                      TempMaskFile& file,
                      std::string& error) {
    std::string pattern =
        (std::filesystem::temp_directory_path() /
         "crimson-subject-mask-XXXXXX.bin")
            .string();
    std::vector<char> path_buffer(pattern.begin(), pattern.end());
    path_buffer.push_back('\0');

    constexpr int kSuffixLength = 4;
    const int fd = mkstemps(path_buffer.data(), kSuffixLength);
    if (fd < 0) {
        error = "Failed to create temporary mask payload: " +
                std::string(std::strerror(errno));
        return false;
    }

    size_t written = 0;
    while (written < mask.size()) {
        const ssize_t rc =
            write(fd, mask.data() + written, mask.size() - written);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            error = "Failed to write temporary mask payload: " +
                    std::string(std::strerror(errno));
            close(fd);
            std::filesystem::remove(path_buffer.data());
            return false;
        }
        if (rc == 0) {
            error = "Failed to write temporary mask payload: write returned zero bytes.";
            close(fd);
            std::filesystem::remove(path_buffer.data());
            return false;
        }
        written += static_cast<size_t>(rc);
    }

    if (close(fd) != 0) {
        error = "Failed to close temporary mask payload: " +
                std::string(std::strerror(errno));
        std::filesystem::remove(path_buffer.data());
        return false;
    }

    file.path = path_buffer.data();
    return true;
}

bool runProcessCapture(const std::vector<std::string>& argv,
                       int& exit_code,
                       std::string& output,
                       std::string& error) {
    if (argv.empty() || argv.front().empty()) {
        error = "No Palette writeback command configured.";
        return false;
    }

    int pipe_fds[2] = {-1, -1};
    if (pipe(pipe_fds) != 0) {
        error = "Failed to create command output pipe: " +
                std::string(std::strerror(errno));
        return false;
    }

    const pid_t pid = fork();
    if (pid < 0) {
        error = "Failed to fork Palette writeback command: " +
                std::string(std::strerror(errno));
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return false;
    }

    if (pid == 0) {
        close(pipe_fds[0]);
        dup2(pipe_fds[1], STDOUT_FILENO);
        dup2(pipe_fds[1], STDERR_FILENO);
        close(pipe_fds[1]);

        std::vector<char*> child_argv;
        child_argv.reserve(argv.size() + 1);
        for (const auto& item : argv) {
            child_argv.push_back(const_cast<char*>(item.c_str()));
        }
        child_argv.push_back(nullptr);
        execvp(child_argv[0], child_argv.data());
        _exit(127);
    }

    close(pipe_fds[1]);
    char buffer[4096];
    for (;;) {
        const ssize_t count = read(pipe_fds[0], buffer, sizeof(buffer));
        if (count > 0) {
            output.append(buffer, static_cast<size_t>(count));
            continue;
        }
        if (count == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        error = "Failed while reading Palette command output: " +
                std::string(std::strerror(errno));
        close(pipe_fds[0]);
        return false;
    }
    close(pipe_fds[0]);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        error = "Failed waiting for Palette writeback command: " +
                std::string(std::strerror(errno));
        return false;
    }

    if (WIFEXITED(status)) {
        exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        exit_code = 128 + WTERMSIG(status);
    } else {
        exit_code = 1;
    }
    return true;
}
#endif

SubjectMaskWritebackResult resultFromJson(
    const SubjectMaskWritebackRequest& request,
    const std::string& payload_text) {
    SubjectMaskWritebackResult result;
    result.roi_index = request.roi_index;
    result.component_name = request.component_name;
    result.raw_json = payload_text;

    const auto payload = json::parse(payload_text);
    result.ok = payload.value("ok", false);
    result.status = payload.value("status", std::string());
    result.message = result.status;
    result.roi_index = payload.value("roi_index", request.roi_index);
    result.component_name =
        payload.value("component_name", request.component_name);
    result.row_revision_before =
        payload.value("row_revision_before", static_cast<int64_t>(-1));
    result.row_revision_after =
        payload.value("row_revision_after", static_cast<int64_t>(-1));
    result.edit_applied = payload.value("edit_applied", false);
    result.mask_changed = payload.value("mask_changed", false);
    result.contour_points =
        payload.value("contour_points", static_cast<int64_t>(-1));
    result.updated_at_utc =
        payload.value("updated_at_utc", std::string());
    return result;
}

}  // namespace

SubjectMaskWritebackResult PreviewOnlyWritebackClient::save(
    const SubjectMaskWritebackRequest& request) const {
    SubjectMaskWritebackResult result =
        makeRejectedResult(request,
                           "preview_only",
                           "Subject-mask save is disabled; current backend is preview-only.");
    result.preview_only = true;
    return result;
}

PaletteCommandWritebackClient::PaletteCommandWritebackClient(std::string command)
    : command_(std::move(command)) {}

PaletteCommandWritebackClient PaletteCommandWritebackClient::fromEnvironment() {
    const char* env_value = std::getenv("PALETTE_WRITEBACK_CMD");
    return PaletteCommandWritebackClient(env_value == nullptr
                                             ? std::string()
                                             : std::string(env_value));
}

SubjectMaskWritebackResult PaletteCommandWritebackClient::save(
    const SubjectMaskWritebackRequest& request) const {
    std::string validation_error;
    if (!validateRequestShape(request, validation_error)) {
        return makeRejectedResult(request, "invalid_request", validation_error);
    }
    if (command_.empty()) {
        return makeRejectedResult(
            request,
            "not_configured",
            "PALETTE_WRITEBACK_CMD is not configured for subject-mask saves.");
    }

#ifdef _WIN32
    return makeRejectedResult(
        request,
        "unsupported",
        "Palette command writeback is not implemented on Windows.");
#else
    TempMaskFile mask_file;
    const std::vector<uint8_t> mask = binarizedMask(request.binary_mask);
    std::string temp_error;
    if (!writeTempRawMask(mask, mask_file, temp_error)) {
        return makeRejectedResult(request, "payload_error", temp_error);
    }

    std::vector<std::string> argv = {
        command_,
        "--zarr-path",
        request.zarr_path,
        "--refined-run",
        request.refined_run,
        "--component-name",
        request.component_name,
        "--roi-index",
        std::to_string(request.roi_index),
        "--mask-path",
        mask_file.path.string(),
        "--mask-shape",
        maskShapeArgument(request.rows, request.cols),
        "--reason",
        request.reason.empty() ? "crimson_refined_subject_mask_edit"
                               : request.reason,
    };
    if (!request.source_subject_mask_run.empty()) {
        argv.push_back("--source-subject-mask-run");
        argv.push_back(request.source_subject_mask_run);
    }
    if (request.validate) {
        argv.push_back("--validate");
    }

    int exit_code = 1;
    std::string output;
    std::string process_error;
    if (!runProcessCapture(argv, exit_code, output, process_error)) {
        return makeRejectedResult(request, "command_error", process_error);
    }
    if (exit_code != 0) {
        std::ostringstream oss;
        oss << "Palette writeback command failed with exit code "
            << exit_code;
        if (!output.empty()) {
            oss << ": " << output;
        }
        return makeRejectedResult(request, "command_failed", oss.str());
    }

    try {
        SubjectMaskWritebackResult result = resultFromJson(request, output);
        if (result.message.empty()) {
            result.message = result.ok ? "Palette writeback completed."
                                       : "Palette writeback failed.";
        }
        return result;
    } catch (const std::exception& exc) {
        SubjectMaskWritebackResult result =
            makeRejectedResult(request,
                               "invalid_response",
                               std::string("Palette writeback returned non-JSON output: ") +
                                   exc.what());
        result.raw_json = output;
        return result;
    }
#endif
}

SubjectMaskWritebackResult PaletteServiceWritebackClient::save(
    const SubjectMaskWritebackRequest& request) const {
    return makeRejectedResult(
        request,
        "not_implemented",
        "Palette service writeback backend is reserved for a future endpoint.");
}
