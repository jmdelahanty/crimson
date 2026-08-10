#include "platform/macos/apple_analysis_product_adoption_policy.h"

#include <iostream>

namespace {

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__           \
                << ": " #condition << '\n';                                    \
      return false;                                                            \
    }                                                                          \
  } while (false)

bool testInitialFrameDemandPolicy() {
  CHECK(!ShouldRequestInitialAppleAnalysisFrame(false, false, true));
  CHECK(ShouldRequestInitialAppleAnalysisFrame(false, false, false));
  CHECK(ShouldRequestInitialAppleAnalysisFrame(true, false, true));
  CHECK(ShouldRequestInitialAppleAnalysisFrame(false, true, true));
  return true;
}

bool testSwimBoutDependencyPolicy() {
  using Decision = AppleAnalysisProductAdoptionDecision;
  CHECK(DecideAppleAnalysisProductAdoption(true, false, false) ==
        Decision::DeferSwimBoutsUntilMotionSettles);
  CHECK(DecideAppleAnalysisProductAdoption(true, true, false) ==
        Decision::Adopt);
  CHECK(DecideAppleAnalysisProductAdoption(true, false, true) ==
        Decision::Adopt);
  CHECK(DecideAppleAnalysisProductAdoption(false, false, false) ==
        Decision::Adopt);
  return true;
}

} // namespace

int main() {
  if (!testInitialFrameDemandPolicy() || !testSwimBoutDependencyPolicy()) {
    return 1;
  }
  std::cout << "apple_analysis_product_adoption_policy_tests: PASS\n";
  return 0;
}
