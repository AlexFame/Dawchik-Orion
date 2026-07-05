#include "SoundClassifier.h"

#if JUCE_MAC

#import <Foundation/Foundation.h>
#import <SoundAnalysis/SoundAnalysis.h>

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

// Observer accumulating the SUM and COUNT of confidence per class across all windows, so we can use
// the AVERAGE confidence — a single noisy window (e.g. a spurious "vocal" spike) won't win over the
// sound that's actually present most of the time.
@interface OrionSoundObserver : NSObject <SNResultsObserving>
@property (nonatomic, strong) NSMutableDictionary<NSString*, NSNumber*>* sums;
@property (nonatomic, assign) int windows;
@end

@implementation OrionSoundObserver
- (instancetype)init
{
    self = [super init];
    if (self != nil)
    {
        _sums = [NSMutableDictionary dictionary];
        _windows = 0;
    }
    return self;
}

- (void)request:(id<SNRequest>)request didProduceResult:(id<SNResult>)result
{
    if (![result isKindOfClass:[SNClassificationResult class]])
        return;
    SNClassificationResult* cr = (SNClassificationResult*)result;
    ++self.windows;
    for (SNClassification* c in cr.classifications)
    {
        NSNumber* prev = self.sums[c.identifier];
        self.sums[c.identifier] = @((prev != nil ? prev.doubleValue : 0.0) + c.confidence);
    }
}

- (void)request:(id<SNRequest>)request didFailWithError:(NSError*)error {}
- (void)requestDidComplete:(id<SNRequest>)request {}
@end

namespace orion
{
namespace
{
// Map an Apple identifier (snake_case, e.g. "violin_fiddle") to our tag. Substring match keeps it
// robust to Apple's exact naming across OS versions. Order matters a little (more specific first).
juce::String tagForIdentifier(const juce::String& idLower)
{
    static const std::pair<const char*, const char*> map[] = {
        { "violin", "Violin" }, { "fiddle", "Violin" }, { "cello", "Cello" }, { "viola", "Viola" },
        { "double_bass", "Bass" }, { "harp", "Harp" }, { "string", "Strings" },
        { "piano", "Piano" }, { "keyboard", "Keys" }, { "organ", "Organ" }, { "accordion", "Accordion" },
        { "acoustic_guitar", "Guitar" }, { "electric_guitar", "Guitar" }, { "bass_guitar", "Bass" },
        { "guitar", "Guitar" }, { "banjo", "Banjo" }, { "ukulele", "Ukulele" }, { "mandolin", "Mandolin" },
        { "bass_drum", "Kick" }, { "snare", "Snare" }, { "hi_hat", "Hat" }, { "hihat", "Hat" },
        { "cymbal", "Cymbal" }, { "tabla", "Perc" }, { "drum_kit", "Drums" }, { "drum", "Drums" },
        { "percussion", "Perc" }, { "tambourine", "Perc" }, { "cowbell", "Perc" }, { "gong", "Perc" },
        { "trumpet", "Trumpet" }, { "trombone", "Brass" }, { "saxophone", "Sax" }, { "brass", "Brass" },
        { "french_horn", "Brass" }, { "tuba", "Brass" }, { "flute", "Flute" }, { "clarinet", "Clarinet" },
        { "oboe", "Oboe" }, { "harmonica", "Harmonica" }, { "bagpipe", "Bagpipes" }, { "wind_instrument", "Winds" },
        { "singing", "Vocal" }, { "vocal", "Vocal" }, { "choir", "Choir" }, { "chant", "Vocal" },
        { "rapping", "Vocal" }, { "synthesizer", "Synth" }, { "bell", "Bell" }, { "chime", "Bell" },
        { "xylophone", "Mallet" }, { "marimba", "Mallet" }, { "vibraphone", "Mallet" }, { "glockenspiel", "Mallet" },
        { "plucked_string", "Pluck" }, { "sitar", "Sitar" }, { "steel_guitar", "Guitar" },
    };
    for (const auto& [needle, tag] : map)
        if (idLower.contains(needle))
            return juce::String(tag);
    return {};
}
}  // namespace

juce::StringArray classifyWithSoundAnalysis(const juce::File& file)
{
    juce::StringArray tags;
    @autoreleasepool
    {
        NSString* path = [NSString stringWithUTF8String:file.getFullPathName().toRawUTF8()];
        if (path == nil)
            return tags;
        NSURL* url = [NSURL fileURLWithPath:path];

        NSError* error = nil;
        SNAudioFileAnalyzer* analyzer = [[SNAudioFileAnalyzer alloc] initWithURL:url error:&error];
        if (analyzer == nil || error != nil)
            return tags;

        SNClassifySoundRequest* request =
            [[SNClassifySoundRequest alloc] initWithClassifierIdentifier:SNClassifierIdentifierVersion1 error:&error];
        if (request == nil || error != nil)
            return tags;

        OrionSoundObserver* observer = [[OrionSoundObserver alloc] init];
        if (![analyzer addRequest:request withObserver:observer error:&error] || error != nil)
            return tags;

        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        [analyzer analyzeWithCompletionHandler:^(BOOL) { dispatch_semaphore_signal(done); }];
        // Bounded wait so a stuck analysis can never hang the background pool.
        dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(20 * NSEC_PER_SEC)));

        if (observer.windows <= 0)
            return tags;

        // Average confidence per mapped instrument tag across windows; keep only the strongest ones.
        // Averaging + a firm threshold kills one-window spikes (the false "vocal" problem).
        constexpr double kMinAvgConfidence = 0.55;
        constexpr int    kMaxInstrumentTags = 2;
        std::map<std::string, double> tagAvg;   // tag -> best avg confidence among its identifiers
        for (NSString* identifier in observer.sums)
        {
            const double avg = observer.sums[identifier].doubleValue / observer.windows;
            if (avg < kMinAvgConfidence)
                continue;
            const auto tag = tagForIdentifier(juce::String([identifier UTF8String]).toLowerCase());
            if (tag.isEmpty())
                continue;
            const auto key = tag.toStdString();
            if (auto it = tagAvg.find(key); it == tagAvg.end() || avg > it->second)
                tagAvg[key] = avg;
        }

        std::vector<std::pair<std::string, double>> ranked(tagAvg.begin(), tagAvg.end());
        std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        for (int i = 0; i < static_cast<int>(ranked.size()) && i < kMaxInstrumentTags; ++i)
            tags.add(juce::String(ranked[static_cast<std::size_t>(i)].first));
    }
    return tags;
}
}  // namespace orion

#else

namespace orion
{
juce::StringArray classifyWithSoundAnalysis(const juce::File&) { return {}; }
}

#endif
