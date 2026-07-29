#pragma once

#include <vector>

namespace orion
{
// Decayed meter levels + peak-hold readouts for the track / aux-bus / master meters.
//
// Pure display state: filled once per UI tick by MainComponent::updateTrackMeterLevels() (the single
// consumer of the audio thread's peaks, so the timeline header meters, the mixer, and the bottom
// MASTER OUT bar all read consistent values) and read back by those views. Extracted verbatim out of
// MainComponent to shrink that god-object — field names are unchanged, so behaviour is identical.
struct MeterState
{
    // Per-track output levels (index = track index).
    std::vector<float> trackMeterLevels;    // combined fast bar level (linear), responsive
    std::vector<float> trackMeterLevelsL;   // per-channel fast bar level (linear) — left
    std::vector<float> trackMeterLevelsR;   // per-channel fast bar level (linear) — right
    std::vector<float> trackPeakHoldDb;     // numeric readout in dB (Logic-style peak hold)
    std::vector<float> trackPeakRecentDb;   // loudest dB seen since the last discrete update
    std::vector<int>   trackPeakHoldFrames; // remaining peak-hold frames before the number snaps

    // Master output metering.
    float masterRawPeakL { 0.0f };          // raw peak this tick (left), for the mixer
    float masterRawPeakR { 0.0f };          // raw peak this tick (right), for the mixer
    float masterMeterLevel { 0.0f };        // decayed 0..1 level for the bottom MASTER OUT bar
    float masterMeterLevelL { 0.0f };       // per-channel decayed linear level for the mixer master bar
    float masterMeterLevelR { 0.0f };
    float masterMeterDb { -100.0f };        // numeric dB readout for the bottom MASTER OUT text
    float masterMeterRecentDb { -100.0f };  // loudest dB since the last discrete update
    int   masterMeterDbHoldFrames { 0 };    // remaining peak-hold frames before it snaps

    // Aux-bus meters (mirror the track meter machinery).
    std::vector<float> busMeterLevelsL;
    std::vector<float> busMeterLevelsR;
    std::vector<float> busPeakHoldDb;
    std::vector<float> busPeakRecentDb;
    std::vector<int>   busPeakHoldFrames;
};
} // namespace orion
