#include "clipped_media_prewarm_policy.h"

#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

void check(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}

void testLeadWindow() {
  using crimson::playback::leadFrames;
  check(leadFrames(30.0, 1.0) == 90, "normal lead window");
  check(leadFrames(30.0, 0.5) == 45, "slow playback lead window");
  check(leadFrames(24.0, 1.0) == 72, "source frame rate lead window");
  check(leadFrames(120.0, 2.0) == 90, "lead window cap");
  check(leadFrames(0.0, 0.0) == 1, "invalid rate floor");
}

void testContiguousTail() {
  using crimson::playback::hasContiguousTail;
  check(hasContiguousTail(97, 100, {100, 98, 97, 99}),
        "out-of-order resident tail");
  check(!hasContiguousTail(97, 100, {97, 98, 98, 100}),
        "duplicate cannot fill a missing frame");
  check(!hasContiguousTail(97, 100, {97, 98, 99}),
        "last old frame must be resident");
  check(hasContiguousTail(100, 100, {99, 100}),
        "single-frame tail");
  check(!hasContiguousTail(-1, 100, {100}), "negative tail start");
  check(!hasContiguousTail(101, 100, {100, 101}),
        "reversed tail range");
}

void testAdoptionIdentity() {
  using crimson::playback::canAdopt;
  check(canAdopt(54000, 0, 54000, 0, true, false),
        "matching first candidate frame");
  check(!canAdopt(54000, 0, 54000, 0, false, false),
        "old producer must be joined");
  check(!canAdopt(54000, 0, 54000, 0, true, true),
        "failed candidate cannot be adopted");
  check(!canAdopt(54000, 0, 54001, 0, true, false),
        "wrong parent identity");
  check(!canAdopt(54000, 0, 54000, 1, true, false),
        "wrong local identity");
  check(!canAdopt(-1, 0, -1, 0, true, false),
        "invalid boundary identity");
}

void testStagingAdmission() {
  using crimson::playback::allowedStageBytes;
  check(allowedStageBytes(8, 16, 32, 16), "bounded staging admitted");
  check(!allowedStageBytes(0, 16, 32, 16), "zero allocation rejected");
  check(!allowedStageBytes(17, 16, 64, 16), "budget cap enforced");
  check(!allowedStageBytes(16, 32, 31, 16), "headroom enforced");
  check(!allowedStageBytes(1, 16, 16, 16), "no free headroom");
}

} // namespace

int main() {
  try {
    testLeadWindow();
    testContiguousTail();
    testAdoptionIdentity();
    testStagingAdmission();
    std::cout << "clipped_media_prewarm_policy_tests: PASS\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "clipped_media_prewarm_policy_tests: FAIL: "
              << e.what() << '\n';
    return 1;
  }
}
