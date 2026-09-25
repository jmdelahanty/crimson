#pragma once

#include <array>
#include <string>

struct ReviewMetadataEditorState {
    int intended_use = 0;
    int review_state = 0;
    int method = 0;
    std::array<char, 64> reviewer{};
    std::array<char, 256> notes{};
    bool reviewer_initialized = false;
};

struct ReviewMetadataValues {
    std::string intended_use;
    std::string review_state;
    std::string method;
    std::string reviewer;
    std::string notes;
};

void drawReviewMetadataEditor(const char* id_suffix,
                              ReviewMetadataEditorState& state);

ReviewMetadataValues resolveReviewMetadataValues(
    const ReviewMetadataEditorState& state);
