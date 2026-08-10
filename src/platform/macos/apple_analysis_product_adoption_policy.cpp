#include "platform/macos/apple_analysis_product_adoption_policy.h"

bool ShouldRequestInitialAppleAnalysisFrame(bool video_smoke,
                                            bool ui_reference_enabled,
                                            bool loader_running) {
  return video_smoke || ui_reference_enabled || !loader_running;
}

AppleAnalysisProductAdoptionDecision
DecideAppleAnalysisProductAdoption(bool has_swim_bout_result,
                                   bool motion_available, bool motion_failed) {
  if (has_swim_bout_result && !motion_available && !motion_failed) {
    return AppleAnalysisProductAdoptionDecision::
        DeferSwimBoutsUntilMotionSettles;
  }
  return AppleAnalysisProductAdoptionDecision::Adopt;
}
