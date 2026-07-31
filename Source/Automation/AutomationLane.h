#pragma once

#include <juce_core/juce_core.h>

#include <vector>

// ============================================================================================
// Source/Automation — track/parameter automation (Bitwig / Ableton model).
//
// This module owns everything about automation: the envelope data model (this file), value
// resolution, and — in later phases — recording modes (Read/Write/Touch/Latch/Trim) and Bitwig-style
// modulators (LFO / envelope followers mapped to any parameter). It's deliberately standalone so the
// engine (Audio/PlaybackSources) and the UI (ArrangementTimeline) both depend on it, not each other.
//
// Values are stored in each parameter's NATURAL unit (dB for volume, -1..+1 for pan) so playback
// applies them directly; the UI maps that onto a 0..1 lane via the param's min/max. Each point carries
// a per-segment `curve` (tension, -1..+1; 0 = linear) like Bitwig/Ableton curved segments.
// ============================================================================================

namespace orion
{
// The KIND of parameter a lane drives. The model covers everything up-front so nothing needs a
// reshape later — any track type (MIDI / audio / VST) and any parameter is addressable:
//   trackVolume / trackPan   → the track fader / pan (no indices)
//   trackSend                → send slot `targetIndex`
//   instrumentParam          → hosted VST instrument, plugin parameter `paramIndex`
//   insertParam              → insert FX at `targetIndex`, plugin parameter `paramIndex`
// Playback currently applies volume/pan; plugin-param + send application land in the next phase, but
// the data model, serialization and UI target model are already shaped for them.
enum class AutomationParam
{
    trackVolume,       // dB
    trackPan,          // -1..+1
    trackSend,         // 0..1 (send level), targetIndex = send slot
    instrumentParam,   // 0..1 normalised, paramIndex = plugin param
    insertParam        // 0..1 normalised, targetIndex = insert index, paramIndex = plugin param
};

// Volume/pan use natural units; everything else is a normalised 0..1 parameter value.
inline float automationParamMin(AutomationParam p) noexcept
{
    return p == AutomationParam::trackVolume ? -60.0f : (p == AutomationParam::trackPan ? -1.0f : 0.0f);
}
inline float automationParamMax(AutomationParam p) noexcept
{
    return p == AutomationParam::trackVolume ?   6.0f : 1.0f;
}
inline float automationParamDefault(AutomationParam p) noexcept
{
    return p == AutomationParam::trackVolume ? 0.0f : (p == AutomationParam::trackSend ? 0.0f : (p == AutomationParam::trackPan ? 0.0f : 0.5f));
}
inline juce::String automationParamName(AutomationParam p)
{
    switch (p)
    {
        case AutomationParam::trackVolume:     return "Volume";
        case AutomationParam::trackPan:        return "Pan";
        case AutomationParam::trackSend:       return "Send";
        case AutomationParam::instrumentParam: return "Instrument";
        case AutomationParam::insertParam:     return "Insert";
    }
    return "Param";
}

struct AutomationPoint
{
    double beat  { 0.0 };
    float  value { 0.0f };   // natural unit for the lane's param
    float  curve { 0.0f };   // segment tension to the NEXT point: -1..+1, 0 = linear
};

struct AutomationLane
{
    AutomationParam param { AutomationParam::trackVolume };
    // Which slot the param lives on: send slot / insert index (-1 = the track itself / not applicable).
    int targetIndex { -1 };
    // Which parameter inside a hosted plugin (for instrumentParam / insertParam); -1 = not applicable.
    int paramIndex { -1 };
    bool enabled { true };
    std::vector<AutomationPoint> points;   // kept sorted by beat

    bool empty() const noexcept { return points.empty(); }
    bool active() const noexcept { return enabled && ! points.empty(); }

    // Insert a point, keeping the list sorted by beat. Replaces an existing point at (nearly) the
    // same beat so a drag doesn't pile up duplicates. Returns the index.
    int addPoint(double beat, float value, float curve = 0.0f)
    {
        for (std::size_t i = 0; i < points.size(); ++i)
        {
            if (std::abs(points[i].beat - beat) < 1.0e-6)
            {
                points[i].value = value;
                points[i].curve = curve;
                return static_cast<int>(i);
            }
            if (points[i].beat > beat)
            {
                points.insert(points.begin() + static_cast<long>(i), { beat, value, curve });
                return static_cast<int>(i);
            }
        }
        points.push_back({ beat, value, curve });
        return static_cast<int>(points.size()) - 1;
    }

    // Value of the envelope at `beat`. Flat outside the first/last points; `fallback` when empty.
    float valueAt(double beat, float fallback) const
    {
        if (points.empty())
            return fallback;
        if (beat <= points.front().beat)
            return points.front().value;
        if (beat >= points.back().beat)
            return points.back().value;

        for (std::size_t i = 1; i < points.size(); ++i)
        {
            if (beat <= points[i].beat)
            {
                const auto& a = points[i - 1];
                const auto& b = points[i];
                const double span = b.beat - a.beat;
                float t = span > 1.0e-9 ? static_cast<float>((beat - a.beat) / span) : 0.0f;
                t = shapeTension(t, a.curve);
                return a.value + (b.value - a.value) * t;
            }
        }
        return points.back().value;
    }

    // Curved segment: tension>0 eases in (slow start), <0 eases out. Matches Bitwig/Ableton feel.
    static float shapeTension(float t, float tension) noexcept
    {
        if (std::abs(tension) < 1.0e-4f)
            return t;
        const float p = std::pow(2.0f, tension * 3.0f);   // t^p, p>1 ease-in, p<1 ease-out
        return std::pow(juce::jlimit(0.0f, 1.0f, t), p);
    }

    juce::var toVar() const
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty("param", static_cast<int>(param));
        obj->setProperty("targetIndex", targetIndex);
        obj->setProperty("paramIndex", paramIndex);
        obj->setProperty("enabled", enabled);
        juce::Array<juce::var> pts;
        for (const auto& pt : points)
        {
            auto* po = new juce::DynamicObject();
            po->setProperty("b", pt.beat);
            po->setProperty("v", pt.value);
            po->setProperty("c", pt.curve);
            pts.add(juce::var(po));
        }
        obj->setProperty("points", pts);
        return juce::var(obj);
    }

    static AutomationLane fromVar(const juce::var& v)
    {
        AutomationLane lane;
        lane.param = static_cast<AutomationParam>(static_cast<int>(v.getProperty("param", 0)));
        lane.targetIndex = static_cast<int>(v.getProperty("targetIndex", -1));
        lane.paramIndex = static_cast<int>(v.getProperty("paramIndex", -1));
        lane.enabled = static_cast<bool>(v.getProperty("enabled", true));
        if (const auto* arr = v.getProperty("points", juce::var()).getArray())
            for (const auto& pv : *arr)
                lane.points.push_back({ static_cast<double>(pv.getProperty("b", 0.0)),
                                        static_cast<float>(pv.getProperty("v", 0.0)),
                                        static_cast<float>(pv.getProperty("c", 0.0)) });
        return lane;
    }
};
} // namespace orion
