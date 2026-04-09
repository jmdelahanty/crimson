#include "gui/review_metadata_editor.h"

#include "imgui.h"

#include <cstdlib>
#include <cstring>
#include <pwd.h>
#include <string>
#include <unistd.h>

namespace {

constexpr const char* kReviewUseItems[] = {"full_recording", "training"};
constexpr const char* kReviewStateItems[] = {
    "approved", "needs_review", "pending", "rejected"};
constexpr const char* kReviewMethodItems[] = {
    "manual", "algorithmic", "hybrid", "spotcheck"};

std::string makeLabel(const char* visible_label, const char* id_suffix) {
    std::string label = visible_label;
    label += "##";
    label += id_suffix;
    return label;
}

std::string resolveDefaultReviewer() {
    const char* env_user = std::getenv("USER");
    if (env_user != nullptr && env_user[0] != '\0') {
        return env_user;
    }

    const char* env_logname = std::getenv("LOGNAME");
    if (env_logname != nullptr && env_logname[0] != '\0') {
        return env_logname;
    }

    if (passwd* pw = getpwuid(getuid())) {
        if (pw->pw_name != nullptr && pw->pw_name[0] != '\0') {
            return pw->pw_name;
        }
    }

    return {};
}

}  // namespace

void drawReviewMetadataEditor(const char* id_suffix,
                              ReviewMetadataEditorState& state) {
    if (!state.reviewer_initialized) {
        const std::string default_reviewer = resolveDefaultReviewer();
        if (!default_reviewer.empty()) {
            std::snprintf(state.reviewer.data(),
                          state.reviewer.size(),
                          "%s",
                          default_reviewer.c_str());
        }
        state.reviewer_initialized = true;
    }

    const std::string intended_use_label =
        makeLabel("Intended Use", id_suffix);
    ImGui::Combo(intended_use_label.c_str(),
                 &state.intended_use,
                 kReviewUseItems,
                 IM_ARRAYSIZE(kReviewUseItems));

    const std::string review_state_label =
        makeLabel("Review State", id_suffix);
    ImGui::Combo(review_state_label.c_str(),
                 &state.review_state,
                 kReviewStateItems,
                 IM_ARRAYSIZE(kReviewStateItems));

    const std::string method_label = makeLabel("Review Method", id_suffix);
    ImGui::Combo(method_label.c_str(),
                 &state.method,
                 kReviewMethodItems,
                 IM_ARRAYSIZE(kReviewMethodItems));

    const std::string reviewer_label = makeLabel("Reviewer", id_suffix);
    ImGui::InputText(reviewer_label.c_str(),
                     state.reviewer.data(),
                     state.reviewer.size());

    const std::string notes_label = makeLabel("Notes", id_suffix);
    ImGui::InputText(notes_label.c_str(),
                     state.notes.data(),
                     state.notes.size());
}

ReviewMetadataValues resolveReviewMetadataValues(
    const ReviewMetadataEditorState& state) {
    return ReviewMetadataValues{
        kReviewUseItems[state.intended_use],
        kReviewStateItems[state.review_state],
        kReviewMethodItems[state.method],
        state.reviewer.data(),
        state.notes.data(),
    };
}
