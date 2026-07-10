#include "StemSeparator.h"

#include <juce_core/juce_core.h>

#include <array>
#include <cmath>
#include <cstdlib>
#include <map>

namespace orion::stems
{
    namespace
    {
        juce::File locateBin()
        {
            if (auto* env = std::getenv ("ORION_STEMSPLIT_BIN"))
            {
                juce::File f (juce::String::fromUTF8 (env));
                if (f.existsAsFile()) return f;
            }
            const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
            const juce::StringArray rels {
                "../Resources/stemsplit/bin/orion-stemsplit",   // inside app bundle (Contents/MacOS → Contents/Resources)
                "stemsplit/bin/orion-stemsplit",
                "Resources/stemsplit/bin/orion-stemsplit",
            };
            for (auto& r : rels)
            {
                auto f = exe.getParentDirectory().getChildFile (r);
                if (f.existsAsFile()) return f;
            }
            return {};
        }

        juce::File locateModel (const juce::File& bin)
        {
            if (auto* env = std::getenv ("ORION_STEMSPLIT_MODEL"))
            {
                juce::File f (juce::String::fromUTF8 (env));
                if (f.existsAsFile()) return f;
            }
            // …/stemsplit/bin/orion-stemsplit → …/stemsplit/models/htdemucs-6s.bin
            if (bin.existsAsFile())
            {
                auto f = bin.getParentDirectory().getParentDirectory().getChildFile ("models/htdemucs-6s.bin");
                if (f.existsAsFile()) return f;
            }
            return {};
        }

        // Decode `input` (any supported format) to a clean 44.1 kHz stereo 16-bit WAV — the rate/format
        // Demucs expects — sidestepping the helper's stricter WAV loader. Returns the output duration in
        // seconds, or <= 0 on failure.
        double writeCleanWav (const juce::File& input, const juce::File& dest)
        {
            juce::AudioFormatManager fm;
            fm.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (input));
            if (reader == nullptr || reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0)
                return 0.0;

            const int srcLen = static_cast<int> (juce::jmin<juce::int64> (reader->lengthInSamples, (juce::int64) reader->sampleRate * 600));
            const int srcCh  = juce::jmax (1, static_cast<int> (reader->numChannels));
            juce::AudioBuffer<float> src (srcCh, srcLen);
            reader->read (&src, 0, srcLen, 0, true, true);

            // Fold to stereo.
            juce::AudioBuffer<float> stereo (2, srcLen);
            if (srcCh >= 2)
            {
                stereo.copyFrom (0, 0, src, 0, 0, srcLen);
                stereo.copyFrom (1, 0, src, 1, 0, srcLen);
            }
            else
            {
                stereo.copyFrom (0, 0, src, 0, 0, srcLen);
                stereo.copyFrom (1, 0, src, 0, 0, srcLen);
            }

            constexpr double targetRate = 44100.0;
            juce::AudioBuffer<float>* toWrite = &stereo;
            juce::AudioBuffer<float> resampled;
            if (std::abs (reader->sampleRate - targetRate) > 1.0)
            {
                const double ratio = reader->sampleRate / targetRate;   // input samples per output sample
                const int outLen = juce::jmax (1, static_cast<int> (std::round (srcLen / ratio)));
                resampled.setSize (2, outLen);
                for (int ch = 0; ch < 2; ++ch)
                {
                    juce::LagrangeInterpolator interp;
                    interp.process (ratio, stereo.getReadPointer (ch), resampled.getWritePointer (ch), outLen);
                }
                toWrite = &resampled;
            }

            dest.deleteFile();
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::FileOutputStream> os (dest.createOutputStream());
            if (os == nullptr)
                return 0.0;
            std::unique_ptr<juce::AudioFormatWriter> writer (
                wav.createWriterFor (os.get(), targetRate, 2, 16, {}, 0));
            if (writer == nullptr)
                return 0.0;
            os.release();   // writer owns the stream now
            writer->writeFromAudioSampleBuffer (*toWrite, 0, toWrite->getNumSamples());
            return toWrite->getNumSamples() / targetRate;
        }

        // Parse "[THREAD i] ( NN.N%)" progress lines; keep the latest per thread so we can report an
        // overall average. Returns true if `overall` was updated.
        void parseProgress (const juce::String& chunk, juce::String& carry,
                            std::map<int, float>& perThread, std::function<void (float)>& progress)
        {
            carry += chunk;
            int nl;
            while ((nl = carry.indexOfChar ('\n')) >= 0)
            {
                const auto line = carry.substring (0, nl);
                carry = carry.substring (nl + 1);

                const int th = line.indexOf ("[THREAD ");
                const int pc = line.indexOf ("%)");
                if (th < 0 || pc < 0) continue;
                const int idx = line.substring (th + 8).getIntValue();
                const int op  = line.lastIndexOfChar ('(');
                if (op < 0 || op >= pc) continue;
                const float pct = line.substring (op + 1, pc).getFloatValue();
                perThread[idx] = juce::jlimit (0.0f, 100.0f, pct);

                if (progress && ! perThread.empty())
                {
                    float sum = 0.0f;
                    for (auto& [k, v] : perThread) sum += v;
                    progress (juce::jlimit (0.0f, 1.0f, (sum / perThread.size()) / 100.0f));
                }
            }
        }
    }

    bool isAvailable()
    {
        const auto bin = locateBin();
        return bin.existsAsFile() && locateModel (bin).existsAsFile();
    }

    Result separate (const juce::File& input, const juce::File& outDir,
                     std::function<void (float)> progress, std::function<bool ()> shouldCancel)
    {
        Result r;
        const auto bin = locateBin();
        if (! bin.existsAsFile()) { r.error = "Stem-splitter helper not found"; return r; }
        const auto model = locateModel (bin);
        if (! model.existsAsFile()) { r.error = "Stem-splitter model not found"; return r; }

        outDir.createDirectory();
        const auto cleanWav = outDir.getChildFile ("_stemsplit_input.wav");
        const double durationSec = writeCleanWav (input, cleanWav);
        if (durationSec <= 0.0) { r.error = "Could not decode source audio"; return r; }
        if (progress) progress (0.02f);

        // Demucs processes fixed ~7.8 s segments, and the multi-threaded helper pads EACH thread's chunk
        // up to a full segment. So over-splitting a short loop into 8 tiny chunks wastes ~4x compute —
        // matching the thread count to the number of real segments (duration / 7.8 s) is much faster
        // (measured: a 15 s loop went 63 s → 35 s). Cap at the core count.
        const int maxThreads = juce::jlimit (1, 8, juce::SystemStats::getNumCpus());
        const int numThreads = juce::jlimit (1, maxThreads,
                                             static_cast<int> (std::lround (durationSec / 7.8)));
        juce::ChildProcess cp;
        juce::StringArray args;
        args.add (bin.getFullPathName());
        args.add (model.getFullPathName());
        args.add (cleanWav.getFullPathName());
        args.add (outDir.getFullPathName());
        args.add (juce::String (numThreads));
        if (! cp.start (args, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
        {
            cleanWav.deleteFile();
            r.error = "Could not launch stem-splitter helper";
            return r;
        }

        juce::String carry;
        std::map<int, float> perThread;
        char buf[8192];
        while (cp.isRunning())
        {
            if (shouldCancel && shouldCancel())
            {
                cp.kill();
                cleanWav.deleteFile();
                r.error = "Cancelled";
                return r;
            }
            const auto n = cp.readProcessOutput (buf, (int) sizeof (buf));
            if (n > 0)
                parseProgress (juce::String::fromUTF8 (buf, n), carry, perThread, progress);
            else
                juce::Thread::sleep (40);
        }
        cleanWav.deleteFile();

        static constexpr std::array<const char*, 6> names { "drums", "bass", "other", "vocals", "guitar", "piano" };
        for (int i = 0; i < 6; ++i)
        {
            auto f = outDir.getChildFile ("target_" + juce::String (i) + "_" + names[(std::size_t) i] + ".wav");
            if (f.existsAsFile() && f.getSize() > 0)
                r.stems.push_back ({ juce::String (names[(std::size_t) i]), f });
        }

        r.ok = ! r.stems.empty();
        if (! r.ok && r.error.isEmpty()) r.error = "No stems were produced";
        if (progress) progress (1.0f);
        return r;
    }
}
