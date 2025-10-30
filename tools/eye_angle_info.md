Here’s how the eye-angle analysis defines each signal and the geometry behind it (all angles in degrees, clipped to 0–90 for magnitudes unless noted):

left_deg, right_deg – Magnitude-only rotation of each eye’s ellipse major axis relative to the head axis.
Major axis comes from the refined eye mask’s fitted ellipse.
Head axis is the unit vector from the swim bladder landmark to the midpoint between the left & right eye landmarks. These values tell you “how far” each eye has rotated, ignoring direction.

left_signed_deg, right_signed_deg – Same angle as above, but signed. The sign is determined by whether the major axis points toward the nasal side (positive) or temporal side (negative). This lets the analysis add/subtract rotations with binocular conventions.

left_minor_signed_deg, right_minor_signed_deg – Signed rotation of the ellipse minor axis. After finding the minor axis (perpendicular to the major axis), the code checks whether it points toward the temporal side (positive) or nasal side (negative). These are useful when the pupil boundary is better captured by the short axis.

left_feret_major_signed_deg, right_feret_major_signed_deg – Same signed-angle calculation, but using the Feret “major” diameter (the longest chord through the binary mask). Feret endpoints give a more robust direction when the ellipse fit is noisy.

left_feret_minor_signed_deg, right_feret_minor_signed_deg – Signed orientation of the Feret “minor” diameter (shortest chord). Again, positive/negative is determined relative to temporal vs nasal directions.

vergence_deg – Binocular convergence magnitude, defined as abs(left_signed_deg + right_signed_deg). Positive values indicate the eyes are rotating inward together.

vergence_signed_deg – Signed convergence (left_signed_deg + right_signed_deg). Positive means both eyes aim nasally (typical convergence), negative means they’re diverging outward.

vergence_minor_signed_deg, vergence_feret_major_signed_deg, vergence_feret_minor_signed_deg – Convergence computed from the corresponding minor-axis or Feret-based angles. You get the same inward/outward sign conventions but grounded in different underlying measurements.

version_deg – Binocular version magnitude, computed as 0.5 * abs(left_signed_deg - right_signed_deg). This captures common-mode eye motion (both eyes sweeping together).

version_minor_deg, version_feret_major_deg, version_feret_minor_deg – Version derived from minor-axis or Feret angles (same formula, different inputs).

vergence_speed_deg_s, version_speed_deg_s, etc. – First derivatives (per-second) of the corresponding angles, computed only when frame timestamps/FPS are available.

vergence_signed_accel_deg_s2, version_accel_deg_s2, etc. – Second derivatives (angular acceleration) based on the signed speeds.

Behind the scenes, the analysis also stores the ellipse lengths (ellipse_major, ellipse_minor) and ratio; the signed angles are only recorded when the ellipse fit converged, had nonzero axes, and wasn’t too circular (ratio below ELLIPSE_CIRCULARITY_THRESHOLD). Each detection carries QA flags so you can filter frames where the geometry was unreliable.

So in short:

“Left/right” angles quantify how far each eye has rotated relative to the head axis.
“Signed” variants tell you inward (+) vs outward (–) rotation.
“Vergence” is the binocular inward/outward sum.
“Version” is the binocular common-mode sweep.
“Minor” and “Feret” variants repeat the same math using alternative axis definitions that can be more stable when the ellipse fit is noisy.