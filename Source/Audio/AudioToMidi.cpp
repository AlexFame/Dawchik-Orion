#include "AudioToMidi.h"

#include <juce_core/juce_core.h>

namespace orion::a2m
{
    namespace
    {
        // Walk up from the executable looking for Resources/audio2midi/transcribe.py, so both the
        // dev build (repo tree) and a shipped bundle (Contents/Resources) resolve without config.
        juce::File locateScript()
        {
            if (auto* env = std::getenv ("ORION_A2M_SCRIPT"))
            {
                juce::File f (juce::String::fromUTF8 (env));
                if (f.existsAsFile()) return f;
            }

            const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);

            // Direct bundle layout: Contents/MacOS/Orion → Contents/Resources/audio2midi/…
            {
                auto f = exe.getParentDirectory().getParentDirectory()
                             .getChildFile ("Resources/audio2midi/transcribe.py");
                if (f.existsAsFile()) return f;
            }

            // User-installed copy (for a shipped app whose bundle didn't embed it).
            {
                auto f = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                             .getChildFile ("Orion/audio2midi/transcribe.py");
                if (f.existsAsFile()) return f;
            }

            // Dev tree: climb parents until we find Orion/Resources/audio2midi/transcribe.py.
            juce::File dir = exe.getParentDirectory();
            for (int i = 0; i < 10 && dir.exists(); ++i)
            {
                auto f = dir.getChildFile ("Resources/audio2midi/transcribe.py");
                if (f.existsAsFile()) return f;
                auto parent = dir.getParentDirectory();
                if (parent == dir) break;
                dir = parent;
            }
            return {};
        }

        // The venv interpreter sits next to the script: <audio2midi>/venv/bin/python.
        juce::File locatePython (const juce::File& script)
        {
            if (auto* env = std::getenv ("ORION_A2M_PYTHON"))
            {
                juce::File f (juce::String::fromUTF8 (env));
                if (f.existsAsFile()) return f;
            }
            if (script.existsAsFile())
            {
                auto f = script.getParentDirectory().getChildFile ("venv/bin/python");
                if (f.existsAsFile()) return f;
            }
            return {};
        }

        // Fold to mono/stereo and write a 44.1 kHz 16-bit WAV — a clean input the model likes.
        bool writeWav (const juce::AudioBuffer<float>& audio, double sampleRate, const juce::File& dest)
        {
            if (audio.getNumSamples() <= 0 || sampleRate <= 0.0)
                return false;

            dest.deleteFile();
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::FileOutputStream> os (dest.createOutputStream());
            if (os == nullptr)
                return false;

            const int ch = juce::jlimit (1, 2, audio.getNumChannels());
            std::unique_ptr<juce::AudioFormatWriter> writer (
                wav.createWriterFor (os.get(), sampleRate, static_cast<unsigned int> (ch), 16, {}, 0));
            if (writer == nullptr)
                return false;
            os.release(); // writer owns the stream now

            writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
            writer.reset(); // flush + close
            return dest.existsAsFile() && dest.getSize() > 44;
        }
    }

    bool isAvailable()
    {
        const auto script = locateScript();
        return script.existsAsFile() && locatePython (script).existsAsFile();
    }

    Result transcribe (const juce::AudioBuffer<float>& audio, double sampleRate,
                       Options opt, std::function<bool()> cancel)
    {
        Result r;

        const auto script = locateScript();
        const auto python = locatePython (script);
        if (! script.existsAsFile() || ! python.existsAsFile())
        {
            r.error = "Audio-to-MIDI helper not installed";
            return r;
        }

        auto tmpDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("Orion_a2m_" + juce::String (juce::Time::currentTimeMillis()));
        tmpDir.createDirectory();
        const juce::File inWav  = tmpDir.getChildFile ("in.wav");
        const juce::File outJson = tmpDir.getChildFile ("notes.json");
        const juce::ScopeGuard cleanup { [&] { tmpDir.deleteRecursively(); } };

        if (! writeWav (audio, sampleRate, inWav))
        {
            r.error = "Could not stage audio for transcription";
            return r;
        }
        if (cancel && cancel())
            return r;

        juce::StringArray args;
        args.add (python.getFullPathName());
        args.add (script.getFullPathName());
        args.add (inWav.getFullPathName());
        args.add (outJson.getFullPathName());
        args.add ("--onset");       args.add (juce::String (opt.onsetThreshold, 3));
        args.add ("--frame");       args.add (juce::String (opt.frameThreshold, 3));
        args.add ("--min-note-ms"); args.add (juce::String (opt.minNoteMs, 1));
        if (opt.minFreqHz > 0.0) { args.add ("--min-freq"); args.add (juce::String (opt.minFreqHz, 1)); }
        if (opt.maxFreqHz > 0.0) { args.add ("--max-freq"); args.add (juce::String (opt.maxFreqHz, 1)); }
        if (opt.multiPitchBends) args.add ("--multi-bends");

        juce::ChildProcess cp;
        if (! cp.start (args, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
        {
            r.error = "Could not launch transcription process";
            return r;
        }

        // Drain output while polling for cancellation (transcription of a loop is a few seconds).
        juce::String log;
        char buf[2048];
        for (;;)
        {
            const int got = cp.readProcessOutput (buf, sizeof (buf));
            if (got > 0)
                log += juce::String::fromUTF8 (buf, got);
            else if (! cp.isRunning())
                break;
            if (cancel && cancel())
            {
                cp.kill();
                return r;
            }
            if (got <= 0)
                juce::Thread::sleep (30);
        }

        if (! outJson.existsAsFile())
        {
            r.error = "Transcription produced no output";
            return r;
        }

        auto parsed = juce::JSON::parse (outJson.loadFileAsString());
        if (auto* obj = parsed.getDynamicObject())
        {
            if (! obj->getProperty ("ok"))
            {
                r.error = obj->getProperty ("error").toString();
                if (r.error.isEmpty()) r.error = "Transcription failed";
                return r;
            }
            if (auto* arr = obj->getProperty ("notes").getArray())
            {
                r.notes.reserve (static_cast<std::size_t> (arr->size()));
                for (auto& v : *arr)
                {
                    if (auto* n = v.getDynamicObject())
                    {
                        Note note;
                        note.startSec = static_cast<double> (n->getProperty ("start"));
                        note.endSec   = static_cast<double> (n->getProperty ("end"));
                        note.pitch    = static_cast<int>    (n->getProperty ("pitch"));
                        note.amp      = static_cast<float>  (static_cast<double> (n->getProperty ("amp")));
                        if (note.endSec > note.startSec && note.pitch >= 0 && note.pitch <= 127)
                            r.notes.push_back (note);
                    }
                }
            }
            r.ok = true;
            return r;
        }

        r.error = "Could not parse transcription output";
        return r;
    }
}
