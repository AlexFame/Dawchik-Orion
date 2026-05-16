#pragma once

#include <functional>

#include "TransportEngine.h"
#include "../Core/ProjectState.h"

namespace orion
{
class TransportController final
{
public:
    using VoidCallback = std::function<void()>;
    using StatusCallback = std::function<void(const juce::String&)>;

    TransportController(ProjectState& projectState, TransportEngine& transportEngine);

    void togglePlayback(bool withCountIn,
                        VoidCallback stopPreview,
                        VoidCallback preparePlayback,
                        VoidCallback syncPlayback);
    void stop(VoidCallback stopPreview, VoidCallback syncPlayback);
    void rewind(VoidCallback stopPreview, VoidCallback syncPlayback);
    void setLoopEnabled(bool shouldEnableLoop,
                        const TimelineClip* selectedClip,
                        StatusCallback setStatus);
    void setTempoBpm(double tempoBpm) noexcept;
    void setRecordArmed(bool shouldArm) noexcept;

private:
    ProjectState& project;
    TransportEngine& transport;
};
}  // namespace orion
