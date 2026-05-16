#include "TransportController.h"

namespace orion
{
TransportController::TransportController(ProjectState& projectState, TransportEngine& transportEngine)
    : project(projectState),
      transport(transportEngine)
{
}

void TransportController::togglePlayback(bool withCountIn,
                                         VoidCallback stopPreview,
                                         VoidCallback preparePlayback,
                                         VoidCallback syncPlayback)
{
    if (stopPreview != nullptr)
        stopPreview();

    if (! transport.isPlaying() && ! transport.isCountInActive())
    {
        if (preparePlayback != nullptr)
            preparePlayback();
        if (syncPlayback != nullptr)
            syncPlayback();
    }

    transport.togglePlayback(withCountIn);
}

void TransportController::stop(VoidCallback stopPreview, VoidCallback syncPlayback)
{
    if (stopPreview != nullptr)
        stopPreview();

    transport.stop();
    transport.setPlayheadBeat(0.0);

    if (syncPlayback != nullptr)
        syncPlayback();
}

void TransportController::rewind(VoidCallback stopPreview, VoidCallback syncPlayback)
{
    if (stopPreview != nullptr)
        stopPreview();

    transport.rewindToStart();
    transport.setPlayheadBeat(0.0);

    if (syncPlayback != nullptr)
        syncPlayback();
}

void TransportController::setLoopEnabled(bool shouldEnableLoop,
                                         const TimelineClip* selectedClip,
                                         StatusCallback setStatus)
{
    if (shouldEnableLoop && selectedClip != nullptr)
    {
        project.setLoopRange(selectedClip->startBeat, selectedClip->startBeat + selectedClip->lengthInBeats);
        if (setStatus != nullptr)
            setStatus("Loop: " + selectedClip->name);
    }

    transport.setLoopEnabled(shouldEnableLoop);
}

void TransportController::setTempoBpm(double tempoBpm) noexcept
{
    project.setTempoBpm(tempoBpm);
}

void TransportController::setRecordArmed(bool shouldArm) noexcept
{
    transport.setRecordArmed(shouldArm);
}
}  // namespace orion
