#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

namespace orion
{

/**
    A small fixed pool of realtime worker threads used to render independent
    per-track units (hosted VST instruments) of a single audio block in parallel.

    This is the Ableton model rather than the FL one: we do not try to detect and
    suspend idle plugins, we simply spread the per-track work across the cores
    that are already sitting idle. A block that used to run 40 plugins serially on
    one core now runs them on all performance cores.

    Usage is deliberately narrow. runParallel() blocks until every job has
    finished, the calling (audio) thread takes jobs alongside the workers, and no
    memory is allocated once setNumThreads() has run. Jobs must be independent:
    each one owns its own scratch buffer and touches no shared state.
*/
class AudioRenderPool
{
public:
    /** Called with a job index in [0, numJobs). Must be realtime-safe. */
    using Job = std::function<void (int)>;

    AudioRenderPool();
    ~AudioRenderPool();

    /** Threads to run jobs on, counting the calling thread. <= 1 disables the
        pool and makes runParallel() a plain serial loop. Message thread only,
        while audio is stopped or prepared.
    */
    void setNumThreads (int totalThreads);

    /** Block timing, used to request an appropriate realtime workgroup priority. */
    void setBlockTiming (double sampleRate, int blockSize);

    int getNumThreads() const noexcept { return 1 + static_cast<int> (workers.size()); }

    /** Total threads to use: one per performance core, clamped to a sane range. */
    static int recommendedThreadCount();

    /** Runs job(0)..job(numJobs-1) across the pool and returns once all are done.
        The calling thread participates. Falls back to a serial loop when the pool
        is disabled or there is less than one job per thread to gain from.
        Audio thread; blocking; no allocation.
    */
    void runParallel (int numJobs, const Job& job);

private:
    class Worker;
    friend class Worker;

    /** Claims and runs jobs until the queue is drained. Workers and caller both call this. */
    void drainJobs();

    std::vector<std::unique_ptr<Worker>> workers;

    std::atomic<const Job*> currentJob { nullptr };
    std::atomic<int> numJobsTotal { 0 };
    std::atomic<int> nextJobIndex { 0 };
    std::atomic<int> jobsRemaining { 0 };

    double preparedSampleRate { 44100.0 };
    int preparedBlockSize { 512 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioRenderPool)
};

} // namespace orion
