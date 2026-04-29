#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct SubjectMaskWritebackRequest {
    std::string zarr_path;
    std::string refined_run;
    std::string component_name;
    int32_t roi_index = -1;
    size_t rows = 0;
    size_t cols = 0;
    std::vector<uint8_t> binary_mask;
    std::string reason = "crimson_refined_subject_mask_edit";
    bool validate = true;
    std::string source_subject_mask_run;
};

struct SubjectMaskWritebackResult {
    bool ok = false;
    bool preview_only = false;
    std::string status;
    std::string message;
    int32_t roi_index = -1;
    std::string component_name;
    int64_t row_revision_before = -1;
    int64_t row_revision_after = -1;
    bool edit_applied = false;
    bool mask_changed = false;
    int64_t contour_points = -1;
    std::string updated_at_utc;
    std::string raw_json;
};

class SubjectMaskWritebackClient {
public:
    virtual ~SubjectMaskWritebackClient() = default;
    virtual const char* backendName() const = 0;
    virtual SubjectMaskWritebackResult save(
        const SubjectMaskWritebackRequest& request) const = 0;
};

class PreviewOnlyWritebackClient final : public SubjectMaskWritebackClient {
public:
    const char* backendName() const override { return "PreviewOnly"; }
    SubjectMaskWritebackResult save(
        const SubjectMaskWritebackRequest& request) const override;
};

class PaletteCommandWritebackClient final : public SubjectMaskWritebackClient {
public:
    explicit PaletteCommandWritebackClient(std::string command);

    static PaletteCommandWritebackClient fromEnvironment();

    const char* backendName() const override { return "PaletteCommand"; }
    bool configured() const { return !command_.empty(); }
    const std::string& command() const { return command_; }

    SubjectMaskWritebackResult save(
        const SubjectMaskWritebackRequest& request) const override;

private:
    std::string command_;
};

class PaletteServiceWritebackClient final : public SubjectMaskWritebackClient {
public:
    const char* backendName() const override { return "PaletteService"; }
    SubjectMaskWritebackResult save(
        const SubjectMaskWritebackRequest& request) const override;
};
