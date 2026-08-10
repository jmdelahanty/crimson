#pragma once

enum class AppleAnalysisProductAdoptionDecision {
  Adopt,
  DeferSwimBoutsUntilMotionSettles,
};

bool ShouldRequestInitialAppleAnalysisFrame(bool video_smoke,
                                            bool ui_reference_enabled,
                                            bool loader_running);

AppleAnalysisProductAdoptionDecision
DecideAppleAnalysisProductAdoption(bool has_swim_bout_result,
                                   bool motion_available, bool motion_failed);
