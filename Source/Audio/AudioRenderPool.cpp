#include "AudioRenderPool.h"

#include <thread>

#if JUCE_MAC || JUCE_IOS
 #include <sys/sysctl.h>
#endif

#if defined (__aarch64__) || defined (__arm64__)
 #define ORION_SPIN_HINT() asm volatile ("yield")
#elif defined (__x86_64__) || defined (__i386__)
 #include <immintrin.h>
 #define ORION_SPIN_HINT() _mm_pause()
#else
 #define ORION_SPIN_HINT() ((void) 0)
#endif

namespace orion
{

// A worker parks on its event between blocks and drains the shared job queue
// when woken. It never touches the pool's state other than through drainJobs().
class AudioRenderPool::Worker final : public juce::Thread
{
public:
    Worker (AudioRenderPool& p, int index)
        : juce::Thread ("Orion render " + juce::String (index)), pool (p)
    {
    }

    ~Worker() override
    {
        signalThreadShouldExit();
        wakeUp.signal();
        stopThread (2000);
    }

    void run() override
    {
        while (! threadShouldExit())
        {
            wakeUp.wait (-1);

            if (threadShouldExit())
                return;

            pool.drainJobs();
        }
    }

    juce::WaitableEvent wakeUp { false };   // auto-reset

private:
    AudioRenderPool& pool;
};

AudioRenderPool::AudioRenderPool() = default;
AudioRenderPool::~AudioRenderPool() = default;

int AudioRenderPool::recommendedThreadCount()
{
    int cores = juce::SystemStats::getNumPhysicalCpus();

#if JUCE_MAC || JUCE_IOS
    // Apple silicon: only the performance cores are worth rendering on. The
    // efficiency cores are several times slower and would stall every block at
    // the join, since the caller waits for the slowest job.
    int perf = 0;
    std::size_t size = sizeof (perf);
    if (sysctlbyname ("hw.perflevel0.logicalcpu", &perf, &size, nullptr, 0) == 0 && perf > 0)
        cores = perf;
#endif

    // Total threads, i.e. the audio thread plus (cores - 1) workers.
    return juce::jlimit (1, 8, cores);
}

void AudioRenderPool::setNumThreads (int totalThreads)
{
    const auto wanted = juce::jmax (0, totalThreads - 1);   // minus the calling thread

    if (wanted == static_cast<int> (workers.size()))
        return;

    workers.clear();   // joins the old threads

    for (int i = 0; i < wanted; ++i)
    {
        auto w = std::make_unique<Worker> (*this, i);

        // Realtime priority with the audio block's period: a worker that misses
        // the deadline glitches the whole mix, exactly like the audio thread.
        const auto options = juce::Thread::RealtimeOptions{}
                                 .withPriority (10)
                                 .withApproximateAudioProcessingTime (preparedBlockSize, preparedSampleRate);

        if (! w->startRealtimeThread (options))
            w->startThread (juce::Thread::Priority::highest);

        workers.push_back (std::move (w));
    }
}

void AudioRenderPool::setBlockTiming (double sampleRate, int blockSize)
{
    if (sampleRate > 0.0)  preparedSampleRate = sampleRate;
    if (blockSize > 0)     preparedBlockSize = blockSize;
}

void AudioRenderPool::drainJobs()
{
    const auto* job = currentJob.load (std::memory_order_acquire);

    if (job == nullptr)
        return;

    for (;;)
    {
        const auto index = nextJobIndex.fetch_add (1, std::memory_order_relaxed);

        // Re-read the total on every claim rather than caching it: a worker that
        // wakes late may be looking at the *next* block's queue, and the total is
        // the only thing keeping its index in range.
        if (index >= numJobsTotal.load (std::memory_order_acquire))
            return;

        (*job) (index);

        jobsRemaining.fetch_sub (1, std::memory_order_release);
    }
}

void AudioRenderPool::runParallel (int numJobs, const Job& job)
{
    if (numJobs <= 0)
        return;

    // One job, or no workers: the serial path, byte-identical to not having a pool.
    if (numJobs < 2 || workers.empty())
    {
        for (int i = 0; i < numJobs; ++i)
            job (i);

        return;
    }

    jobsRemaining.store (numJobs, std::memory_order_relaxed);
    nextJobIndex.store (0, std::memory_order_relaxed);
    numJobsTotal.store (numJobs, std::memory_order_relaxed);
    currentJob.store (&job, std::memory_order_release);

    for (auto& w : workers)
        w->wakeUp.signal();

    // The caller is a worker too, and usually finishes most of the queue itself.
    drainJobs();

    // Join. Spin first (the remaining jobs are microseconds away), then yield so
    // a preempted worker on a busy machine can actually get its core back.
    for (int spins = 0; jobsRemaining.load (std::memory_order_acquire) > 0; ++spins)
    {
        if (spins < 2000)
            ORION_SPIN_HINT();
        else
            std::this_thread::yield();
    }

    currentJob.store (nullptr, std::memory_order_release);
}

} // namespace orion
