#pragma once

#include "gui/analysis_timeline_window.h"
#include "zarr_loader.h"

#include <cstddef>
#include <string>
#include <vector>

const ZarrDetectionData::MovementSeries* renderMovementDatasetUI(
    ZarrDetectionLoader& zarr_loader,
    const char* combo_label);

std::vector<size_t> findCompatibleSwimBoutIndices(
    const ZarrDetectionData::MovementSeries& movement_series,
    const std::vector<ZarrDetectionData::SwimBoutSeries>& swim_bouts);

std::string swimBoutCandidateLabel(
    const ZarrDetectionData::SwimBoutSeries& bouts);

const ZarrDetectionData::SwimBoutSeries* resolveSelectedSwimBoutSeries(
    const std::vector<ZarrDetectionData::SwimBoutSeries>& swim_bouts,
    const std::vector<size_t>& compatible_indices,
    AnalysisTimelineWindowState& state);

std::vector<size_t> findCompatibleBoutKinematicsIndices(
    const ZarrDetectionData::MovementSeries& movement_series,
    const ZarrDetectionData::SwimBoutSeries* swim_bouts,
    const std::vector<ZarrDetectionData::BoutKinematicsSeries>& bout_kinematics);

const ZarrDetectionData::BoutKinematicsSeries*
resolveSelectedBoutKinematicsSeries(
    const std::vector<ZarrDetectionData::BoutKinematicsSeries>& bout_kinematics,
    const std::vector<size_t>& compatible_indices,
    AnalysisTimelineWindowState& state);
