/* -*- c-basic-offset: 4 indent-tabs-mode: nil -*-  vi:set ts=8 sts=4 sw=4: */

/*
  NNLS-Chroma / Chordino

  Audio feature extraction plugins for chromagram and chord
  estimation.

  Centre for Digital Music, Queen Mary University of London.
  This file copyright 2014 QMUL.
    
  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License as
  published by the Free Software Foundation; either version 2 of the
  License, or (at your option) any later version.  See the file
  COPYING included with this distribution for more information.
*/

/*
  Extract chords from an audio file, read using libsndfile.  Works by
  constructing the plugin as a C++ class directly, and using plugin
  adapters from the Vamp Host SDK to provide input.

  You can compile this with e.g. the following (Linux example):

  $ g++ -D_VAMP_PLUGIN_IN_HOST_NAMESPACE -O2 -ffast-math chordextract.cpp Chordino.cpp NNLSBase.cpp chromamethods.cpp viterbi.cpp nnls.c -o chordextract -lsndfile -lvamp-hostsdk -ldl

  But the same idea should work on any platform, so long as the Boost
  Tokenizer headers and the Vamp Host SDK library are available and
  the _VAMP_PLUGIN_IN_HOST_NAMESPACE preprocessor symbol is defined
  throughout.
*/

#define _VAMP_PLUGIN_IN_HOST_NAMESPACE 1

#include <vamp-hostsdk/PluginInputDomainAdapter.h>
#include <vamp-hostsdk/PluginBufferingAdapter.h>

#include "Chordino.h"
#include "NNLSChroma.h"

#include <sndfile.h>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>

using namespace std;
using namespace Vamp;
using namespace Vamp::HostExt;

int main(int argc, char **argv)
{
    const char *myname = argv[0];

    const bool chromaMode = argc == 3 && (string(argv[1]) == "--chroma"
                                      || string(argv[1]) == "--both-chroma"
                                      || string(argv[1]) == "--semitones"
                                      || string(argv[1]) == "--key-features");
    const bool bothChromaMode = chromaMode && string(argv[1]) == "--both-chroma";
    const bool semitoneMode = chromaMode && string(argv[1]) == "--semitones";
    const bool keyFeaturesMode = chromaMode && string(argv[1]) == "--key-features";
    if ((!chromaMode && argc != 2) || (chromaMode && argc != 3)) {
	cerr << "usage: " << myname << " [--chroma|--both-chroma|--semitones|--key-features] file.wav" << endl;
	return 2;
    }

    const char *infile = argv[chromaMode ? 2 : 1];

    SF_INFO sfinfo;
    SNDFILE *sndfile = sf_open(infile, SFM_READ, &sfinfo);

    if (!sndfile) {
	cerr << myname << ": Failed to open input file " << infile
	     << ": " << sf_strerror(sndfile) << endl;
	return 1;
    }

    Plugin *plugin = chromaMode
        ? static_cast<Plugin *>(new NNLSChroma(sfinfo.samplerate))
        : static_cast<Plugin *>(new Chordino(sfinfo.samplerate));
    PluginInputDomainAdapter *ia = new PluginInputDomainAdapter(plugin);
    ia->setProcessTimestampMethod(PluginInputDomainAdapter::ShiftData);
    PluginBufferingAdapter *adapter = new PluginBufferingAdapter(ia);

    int blocksize = adapter->getPreferredBlockSize();

    // Plugin requires 1 channel (we will mix down)
    if (!adapter->initialise(1, blocksize, blocksize)) {
	cerr << myname << ": Failed to initialise Chordino adapter!" << endl;
	return 1;
    }

    float *filebuf = new float[sfinfo.channels * blocksize];
    float *mixbuf = new float[blocksize];

    Plugin::FeatureList outputFeatures;
    Plugin::FeatureList secondaryOutputFeatures;
    Plugin::FeatureSet fs;

    int outputFeatureNo = -1;
    int secondaryOutputFeatureNo = -1;
    Plugin::OutputList outputs = adapter->getOutputDescriptors();
    for (int i = 0; i < int(outputs.size()); ++i) {
	if (outputs[i].identifier == (keyFeaturesMode ? "bothchroma"
                                              : semitoneMode ? "semitonespectrum"
                                              : bothChromaMode ? "bothchroma"
                                              : chromaMode ? "chroma"
                                              : "simplechord")) {
	    outputFeatureNo = i;
	}
	if (keyFeaturesMode && outputs[i].identifier == "semitonespectrum") {
	    secondaryOutputFeatureNo = i;
	}
    }
    if (outputFeatureNo < 0 || (keyFeaturesMode && secondaryOutputFeatureNo < 0)) {
	cerr << myname << ": Failed to identify requested output!" << endl;
	return 1;
    }
    
    int frame = 0;
    const int64_t maxFrames = keyFeaturesMode
        ? std::min<int64_t>(sfinfo.frames, static_cast<int64_t>(sfinfo.samplerate) * 30)
        : sfinfo.frames;
    while (frame < maxFrames) {

	int count = -1;
	const int framesToRead = static_cast<int>(std::min<int64_t>(blocksize, maxFrames - frame));
	if ((count = sf_readf_float(sndfile, filebuf, framesToRead)) <= 0) break;

	// mix down
	for (int i = 0; i < blocksize; ++i) {
	    mixbuf[i] = 0.f;
	    if (i < count) {
		for (int c = 0; c < sfinfo.channels; ++c) {
		    mixbuf[i] += filebuf[i * sfinfo.channels + c] / sfinfo.channels;
		}
	    }
	}

	RealTime timestamp = RealTime::frame2RealTime(frame, sfinfo.samplerate);
	
	// feed to plugin: can just take address of buffer, as only one channel
	fs = adapter->process(&mixbuf, timestamp);

	outputFeatures.insert(outputFeatures.end(),
			     fs[outputFeatureNo].begin(),
			     fs[outputFeatureNo].end());
	if (keyFeaturesMode) {
	    secondaryOutputFeatures.insert(secondaryOutputFeatures.end(),
			     fs[secondaryOutputFeatureNo].begin(),
			     fs[secondaryOutputFeatureNo].end());
	}

	frame += count;
    }

    sf_close(sndfile);

    // features at end of processing (actually Chordino does all its work here)
    fs = adapter->getRemainingFeatures();

    // chord output is output index 0
    outputFeatures.insert(outputFeatures.end(),
			 fs[outputFeatureNo].begin(),
			 fs[outputFeatureNo].end());

    if (keyFeaturesMode) {
        secondaryOutputFeatures.insert(secondaryOutputFeatures.end(),
                         fs[secondaryOutputFeatureNo].begin(),
                         fs[secondaryOutputFeatureNo].end());
    }

    const int outputCount = keyFeaturesMode
        ? std::min<int>(outputFeatures.size(), secondaryOutputFeatures.size())
        : static_cast<int>(outputFeatures.size());
    for (int i = 0; i < outputCount; ++i) {
	cout << outputFeatures[i].timestamp.toString() << ":";
        if (chromaMode) {
            for (float value : outputFeatures[i].values) cout << " " << value;
            if (keyFeaturesMode)
                for (float value : secondaryOutputFeatures[i].values) cout << " " << value;
        } else {
            cout << " " << outputFeatures[i].label;
        }
        cout << endl;
    }

    delete[] filebuf;
    delete[] mixbuf;
    
    delete adapter;
}
