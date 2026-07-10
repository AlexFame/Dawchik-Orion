// Headless exercise for orion::AudioRenderPool: correctness under repetition
// (every job runs exactly once, no lost or duplicated indices), real concurrency,
// and the speedup we actually get on this machine's performance cores.
//
//   ./OrionRenderPoolTest

#include "../Audio/AudioRenderPool.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace
{

int failures = 0;

void check (bool ok, const char* what)
{
    std::printf ("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (! ok)
        ++failures;
}

// Stands in for a plugin's processBlock: burns a predictable amount of CPU and
// returns a value the optimiser can't discard.
double burn (double milliseconds)
{
    const auto until = std::chrono::steady_clock::now()
                     + std::chrono::microseconds (static_cast<long long> (milliseconds * 1000.0));
    double acc = 0.0;

    for (int i = 1; std::chrono::steady_clock::now() < until; ++i)
        acc += std::sin (static_cast<double> (i)) / static_cast<double> (i);

    return acc;
}

double elapsedMs (std::chrono::steady_clock::time_point from)
{
    return std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - from).count();
}

// ---------------------------------------------------------------------------

void testEveryJobRunsExactlyOnce (orion::AudioRenderPool& pool)
{
    std::printf ("\nEvery job runs exactly once (2000 blocks, varying job counts)\n");

    bool allOk = true;
    int  totalJobs = 0;

    for (int block = 0; block < 2000 && allOk; ++block)
    {
        const int numJobs = 1 + (block % 64);
        std::vector<std::atomic<int>> runCount (static_cast<std::size_t> (numJobs));

        for (auto& c : runCount)
            c.store (0);

        pool.runParallel (numJobs, [&runCount] (int i)
        {
            runCount[static_cast<std::size_t> (i)].fetch_add (1);
        });

        // runParallel must not return before every job has finished.
        for (int i = 0; i < numJobs; ++i)
            if (runCount[static_cast<std::size_t> (i)].load() != 1)
                allOk = false;

        totalJobs += numJobs;
    }

    std::printf ("  ran %d jobs across 2000 blocks\n", totalJobs);
    check (allOk, "each index seen exactly once, and all complete before runParallel returns");
}

void testJobsActuallyRunInParallel (orion::AudioRenderPool& pool)
{
    std::printf ("\nJobs really do land on different threads\n");

    std::mutex m;
    std::set<std::thread::id> threads;
    std::atomic<int> concurrent { 0 };
    std::atomic<int> peakConcurrent { 0 };

    pool.runParallel (pool.getNumThreads() * 4, [&] (int)
    {
        const auto now = concurrent.fetch_add (1) + 1;

        int peak = peakConcurrent.load();
        while (now > peak && ! peakConcurrent.compare_exchange_weak (peak, now)) {}

        {
            const std::lock_guard<std::mutex> lock (m);
            threads.insert (std::this_thread::get_id());
        }

        burn (2.0);
        concurrent.fetch_sub (1);
    });

    std::printf ("  distinct threads: %d, peak jobs in flight: %d, pool size: %d\n",
                 static_cast<int> (threads.size()), peakConcurrent.load(), pool.getNumThreads());

    check (static_cast<int> (threads.size()) == pool.getNumThreads(),
           "all pool threads (workers + caller) took work");
    check (peakConcurrent.load() > 1, "jobs overlapped in time");
}

void testCallerParticipates (orion::AudioRenderPool& pool)
{
    std::printf ("\nThe calling thread takes jobs too (it must not just block)\n");

    const auto callerId = std::this_thread::get_id();
    std::atomic<bool> callerRanAJob { false };

    for (int block = 0; block < 20; ++block)
        pool.runParallel (pool.getNumThreads() * 4, [&] (int)
        {
            if (std::this_thread::get_id() == callerId)
                callerRanAJob.store (true);

            burn (0.2);
        });

    check (callerRanAJob.load(), "caller executed at least one job");
}

void testDegenerateCases (orion::AudioRenderPool& pool)
{
    std::printf ("\nDegenerate job counts fall back to the serial path\n");

    std::atomic<int> calls { 0 };

    pool.runParallel (0, [&] (int) { calls.fetch_add (1); });
    check (calls.load() == 0, "zero jobs runs nothing");

    const auto callerId = std::this_thread::get_id();
    std::atomic<bool> onCaller { false };
    pool.runParallel (1, [&] (int i)
    {
        calls.fetch_add (1);
        onCaller.store (std::this_thread::get_id() == callerId && i == 0);
    });
    check (calls.load() == 1 && onCaller.load(), "one job runs inline on the calling thread");
}

void testResizeIsSafe (orion::AudioRenderPool& pool)
{
    std::printf ("\nResizing the pool between blocks\n");

    bool ok = true;

    for (int threads : { 1, 2, 6, 3, 1, orion::AudioRenderPool::recommendedThreadCount() })
    {
        pool.setNumThreads (threads);

        std::atomic<int> ran { 0 };
        pool.runParallel (32, [&] (int) { ran.fetch_add (1); });

        if (ran.load() != 32 || pool.getNumThreads() != std::max (1, threads))
            ok = false;
    }

    check (ok, "every size still runs all 32 jobs");
}

void testSpeedup (orion::AudioRenderPool& pool)
{
    std::printf ("\nSpeedup on a synthetic 40-instrument block\n");

    constexpr int numInstruments = 40;
    constexpr double msPerInstrument = 0.25;   // 40 x 0.25ms = 10ms of work, ~a 512-frame block
    constexpr int blocks = 20;

    std::atomic<double> sink { 0.0 };
    const auto job = [&] (int) { sink.fetch_add (burn (msPerInstrument)); };

    orion::AudioRenderPool serial;
    serial.setNumThreads (1);

    auto start = std::chrono::steady_clock::now();
    for (int b = 0; b < blocks; ++b)
        serial.runParallel (numInstruments, job);
    const auto serialMs = elapsedMs (start) / blocks;

    start = std::chrono::steady_clock::now();
    for (int b = 0; b < blocks; ++b)
        pool.runParallel (numInstruments, job);
    const auto parallelMs = elapsedMs (start) / blocks;

    const auto speedup = serialMs / parallelMs;
    const auto threads = pool.getNumThreads();

    std::printf ("  serial:   %6.2f ms/block (1 thread)\n", serialMs);
    std::printf ("  parallel: %6.2f ms/block (%d threads)\n", parallelMs, threads);
    std::printf ("  speedup:  %6.2fx  (efficiency %.0f%% of %d cores)\n",
                 speedup, 100.0 * speedup / threads, threads);

    // Loose bound: synchronisation and OS scheduling eat some of the ideal.
    check (speedup > static_cast<double> (threads) * 0.6,
           "speedup is at least 60% of the ideal linear scaling");
}

void testOverheadOnATinyBlock (orion::AudioRenderPool& pool)
{
    std::printf ("\nSynchronisation overhead per block (the small-buffer risk)\n");

    constexpr int blocks = 2000;
    std::atomic<int> sink { 0 };

    const auto start = std::chrono::steady_clock::now();
    for (int b = 0; b < blocks; ++b)
        pool.runParallel (pool.getNumThreads(), [&] (int) { sink.fetch_add (1); });

    const auto overheadUs = 1000.0 * elapsedMs (start) / blocks;

    // A 64-frame block at 48kHz is 1333us; anything near that is a dropout.
    std::printf ("  %.1f us per runParallel() with no real work\n", overheadUs);
    std::printf ("  (64-frame block @48k = 1333us budget, 512-frame = 10667us)\n");

    check (overheadUs < 200.0, "dispatch+join overhead stays well under a 64-frame budget");
}

} // namespace

int main()
{
    std::printf ("AudioRenderPool test\n");
    std::printf ("  recommendedThreadCount() = %d\n", orion::AudioRenderPool::recommendedThreadCount());

    orion::AudioRenderPool pool;
    pool.setBlockTiming (48000.0, 512);
    pool.setNumThreads (orion::AudioRenderPool::recommendedThreadCount());

    std::printf ("  pool size = %d (audio thread + %d workers)\n",
                 pool.getNumThreads(), pool.getNumThreads() - 1);

    testEveryJobRunsExactlyOnce (pool);
    testJobsActuallyRunInParallel (pool);
    testCallerParticipates (pool);
    testDegenerateCases (pool);
    testSpeedup (pool);
    testOverheadOnATinyBlock (pool);
    testResizeIsSafe (pool);   // last: it leaves the pool resized

    std::printf ("\n%s (%d failure%s)\n",
                 failures == 0 ? "ALL PASS" : "FAILURES",
                 failures, failures == 1 ? "" : "s");

    return failures == 0 ? 0 : 1;
}
