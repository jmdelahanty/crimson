#include "gui/diagnostics_window.h"
#include "imgui.h"
#include "imgui_semantic_snapshot.h"

#include <iostream>
#include <stdexcept>

namespace {
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

struct Fixture {
    DiagnosticsRuntimeStatus status;
    crimson::ui::SemanticSnapshot snapshot;
    Fixture() {
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1000, 800);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels;
        int width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        crimson::ui::setSemanticCaptureEnabled(ImGui::GetCurrentContext(), true);
        status.swap_interval = 1;
        status.worker_errors.emplace_back("test-camera", "fixture error");
        status.app_update.install_metadata_found = true;
        status.app_update.status_detail = "share unavailable";
    }
    ~Fixture() {
        crimson::ui::setSemanticCaptureEnabled(ImGui::GetCurrentContext(), false);
        ImGui::DestroyContext();
    }
    DiagnosticsWindowResult draw() {
        crimson::ui::beginSemanticFrame(ImGui::GetCurrentContext());
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(640, 450), ImGuiCond_Always);
        const auto result = drawDiagnosticsWindow(nullptr, status);
        ImGui::Render();
        snapshot = crimson::ui::finishSemanticFrame(ImGui::GetCurrentContext());
        return result;
    }
    DiagnosticsWindowResult click(const char* label) {
        for (const auto& item : snapshot.items) {
            if (item.label != label) continue;
            auto& io = ImGui::GetIO();
            io.AddMousePosEvent(item.bounds.x + item.bounds.width * 0.5f,
                                item.bounds.y + item.bounds.height * 0.5f);
            draw();
            io.AddMouseButtonEvent(0, true);
            draw();
            io.AddMouseButtonEvent(0, false);
            return draw();
        }
        throw std::runtime_error(std::string("Control missing without Frame Inspect: ") + label);
    }
};
}

int main() {
    try {
        Fixture fixture;
        fixture.draw();
        fixture.draw();
        require(!fixture.snapshot.windows.empty(), "Diagnostics is visible without a recording");
        for (const auto& item : fixture.snapshot.items) {
            require(item.label != "Dump Decode Buffers", "no frame controls without context");
        }
        require(fixture.click("VSync").requested_swap_interval == 0, "VSync can be disabled");
        fixture.status.swap_interval = 0;
        fixture.draw();
        require(fixture.click("VSync").requested_swap_interval == 1, "VSync can be enabled");
        fixture.status.swap_interval = 1;
        fixture.draw(); // Apply the requested state, as the application does.
        require(fixture.click("Clear Worker Errors").request_clear_worker_errors,
                "worker error clear remains accessible");
        require(fixture.click("Refresh Update Check").request_refresh_update_check,
                "update check remains accessible");
        std::cout << "Diagnostics controls without Frame Inspect: PASS\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
