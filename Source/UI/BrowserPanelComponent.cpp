#include "BrowserPanelComponent.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <regex>
#include <limits>

#include "OrionTheme.h"
#include "OrionPopupMenu.h"

namespace
{
namespace th = orion::theme;
const auto mutedText          = th::text::muted;
const auto rowBackground      = juce::Colours::transparentBlack;
const auto rowHover           = juce::Colours::white.withAlpha(0.055f);
const auto rowSelected        = juce::Colours::white.withAlpha(0.09f);
const auto buttonColour       = th::surface::primary;
const auto buttonOutlineColour = th::line::normal.withAlpha(0.6f);
constexpr int rowHeight = 56;
constexpr int rowGap = th::metrics::rowGap;
// Browser entries are list rows, not floating cards. Their selection frame therefore uses
// square corners; rounded geometry is reserved for controls and the preview surface.
constexpr float rowCornerRadius = 0.0f;
constexpr int dragThresholdPx = 5;
constexpr int headerHeight = 176; // title + location + search + type filters + breathing room
constexpr int browserSectionGap = th::metrics::gridUnit * 2;
constexpr int listTopPadding = th::metrics::gridUnit * 3;
constexpr int previewBarHeight = 72; // bottom preview card (waveform + play + name)
constexpr int previewSyncRowHeight = 28; // SYNC toggle row, sits below the preview card
constexpr int tagsRowHeight = 34; // "Tags:" chips row for the selected sample, above the preview
constexpr int contentPadX = 4; // uniform horizontal inset so left/right padding stays symmetric
constexpr float horizontalSwipeThreshold = 0.14f;
constexpr int horizontalSwipeLockMs = 320;
const juce::String mountedDevicesHubName = "Mounted Devices";
// Two fixed Orion accent colours keep the metadata readable and coherent: coral for
// tempo and cyan for key. drawBadge softens them for the dark browser surface.
const auto bpmBadgeColour = th::accent::activeCoral;
const auto keyBadgeColour = th::accent::brandCyanDim;

bool isMountedVolumePath(const juce::File& file)
{
    const auto path = file.getFullPathName();
    return path == "/Volumes" || path.startsWith("/Volumes/");
}

juce::Font browserFont(float size, bool bold = false)
{
    auto font = bold
        ? juce::Font(juce::FontOptions("Avenir Next", size, juce::Font::bold))
        : juce::Font(juce::FontOptions("Avenir Next", "Medium", size));
    // UI copy stays compact; tracking is reserved for branding, not every browser label.
    font.setExtraKerningFactor(bold ? 0.004f : 0.0f);
    return font;
}

class BrowserButtonLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    void drawButtonBackground(juce::Graphics& g, juce::Button& button,
                              const juce::Colour&, bool isMouseOverButton,
                              bool isButtonDown) override
    {
        auto bounds = button.getLocalBounds().toFloat().reduced(0.5f);
        const auto active = button.getToggleState();
        const auto radius = juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.5f;
        auto fill = active ? th::accent::activeCoral.withAlpha(0.18f)
                           : th::surface::elevated.withAlpha(isMouseOverButton ? 0.78f : 0.46f);
        if (isButtonDown)
            fill = active ? th::accent::activeCoralLight.withAlpha(0.28f)
                          : th::surface::hover.withAlpha(0.92f);
        g.setColour(fill);
        g.fillRoundedRectangle(bounds, radius);

        const auto outline = active ? th::accent::activeCoral.withAlpha(0.86f)
                                    : th::line::normal.withAlpha(isMouseOverButton ? 0.92f : 0.72f);
        g.setColour(outline);
        g.drawRoundedRectangle(bounds, radius, active ? 1.2f : 1.0f);
    }

    void drawButtonText(juce::Graphics& g, juce::TextButton& button, bool, bool) override
    {
        g.setColour(button.getToggleState() ? th::text::primary : th::text::secondary);
        g.setFont(browserFont(14.0f, true));
        g.drawText(button.getButtonText(), button.getLocalBounds().reduced(8, 0),
                   juce::Justification::centred, true);
    }
};

BrowserButtonLookAndFeel browserButtonLookAndFeel;

std::vector<float> readDragWaveform(const juce::File& file, int pointCount)
{
    std::vector<float> peaks;
    if (! file.existsAsFile())
        return peaks;

    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));
    if (reader == nullptr || reader->lengthInSamples <= 0)
        return peaks;

    peaks.resize(static_cast<std::size_t>(pointCount), 0.0f);
    juce::AudioBuffer<float> buffer(1, 4096);
    for (int i = 0; i < pointCount; ++i)
    {
        const auto start = static_cast<int64_t>(i) * reader->lengthInSamples / pointCount;
        const auto count = static_cast<int>(juce::jmin<int64_t>(buffer.getNumSamples(),
                                                                 reader->lengthInSamples - start));
        if (count <= 0)
            break;
        buffer.clear();
        reader->read(&buffer, 0, count, start, true, true);
        peaks[static_cast<std::size_t>(i)] = juce::jlimit(0.0f, 1.0f,
                                                          buffer.getMagnitude(0, 0, count));
    }
    return peaks;
}

juce::Image makeDragClip(const orion::BrowserItem& item, const std::vector<float>& peaks)
{
    constexpr int width = 286;
    constexpr int height = 56;
    juce::Image image(juce::Image::ARGB, width, height, true);
    juce::Graphics graphics(image);
    const auto card = image.getBounds().toFloat().reduced(1.0f);
    const auto variant = th::tracks::variantsFor(item.colour);
    juce::ColourGradient body(variant.gradientTop, card.getX(), card.getY(),
                              variant.gradientBottom, card.getX(), card.getBottom(), false);
    graphics.setGradientFill(body);
    graphics.fillRoundedRectangle(card, th::metrics::controlRadius);

    juce::Path headerPath;
    headerPath.addRoundedRectangle(card.getX(), card.getY(), card.getWidth(), 18.0f,
                                   th::metrics::controlRadius, th::metrics::controlRadius,
                                   true, true, false, false);
    graphics.setColour(variant.gradientBottom.darker(0.30f).withAlpha(0.92f));
    graphics.fillPath(headerPath);

    graphics.setColour(juce::Colours::white.withAlpha(0.92f));
    graphics.setFont(browserFont(14.0f, juce::Font::bold));
    graphics.drawText(item.name, juce::Rectangle<int>(10, 5, width - 20, 17),
                      juce::Justification::centredLeft, true);

    if (peaks.empty())
        return image;

    const auto waveArea = juce::Rectangle<float>(10.0f, 22.0f, width - 20.0f, 26.0f);
    const auto centreY = waveArea.getCentreY();
    const auto amplitude = waveArea.getHeight() * 0.48f;
    juce::Path wave;
    wave.startNewSubPath(waveArea.getX(), centreY);
    for (int i = 0; i < static_cast<int>(peaks.size()); ++i)
    {
        const auto x = static_cast<float>(i) / static_cast<float>(juce::jmax(1, static_cast<int>(peaks.size()) - 1))
                       * waveArea.getWidth() + waveArea.getX();
        wave.lineTo(x, centreY - peaks[static_cast<std::size_t>(i)] * amplitude);
    }
    for (int i = static_cast<int>(peaks.size()) - 1; i >= 0; --i)
    {
        const auto x = static_cast<float>(i) / static_cast<float>(juce::jmax(1, static_cast<int>(peaks.size()) - 1))
                       * waveArea.getWidth() + waveArea.getX();
        wave.lineTo(x, centreY + peaks[static_cast<std::size_t>(i)] * amplitude);
    }
    wave.closeSubPath();
    graphics.setColour(variant.waveform.withAlpha(0.92f));
    graphics.fillPath(wave);
    graphics.setColour(variant.waveform.withAlpha(0.86f));
    graphics.drawRoundedRectangle(card.reduced(0.5f), th::metrics::controlRadius, 1.0f);
    return image;
}

juce::File getMacBrowseRoot()
{
    return juce::File("/");
}

juce::File getUserHomeDirectory()
{
    return juce::File::getSpecialLocation(juce::File::userHomeDirectory);
}

bool isUserHomeDirectory(const juce::File& directory)
{
    return directory == getUserHomeDirectory();
}

int getContentHeight(int itemCount) noexcept
{
    return juce::jmax(0, itemCount * (rowHeight + rowGap) - rowGap);
}

double parseBpmFromFileName(const juce::File& file)
{
    const auto text = file.getFileNameWithoutExtension().toLowerCase();

    // Several sources, in priority order — a source only wins if the earlier ones didn't answer.

    // Source 1 (most reliable): an explicit "… 120 bpm" marker.
    if (const auto bpmIndex = text.indexOf("bpm"); bpmIndex >= 0)
    {
        const auto beforeBpm = text.substring(0, bpmIndex).trimEnd();
        juce::String number;
        for (int i = beforeBpm.length() - 1; i >= 0; --i)
        {
            const auto c = beforeBpm[i];
            if (juce::CharacterFunctions::isDigit(c) || c == '.')
                number = juce::String::charToString(c) + number;
            else if (number.isNotEmpty())
                break;
        }
        const auto bpm = number.getDoubleValue();
        if (bpm >= 40.0 && bpm <= 260.0)
            return bpm;
        // else: no valid number by the marker — fall through to the next source.
    }

    // Source 2 (fallback): the most plausible bare number (40–260) anywhere in the name, so files
    // like "Warp 86" / "Track 128" read as tempo. Prefers the longest number (e.g. "128" over a
    // stray "2"); out-of-range numbers (e.g. "808", "2024") are ignored.
    double bestBpm = 0.0;
    int bestDigitCount = 0;
    juce::String currentNumber;
    const auto scan = text + " ";
    for (int i = 0; i < scan.length(); ++i)
    {
        const auto c = scan[i];
        if (juce::CharacterFunctions::isDigit(c)) { currentNumber += juce::String::charToString(c); continue; }
        if (currentNumber.isNotEmpty())
        {
            const auto candidate = currentNumber.getDoubleValue();
            if (candidate >= 40.0 && candidate <= 260.0 && currentNumber.length() >= bestDigitCount)
            {
                bestBpm = candidate;
                bestDigitCount = currentNumber.length();
            }
            currentNumber.clear();
        }
    }
    return bestBpm;
}

std::optional<juce::String> parseKeyFromFileName(const juce::File& file)
{
    auto text = file.getFileNameWithoutExtension().toStdString();
    std::replace(text.begin(), text.end(), '_', ' ');
    std::replace(text.begin(), text.end(), '-', ' ');

    static const std::regex keyPattern(
        R"((^|[^A-Za-z0-9])([A-Ga-g])\s*(#|b)?\s*(min|minor|maj|major|m)(?=$|[^A-Za-z0-9]))",
        std::regex_constants::icase);

    std::smatch match;
    if (! std::regex_search(text, match, keyPattern))
        return std::nullopt;

    auto root = juce::String(match[2].str()).toUpperCase();
    const auto accidental = juce::String(match[3].str());
    if (accidental.isNotEmpty())
        root += accidental == "b" ? "b" : "#";

    const auto quality = juce::String(match[4].str()).toLowerCase();
    return root + (quality.startsWith("maj") ? " Maj" : " Min");
}

void drawBadge(juce::Graphics& g, juce::Rectangle<int> bounds, const juce::String& text, juce::Colour colour)
{
    if (bounds.getWidth() <= 0 || text.isEmpty())
        return;

    // Metadata is secondary to the sample name: use the Orion accent as a quiet tint,
    // not as a second headline competing with the row title.
    g.setColour(colour.withAlpha(0.24f));
    g.fillRoundedRectangle(bounds.toFloat(), th::metrics::controlRadius);
    g.setColour(colour.withAlpha(0.42f));
    g.drawRoundedRectangle(bounds.toFloat().reduced(0.5f), th::metrics::controlRadius, 0.8f);
    g.setColour(th::text::secondary.withAlpha(0.92f));
    g.setFont(browserFont(13.0f, juce::Font::bold));
    g.drawText(text, bounds.reduced(6, 0), juce::Justification::centred, true);
}

// Draws a small glyph inside an icon box: folder for directories, an up-arrow for the
// ".." parent link, a waveform for audio files. `colour` should already be set by the caller.
void drawBrowserEntryIcon(juce::Graphics& g, juce::Rectangle<float> a, bool isDirectory, bool isParentLink)
{
    if (isParentLink)
    {
        // Up arrow.
        const auto cx = a.getCentreX(), cy = a.getCentreY();
        const auto s = juce::jmin(a.getWidth(), a.getHeight()) * 0.34f;
        juce::Path p;
        p.startNewSubPath(cx, cy - s);
        p.lineTo(cx - s, cy + s * 0.2f);
        p.lineTo(cx - s * 0.45f, cy + s * 0.2f);
        p.lineTo(cx - s * 0.45f, cy + s);
        p.lineTo(cx + s * 0.45f, cy + s);
        p.lineTo(cx + s * 0.45f, cy + s * 0.2f);
        p.lineTo(cx + s, cy + s * 0.2f);
        p.closeSubPath();
        g.fillPath(p);
        return;
    }

    if (isDirectory)
    {
        // Folder silhouette with a tab.
        auto f = a.reduced(a.getWidth() * 0.16f, a.getHeight() * 0.22f);
        const auto r = 2.0f;
        const auto tabW = f.getWidth() * 0.42f;
        const auto tabH = f.getHeight() * 0.24f;
        const auto bodyTop = f.getY() + tabH;
        juce::Path p;
        p.startNewSubPath(f.getX(), f.getY() + r);
        p.lineTo(f.getX(), f.getBottom() - r);
        p.quadraticTo(f.getX(), f.getBottom(), f.getX() + r, f.getBottom());
        p.lineTo(f.getRight() - r, f.getBottom());
        p.quadraticTo(f.getRight(), f.getBottom(), f.getRight(), f.getBottom() - r);
        p.lineTo(f.getRight(), bodyTop + r);
        p.quadraticTo(f.getRight(), bodyTop, f.getRight() - r, bodyTop);
        p.lineTo(f.getX() + tabW + r, bodyTop);
        p.lineTo(f.getX() + tabW - tabH * 0.4f, f.getY() + r);
        p.quadraticTo(f.getX() + tabW - tabH * 0.4f - r, f.getY(), f.getX() + tabW - tabH * 0.4f - r * 2.0f, f.getY());
        p.lineTo(f.getX() + r, f.getY());
        p.quadraticTo(f.getX(), f.getY(), f.getX(), f.getY() + r);
        p.closeSubPath();
        g.fillPath(p);
        return;
    }

    // Audio file: a little symmetric waveform of vertical bars.
    const float hs[] = { 0.30f, 0.62f, 0.92f, 0.55f, 0.78f, 0.40f };
    const int n = 6;
    const auto bw = a.getWidth() / (n * 1.7f);
    for (int i = 0; i < n; ++i)
    {
        const auto h = a.getHeight() * hs[i];
        const auto x = a.getX() + bw * 1.7f * i + bw * 0.35f;
        g.fillRoundedRectangle(x, a.getCentreY() - h * 0.5f, bw, h, bw * 0.45f);
    }
}

juce::String normalisedBrowserPathText(const juce::File& file)
{
    return file.getFullPathName().toLowerCase()
               .replaceCharacter('-', ' ')
               .replaceCharacter('_', ' ')
               .replaceCharacter('/', ' ');
}

bool pathLooksLikeLoop(const juce::String& text)
{
    return text.contains("loop")
        || text.contains("loops")
        || text.contains("луп")
        || text.contains("melody")
        || text.contains("melodies")
        || text.contains("phrase")
        || text.contains("phrases")
        || text.contains("construction")
        || text.contains("midi");
}

bool pathLooksLikeOneShot(const juce::String& text)
{
    return text.contains("one shot")
        || text.contains("oneshot")
        || text.contains("one shots")
        || text.contains("oneshots")
        || text.contains("1shot")
        || text.contains("ван шот")
        || text.contains("ваншот")
        || text.contains("drum hits")
        || text.contains("hits")
        || text.contains("stab")
        || text.contains("stabs");
}

bool pathLooksLikeDrumOneShot(const juce::String& text)
{
    return text.contains("drum")
        || text.contains("kick")
        || text.contains("kicks")
        || text.contains("snare")
        || text.contains("snares")
        || text.contains("clap")
        || text.contains("claps")
        || text.contains("hat")
        || text.contains("hats")
        || text.contains("perc")
        || text.contains("percs")
        || text.contains("fx");
}

juce::String metadataTypeForFile(const juce::File& file)
{
    const auto text = normalisedBrowserPathText(file);
    const auto explicitOneShot = pathLooksLikeOneShot(text);
    const auto loop = pathLooksLikeLoop(text) || parseBpmFromFileName(file) > 0.0;

    if (explicitOneShot)
        return "One-shot";

    if (loop)
        return "Loop";

    if (pathLooksLikeDrumOneShot(text))
        return "One-shot";

    return "Audio";
}

// Auto-tag a sample from its file/folder name: type (Loop / One-shot) + content/instrument.
// Synonyms collapse to one canonical tag (vox → Vocal, hh → Hat). Heuristic, filename-based —
// like Ableton's tags for un-analysed user samples — not audio-content ML.
juce::StringArray deriveTags(const juce::File& file)
{
    juce::StringArray tags;
    const auto add = [&tags](const juce::String& t) { if (! tags.contains(t)) tags.add(t); };

    const auto type = metadataTypeForFile(file);
    if (type == "Loop" || type == "One-shot")
        add(type);

    const auto text = normalisedBrowserPathText(file);
    static const std::array<std::pair<const char*, const char*>, 52> keywordTags {{
        { "violin", "Violin" }, { "viola", "Viola" }, { "cello", "Cello" }, { "string", "Strings" },
        { "piano", "Piano" }, { "rhodes", "Rhodes" }, { "organ", "Organ" }, { "keys", "Keys" },
        { "guitar", "Guitar" }, { "808", "808" }, { "bass", "Bass" }, { "sub", "Sub" },
        { "kick", "Kick" }, { "snare", "Snare" }, { "clap", "Clap" }, { "hihat", "Hat" }, { "hat", "Hat" },
        { "crash", "Crash" }, { "ride", "Ride" }, { "cymbal", "Cymbal" }, { "tom", "Tom" }, { "perc", "Perc" },
        { "shaker", "Shaker" }, { "conga", "Conga" }, { "bongo", "Bongo" }, { "rim", "Rim" }, { "drum", "Drums" },
        { "pad", "Pad" }, { "lead", "Lead" }, { "pluck", "Pluck" }, { "arp", "Arp" }, { "chord", "Chords" },
        { "melod", "Melody" }, { "bell", "Bell" }, { "flute", "Flute" }, { "sax", "Sax" }, { "trumpet", "Trumpet" },
        { "brass", "Brass" }, { "horn", "Brass" }, { "vocal", "Vocal" }, { "vox", "Vocal" }, { "choir", "Choir" },
        { "synth", "Synth" }, { "fx", "FX" }, { "riser", "Riser" }, { "sweep", "Sweep" }, { "downlifter", "Downlifter" },
        { "impact", "Impact" }, { "snap", "Snap" }, { "whistle", "Whistle" }, { "acap", "Vocal" }, { "vocs", "Vocal" },
    }};
    for (const auto& [needle, tag] : keywordTags)
        if (text.contains(needle))
            add(tag);

    return tags;
}

struct BrowserSearchFilter
{
    bool wantsLoop { false };
    bool wantsOneShot { false };
    std::optional<double> minBpm;
    std::optional<double> maxBpm;
    juce::String keyQuery;
    juce::StringArray terms;
};

int cappedEditDistance(const juce::String& lhs, const juce::String& rhs, int limit)
{
    if (std::abs(lhs.length() - rhs.length()) > limit)
        return limit + 1;

    std::vector<int> previous(static_cast<std::size_t>(rhs.length() + 1));
    std::vector<int> current(static_cast<std::size_t>(rhs.length() + 1));
    for (int j = 0; j <= rhs.length(); ++j)
        previous[static_cast<std::size_t>(j)] = j;

    for (int i = 1; i <= lhs.length(); ++i)
    {
        current[0] = i;
        auto rowMinimum = current[0];
        for (int j = 1; j <= rhs.length(); ++j)
        {
            const auto cost = lhs[i - 1] == rhs[j - 1] ? 0 : 1;
            current[static_cast<std::size_t>(j)] = std::min({
                previous[static_cast<std::size_t>(j)] + 1,
                current[static_cast<std::size_t>(j - 1)] + 1,
                previous[static_cast<std::size_t>(j - 1)] + cost
            });
            rowMinimum = std::min(rowMinimum, current[static_cast<std::size_t>(j)]);
        }
        if (rowMinimum > limit)
            return limit + 1;
        std::swap(previous, current);
    }
    return previous[static_cast<std::size_t>(rhs.length())];
}

juce::String compactSearchText(juce::String text)
{
    return text.toLowerCase().removeCharacters(" -_./\\,;:()[]{}");
}

bool fuzzyTermMatches(const juce::String& searchable, const juce::String& term)
{
    if (term.isEmpty() || searchable.contains(term))
        return true;

    const auto maxDistance = term.length() >= 6 ? 2 : 1;
    juce::StringArray words;
    words.addTokens(searchable, " \t\r\n-_./\\,;:()[]{}", "");
    for (const auto& word : words)
    {
        if (word.length() < term.length() - maxDistance
            || word.length() > term.length() + maxDistance)
            continue;
        if (cappedEditDistance(word, term, maxDistance) <= maxDistance)
            return true;
    }
    return false;
}

std::optional<std::pair<double, double>> parseBpmRange(const juce::String& value)
{
    const auto range = value.trim().replaceCharacter('-', ':');
    juce::StringArray parts;
    parts.addTokens(range, ":", "");
    if (parts.size() == 1)
    {
        const auto bpm = parts[0].getDoubleValue();
        if (bpm > 0.0)
            return std::make_pair(bpm, bpm);
    }
    if (parts.size() == 2)
    {
        const auto low = parts[0].getDoubleValue();
        const auto high = parts[1].getDoubleValue();
        if (low > 0.0 && high > 0.0)
            return std::make_pair(std::min(low, high), std::max(low, high));
    }
    return std::nullopt;
}

bool isLoopItem(const orion::BrowserItem& item)
{
    if (item.isParentLink)
        return false;

    return metadataTypeForFile(item.file) == "Loop";
}

bool isOneShotItem(const orion::BrowserItem& item)
{
    if (item.isParentLink)
        return false;

    return metadataTypeForFile(item.file) == "One-shot";
}

BrowserSearchFilter parseBrowserSearchFilter(const juce::String& query)
{
    auto normalised = query.toLowerCase();
    normalised = normalised.replaceCharacter('_', ' ')
                           .replaceCharacter('/', ' ')
                           .replaceCharacter('.', ' ')
                           .replaceCharacter(',', ' ');

    BrowserSearchFilter filter;
    filter.wantsLoop = normalised.contains("loop")
                    || normalised.contains("луп")
                    || normalised.contains("петл");
    filter.wantsOneShot = normalised.contains("one shot")
                       || normalised.contains("oneshot")
                       || normalised.contains("1shot")
                       || normalised.contains("ван шот")
                       || normalised.contains("ваншот");

    juce::StringArray rawTerms;
    rawTerms.addTokens(normalised, " \t\r\n", "");
    rawTerms.removeEmptyStrings();
    rawTerms.trim();

    for (const auto& term : rawTerms)
    {
        const auto compactTerm = compactSearchText(term);
        if (compactTerm.startsWith("bpm") || compactTerm.startsWith("tempo"))
        {
            const auto separator = term.indexOfChar(':');
            if (separator >= 0)
            {
                if (const auto range = parseBpmRange(term.substring(separator + 1)))
                {
                    filter.minBpm = range->first;
                    filter.maxBpm = range->second;
                    continue;
                }
            }
        }

        if (compactTerm.startsWith("key") || compactTerm.startsWith("tonality"))
        {
            const auto separator = term.indexOfChar(':');
            if (separator >= 0)
            {
                filter.keyQuery = compactSearchText(term.substring(separator + 1));
                if (filter.keyQuery.endsWith("minor"))
                    filter.keyQuery = filter.keyQuery.replace("minor", "min");
                else if (filter.keyQuery == "cm") filter.keyQuery = "cmin";
                else if (filter.keyQuery == "dm") filter.keyQuery = "dmin";
                else if (filter.keyQuery == "em") filter.keyQuery = "emin";
                else if (filter.keyQuery == "fm") filter.keyQuery = "fmin";
                else if (filter.keyQuery == "gm") filter.keyQuery = "gmin";
                else if (filter.keyQuery == "am") filter.keyQuery = "amin";
                else if (filter.keyQuery == "bm") filter.keyQuery = "bmin";
                if (filter.keyQuery.isNotEmpty())
                    continue;
            }
        }

        if (term.containsOnly("0123456789-"))
        {
            if (const auto range = parseBpmRange(term))
            {
                filter.minBpm = range->first;
                filter.maxBpm = range->second;
                continue;
            }
        }

        if (term == "loop" || term == "loops" || term == "луп" || term == "лупы" || term == "петля" || term == "петли")
            continue;

        if (term == "one" || term == "shot" || term == "shots" || term == "oneshot" || term == "oneshots" || term == "1shot"
            || term == "ван" || term == "шот" || term == "шоты" || term == "ваншот" || term == "ваншоты")
        {
            continue;
        }

        filter.terms.addIfNotAlreadyThere(term);
    }

    return filter;
}

bool matchesBrowserSearch(const orion::BrowserItem& item, const BrowserSearchFilter& filter)
{
    if (item.isParentLink)
        return true;

    if (filter.wantsLoop && ! isLoopItem(item))
        return false;

    if (filter.wantsOneShot && ! isOneShotItem(item))
        return false;

    const auto bpm = parseBpmFromFileName(item.file);
    if (filter.minBpm.has_value()
        && (bpm <= 0.0 || bpm < *filter.minBpm || bpm > *filter.maxBpm))
        return false;

    if (filter.keyQuery.isNotEmpty())
    {
        const auto key = parseKeyFromFileName(item.file);
        if (! key.has_value() || ! fuzzyTermMatches(compactSearchText(*key), filter.keyQuery))
            return false;
    }

    const auto searchable = (item.name + " " + item.subtitle + " " + item.category + " "
                             + item.tags.joinIntoString(" ") + " " + item.file.getFullPathName()).toLowerCase();
    for (const auto& term : filter.terms)
        if (! fuzzyTermMatches(searchable, term))
            return false;

    return true;
}

int browserSearchScore(const orion::BrowserItem& item, const BrowserSearchFilter& filter)
{
    const auto name = item.name.toLowerCase();
    const auto searchable = (name + " " + item.subtitle + " " + item.category + " "
                             + item.tags.joinIntoString(" ")).toLowerCase();
    int score = 0;
    for (const auto& term : filter.terms)
    {
        if (name == term) score += 1000;
        else if (name.startsWith(term)) score += 700;
        else if (name.contains(term)) score += 500;
        else if (searchable.contains(term)) score += 250;
        else score += 50;
    }
    if (filter.keyQuery.isNotEmpty()) score += 100;
    if (filter.minBpm.has_value()) score += 100;
    return score;
}

}  // namespace

namespace orion
{
BrowserPanelComponent::SwipeUnlockTimer::SwipeUnlockTimer(BrowserPanelComponent& ownerIn) noexcept
    : owner(ownerIn)
{
}

void BrowserPanelComponent::SwipeUnlockTimer::restart()
{
    startTimer(horizontalSwipeLockMs);
}

void BrowserPanelComponent::SwipeUnlockTimer::timerCallback()
{
    owner.unlockHorizontalSwipeGesture();
    stopTimer();
}

BrowserPanelComponent::BrowserPanelComponent()
{
    // The panel itself takes keyboard focus so Enter / arrows go to keyPressed() here, not to a
    // focused button (the "Add Folder" button used to grab Enter and pop the folder chooser).
    setWantsKeyboardFocus(true);

    chooseFolderButton.setWantsKeyboardFocus(false);   // legacy hidden control; folders are added from the left rail
    chooseFolderButton.setColour(juce::TextButton::buttonColourId, th::surface::elevated);
    chooseFolderButton.setColour(juce::TextButton::buttonOnColourId, th::surface::hover);
    chooseFolderButton.setColour(juce::TextButton::textColourOffId, th::text::secondary);
    chooseFolderButton.setColour(juce::TextButton::textColourOnId, th::text::primary);
    chooseFolderButton.setColour(juce::ComboBox::outlineColourId, buttonOutlineColour);
    chooseFolderButton.addListener(this);
    addAndMakeVisible(chooseFolderButton);
    chooseFolderButton.setVisible(false);

    // (Close button removed — toggling the browser is the toolbar's BROWSER button.)
    closeButton.setVisible(false);

    searchEditor.setTextToShowWhenEmpty("Search sounds, folders...", th::text::muted);
    // The rounded field is painted by BrowserPanelComponent so the search control shares the
    // same geometry as the browser pills instead of using the square native TextEditor surface.
    searchEditor.setColour(juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
    searchEditor.setColour(juce::TextEditor::textColourId, th::text::primary);
    searchEditor.setColour(juce::CaretComponent::caretColourId, th::text::primary);
    searchEditor.setColour(juce::TextEditor::highlightColourId, juce::Colours::white.withAlpha(0.20f));
    searchEditor.setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    searchEditor.setColour(juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
    searchEditor.setIndents(32, 0);
    searchEditor.setJustification(juce::Justification::centredLeft);
    searchEditor.setFont(browserFont(16.0f));
    searchEditor.setReturnKeyStartsNewLine(false);
    searchEditor.onTextChange = [this]
    {
        searchQuery = searchEditor.getText().trim();
        clearSearchButton.setVisible(searchQuery.isNotEmpty());
        if (onPreviewCleared && ! previewPeaks.empty())
            onPreviewCleared();
        clearPreview();
        if (inSimilarMode() && searchQuery.isNotEmpty())   // typing a query leaves similar mode
        {
            similarQuery.reset();
            similarQueryEmb.clear();
            similarDirty = false;
            entrySignature.clear();
        }
        refreshEntries();
    };
    addAndMakeVisible(searchEditor);

    const auto styleNavigationButton = [this](juce::TextButton& button)
    {
        button.setWantsKeyboardFocus(false);
        button.setLookAndFeel(&browserButtonLookAndFeel);
        button.setColour(juce::TextButton::textColourOffId, th::text::secondary);
        button.setColour(juce::TextButton::textColourOnId, th::text::primary);
        button.addListener(this);
        addAndMakeVisible(button);
    };
    styleNavigationButton(backButton);
    styleNavigationButton(forwardButton);
    styleNavigationButton(clearSearchButton);
    clearSearchButton.setVisible(false);
    backButton.setTooltip("Back");
    forwardButton.setTooltip("Forward");
    clearSearchButton.setTooltip("Clear search");

    const auto styleSectionButton = [this](juce::TextButton& button)
    {
        button.setClickingTogglesState(false);
        button.setWantsKeyboardFocus(false);   // never steal Enter from the list
        button.setLookAndFeel(&browserButtonLookAndFeel);
        button.setColour(juce::TextButton::buttonColourId, th::surface::elevated);
        button.setColour(juce::TextButton::buttonOnColourId, th::surface::hover);
        button.setColour(juce::TextButton::textColourOffId, th::text::secondary);
        button.setColour(juce::TextButton::textColourOnId, th::text::primary);
        button.addListener(this);
        addAndMakeVisible(button);
    };
    styleSectionButton(loopsSectionButton);
    styleSectionButton(oneShotsSectionButton);
    styleSectionButton(favoritesSectionButton);
    updateSectionButtons();

    loadFavorites();
    currentDirectory = getMacBrowseRoot();
    showLocationRoots(false);
    refreshEntries();
    updateNavigationButtons();
    startTimer(1200);
}

std::optional<BrowserItem> BrowserPanelComponent::getSelectedItem() const
{
    if (! selectedIndex.has_value())
        return std::nullopt;

    return items[static_cast<std::size_t>(*selectedIndex)];
}

void BrowserPanelComponent::chooseRootFolder()
{
    const auto startDirectory = showingLocationRoots ? getMacBrowseRoot() : currentDirectory;
    folderChooser = std::make_unique<juce::FileChooser>("Choose sample folder", startDirectory, juce::String());
    folderChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                               [this](const juce::FileChooser& chooser)
                               {
                                   const auto selected = chooser.getResult();
                                   if (selected.isDirectory())
                                   {
                                       if (onRootFolderChosen)
                                           onRootFolderChosen(selected);
                                       else
                                           openFolder(selected);
                                   }

                                   folderChooser.reset();
                               });
}

void BrowserPanelComponent::openFolder(const juce::File& directory)
{
    if (directory.isDirectory())
        navigateTo(directory, true, isCustomLibraryRoot(directory));
}

void BrowserPanelComponent::showRootLocations()
{
    showLocationRoots(true);
}

void BrowserPanelComponent::paint(juce::Graphics& g)
{
    // Keep the browser list readable against the darker transport and rail surfaces.
    g.fillAll(th::core::studio);
    auto bounds = getLocalBounds().reduced(contentPadX, 0);

    auto header = bounds.removeFromTop(headerHeight);
    g.setColour(th::core::studio);
    g.fillRect(header);
    g.setColour(th::line::subtle);
    g.fillRect(header.removeFromBottom(1));

    auto titleArea = header.removeFromTop(40);
    g.setColour(th::text::primary);
    g.setFont(browserFont(22.0f, juce::Font::bold));
    g.drawText("Browser", titleArea.withRight(titleArea.getRight() - 72), juce::Justification::centredLeft);

    auto locationArea = header.removeFromTop(24).reduced(2, 0);
    g.setColour(th::text::tertiary);
    g.setFont(browserFont(13.0f));
    g.drawText(getLocationDisplayName(), locationArea, juce::Justification::centredLeft, true);

    // The current folder is intentionally omitted here. The list already communicates the
    // location, while a path under the title made the browser header feel like a debug readout.

    // Search field background and icon are painted here; the transparent TextEditor above it
    // provides text editing while this keeps the field visually native to Orion.
    const auto searchBounds = searchEditor.getBounds().toFloat();
    g.setColour(th::surface::elevated.withAlpha(0.82f));
    g.fillRoundedRectangle(searchBounds, th::metrics::controlRadius);
    g.setColour(th::line::normal.withAlpha(0.82f));
    g.drawRoundedRectangle(searchBounds.reduced(0.5f), th::metrics::controlRadius, 1.0f);
    const auto searchIcon = searchBounds.withX(searchBounds.getX() + 11.0f)
                                       .withY(searchBounds.getCentreY() - 6.0f)
                                       .withSize(12.0f, 12.0f);
    g.setColour(th::text::tertiary);
    g.drawEllipse(searchIcon.reduced(1.5f), 1.35f);
    g.drawLine(searchIcon.getRight() - 2.0f, searchIcon.getBottom() - 2.0f,
               searchIcon.getRight() + 2.0f, searchIcon.getBottom() + 2.0f, 1.35f);

    const auto filterGroup = loopsSectionButton.getBounds().getUnion(oneShotsSectionButton.getBounds()).toFloat()
                                     .expanded(4.0f, 3.0f);
    g.setColour(th::core::voidBlack.withAlpha(0.34f));
    g.fillRoundedRectangle(filterGroup, th::metrics::controlRadius + 1.0f);
    g.setColour(th::line::subtle.withAlpha(0.68f));
    g.drawRoundedRectangle(filterGroup.reduced(0.5f), th::metrics::controlRadius + 1.0f, 1.0f);

    g.setColour(th::line::subtle.withAlpha(0.72f));
    g.fillRect(header.getX(), header.getBottom() - 1, header.getWidth(), 1);

    const auto listViewport = getListViewportBounds();
    g.saveState();
    g.reduceClipRegion(listViewport);

    for (int index = 0; index < static_cast<int>(items.size()); ++index)
    {
        auto row = getRowBounds(index);
        if (row.isEmpty())
            continue;
        const auto rowFull = row;   // unmutated rect, for the favorite star on the right edge
        // Cull rows outside the visible list area. Without this every row (incl. hundreds of
        // off-screen ones in a big folder) ran filename parsing + text layout on every repaint,
        // which froze the UI briefly on each selection/scroll.
        if (row.getBottom() <= listViewport.getY() || row.getY() >= listViewport.getBottom())
            continue;

        const auto selected = selectedIndex.has_value() && *selectedIndex == index;
        const auto hovered = hoverIndex.has_value() && *hoverIndex == index;
        // Lazy "by sound" analysis: only for rows actually drawn, so a big folder analyses just what
        // you look at (deduped/cached in AudioTagger). Skipped while actively scrolling (debounce) so the
        // heavy ML doesn't spike the CPU mid-flick — it runs once the list settles.
        if (! items[static_cast<std::size_t>(index)].soundTagsRequested
            && juce::Time::getMillisecondCounterHiRes() - lastScrollMs > 260.0)
            requestSoundTags(index);
        const auto& item = items[static_cast<std::size_t>(index)];

        // Row card. Selected rows get a faint tint in the entry's own colour so the
        // selection reads as "this one" rather than a flat grey.
        g.setColour(selected ? item.colour.withAlpha(0.20f) : (hovered ? rowHover : rowBackground));
        g.fillRoundedRectangle(row.toFloat(), rowCornerRadius);
        if (selected)
        {
            g.setColour(th::accent::infoBlue.withAlpha(0.92f));
            g.drawRoundedRectangle(row.toFloat().reduced(0.5f), rowCornerRadius, 1.2f);
            g.fillRoundedRectangle(row.removeFromLeft(3.0f).toFloat(), 1.5f);
        }

        // Icon box on the left: tinted rounded square + folder/waveform/up-arrow glyph.
        auto iconBox = row.removeFromLeft(rowHeight).toFloat().reduced(9.0f);
        g.setColour(item.colour.withAlpha(0.20f));
        g.fillRoundedRectangle(iconBox, th::metrics::controlRadius);
        g.setColour(item.colour.brighter(0.45f).withAlpha(0.95f));
        drawBrowserEntryIcon(g, iconBox.reduced(5.0f), item.isDirectory, item.isParentLink);
        row.removeFromLeft(10);

        g.setColour(juce::Colours::white.withAlpha(0.9f));
        g.setFont(browserFont(17.0f, selected ? juce::Font::bold : juce::Font::plain));
        g.drawText(item.name, row.removeFromTop(19), juce::Justification::centredLeft, true);

        if (! item.isDirectory)
        {
            const auto bpm = parseBpmFromFileName(item.file);
            const auto key = parseKeyFromFileName(item.file);
            const auto roundedBpm = static_cast<int>(std::round(bpm));

            if (roundedBpm > 0)
            {
                auto badge = row.removeFromLeft(66).withHeight(17);
                drawBadge(g, badge, juce::String(roundedBpm) + " BPM", bpmBadgeColour);
                row.removeFromLeft(5);
            }

            if (key.has_value())
            {
                auto badge = row.removeFromLeft(56).withHeight(17);
                drawBadge(g, badge, *key, keyBadgeColour);
                row.removeFromLeft(7);
            }
        }

        g.setColour(mutedText);
        g.setFont(browserFont(15.0f));
        g.drawText(item.subtitle, row.withTrimmedRight(item.isDirectory ? 0 : 34), juce::Justification::centredLeft, true);

        // Favorite heart on the right edge (files only): the classic heart glyph — filled red ♥ when
        // favorited, outline ♡ otherwise.
        if (! item.isDirectory && ! item.isParentLink)
        {
            const auto b = favoriteStarBounds(rowFull);
            const bool fav = isFavorite(item.file);
            g.setColour(fav ? juce::Colour(0xffe60012)   // Uniqlo red
                            : juce::Colours::white.withAlpha(hovered || selected ? 0.42f : 0.22f));
            g.setFont(juce::Font(static_cast<float>(b.getHeight())));
            g.drawText(juce::String::fromUTF8(fav ? "\xe2\x99\xa5" : "\xe2\x99\xa1"),
                       b, juce::Justification::centred);
        }
    }

    if (items.empty() && searchQuery.isNotEmpty() && ! recursiveScanPending)
    {
        auto emptyState = listViewport.reduced(18, 20);
        g.setColour(th::text::secondary.withAlpha(0.88f));
        g.setFont(browserFont(16.0f, true));
        g.drawText("No sounds found", emptyState.removeFromTop(24),
                   juce::Justification::centred, true);
        g.setColour(th::text::tertiary.withAlpha(0.82f));
        g.setFont(browserFont(13.0f));
        g.drawText("Try another name, tag, or folder", emptyState.removeFromTop(22),
                   juce::Justification::centred, true);
    }

    g.restoreState();

    paintTagsRow(g);
    paintPreviewBar(g);
}

void BrowserPanelComponent::paintTagsRow(juce::Graphics& g)
{
    const auto area = getTagsRowBounds();
    auto row = area.reduced(2, 4);

    g.setColour(th::text::secondary.withAlpha(0.72f));
    g.setFont(browserFont(14.0f, juce::Font::bold));
    g.drawText("Tags", row.removeFromLeft(38), juce::Justification::centredLeft);

    const orion::BrowserItem* sel = nullptr;
    if (selectedIndex.has_value() && *selectedIndex >= 0 && *selectedIndex < static_cast<int>(items.size()))
    {
        const auto& it = items[static_cast<std::size_t>(*selectedIndex)];
        if (! it.isDirectory && ! it.isParentLink)
            sel = &it;
    }

    if (sel == nullptr || sel->tags.isEmpty())
        return;   // nothing to show yet — just the label, no placeholder text

    // Chips for each tag of the selected sample.
    g.setFont(browserFont(14.0f, juce::Font::bold));
    int x = row.getX();
    for (const auto& tag : sel->tags)
    {
        const int chipW = tag.length() * 7 + 16;
        if (x + chipW > row.getRight())
            break;
        juce::Rectangle<int> chip(x, row.getY(), chipW, row.getHeight());
        g.setColour(th::cool::turquoise.withAlpha(0.18f));
        g.fillRoundedRectangle(chip.toFloat(), th::metrics::controlRadius);
        g.setColour(th::cool::turquoise.withAlpha(0.35f));
        g.drawRoundedRectangle(chip.toFloat().reduced(0.5f), th::metrics::controlRadius, 1.0f);
        g.setColour(juce::Colours::white.withAlpha(0.88f));
        g.drawText(tag, chip, juce::Justification::centred);
        x += chipW + 5;
    }
}

void BrowserPanelComponent::paintPreviewBar(juce::Graphics& g)
{
    // A clear border line right above the preview: the list clips just above it, so a scrolled
    // sample row ends on this line instead of appearing to slide under the preview card.
    {
        auto border = getLocalBounds().removeFromBottom(previewBarHeight + previewSyncRowHeight + 3);
        g.setColour(th::line::normal);
        g.fillRect(border.removeFromTop(2));
    }

    const auto bar = getPreviewBarBounds();
    // Preview gets a restrained slate-blue utility accent: distinct from selection cyan,
    // but quiet enough to remain part of the browser surface.
    const auto accent = th::accent::previewSlate;

    // Keep the preview as a quiet utility surface. The selected waveform and play state carry
    // the emphasis; the container itself should not compete with the browser list.
    const auto cardF = bar.toFloat().reduced(2.0f);
    const auto cardR = th::metrics::controlRadius;
    {
        g.setColour(th::surface::elevated);
        g.fillRoundedRectangle(cardF, cardR);

        g.setColour(th::line::normal.withAlpha(0.82f));
        g.drawRoundedRectangle(cardF.reduced(0.5f), cardR, 1.0f);
    }

    // Independent BPM and key sync pills. BPM uses the existing preview warp; key sync adds
    // a pitch-preserving Rubber Band transpose to that same stream.
    const auto paintSyncButton = [&g, &accent](juce::Rectangle<int> button, bool enabled,
                                                const juce::String& onText, const juce::String& offText)
    {
        g.setColour(enabled ? accent.withAlpha(0.85f) : juce::Colours::white.withAlpha(0.06f));
        g.fillRoundedRectangle(button.toFloat(), th::metrics::controlRadius);
        g.setColour(enabled ? accent.withAlpha(0.9f) : juce::Colours::white.withAlpha(0.16f));
        g.drawRoundedRectangle(button.toFloat().reduced(0.5f), th::metrics::controlRadius, 1.0f);
        g.setColour(enabled ? juce::Colour(0xff10141a) : juce::Colours::white.withAlpha(0.72f));
        g.setFont(browserFont(12.0f, juce::Font::bold));
        g.drawText(enabled ? onText : offText, button, juce::Justification::centred);
    };
    paintSyncButton(getPreviewSyncButtonBounds(), previewBpmSync, "BPM SYNC  -  ON", "BPM SYNC  -  OFF");
    paintSyncButton(getPreviewKeySyncButtonBounds(), previewKeySync, "KEY SYNC  -  ON", "KEY SYNC  -  OFF");

    // Play / stop button.
    const auto btn = getPreviewPlayButtonBounds();
    if (previewArmed)
    {
        // Quantized & waiting for the next bar: pulse the button so it reads as "queued".
        const auto phase = std::sin(static_cast<float>(juce::Time::getMillisecondCounter() % 600) / 600.0f * juce::MathConstants<float>::twoPi);
        const auto pulse = 0.45f + 0.45f * (0.5f + 0.5f * phase);
        g.setColour(accent.withAlpha(pulse));
    }
    else
    {
        g.setColour(accent.withAlpha(previewPeaks.empty() ? 0.25f : 0.9f));
    }
        g.fillRoundedRectangle(btn.toFloat(), th::metrics::controlRadius);
    g.setColour(th::core::voidBlack);
    const auto gi = btn.toFloat().reduced(btn.getWidth() * 0.3f, btn.getHeight() * 0.26f);
    if (previewPlaying)
    {
        // Stop square.
        g.fillRoundedRectangle(btn.toFloat().reduced(btn.getWidth() * 0.32f), 2.0f);
    }
    else
    {
        // Play triangle.
        juce::Path tri;
        tri.startNewSubPath(gi.getX(), gi.getY());
        tri.lineTo(gi.getRight(), gi.getCentreY());
        tri.lineTo(gi.getX(), gi.getBottom());
        tri.closeSubPath();
        g.fillPath(tri);
    }

    // Waveform area.
    const auto wave = getPreviewWaveformBounds();
    if (previewPeaks.empty())
    {
        g.setColour(juce::Colours::white.withAlpha(0.62f));
        g.setFont(browserFont(15.0f));
        g.drawText("Select a sample to preview", wave, juce::Justification::centred);
        return;
    }

    // Name (top of the waveform area).
    auto waveBox = wave;
    auto nameRow = waveBox.removeFromTop(15);
    g.setColour(juce::Colours::white.withAlpha(0.80f));
    g.setFont(browserFont(14.0f, juce::Font::bold));
    g.drawText(previewName, nameRow, juce::Justification::centredLeft, true);

    // Waveform: vertical mirrored bars.
    const auto n = static_cast<int>(previewPeaks.size());
    const auto midY = waveBox.getCentreY();
    const auto halfH = waveBox.getHeight() * 0.5f - 1.0f;
    const auto playedX = waveBox.getX() + previewPositionRatio * waveBox.getWidth();
    for (int x = 0; x < waveBox.getWidth(); ++x)
    {
        const auto idx = juce::jlimit(0, n - 1, static_cast<int>(static_cast<float>(x) / waveBox.getWidth() * n));
        const auto h = juce::jmax(1.0f, previewPeaks[static_cast<std::size_t>(idx)] * halfH);
        const auto px = waveBox.getX() + x;
        // Make playback progress readable at a glance: the completed waveform is vivid,
        // while the upcoming portion recedes into the preview surface.
        g.setColour(px <= playedX ? accent.brighter(0.42f).withAlpha(1.0f)
                                  : th::text::muted.withAlpha(0.48f));
        g.fillRect(static_cast<float>(px), midY - h, 1.0f, h * 2.0f);
    }

    // Playhead line while playing.
    if (previewPlaying)
    {
        g.setColour(accent.brighter(0.55f).withAlpha(0.98f));
        g.fillRect(playedX - 1.0f, static_cast<float>(waveBox.getY()), 2.0f,
                   static_cast<float>(waveBox.getHeight()));
    }
}

void BrowserPanelComponent::setBrowserSection(BrowserSection section)
{
    if (browserSection == section)
        return;

    browserSection = section;
    updateSectionButtons();
    refreshEntries();
}

// ---- Favorites -----------------------------------------------------------------------------

juce::File BrowserPanelComponent::favoritesFile() const
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
               .getChildFile("Orion").getChildFile("favorites.txt");
}

void BrowserPanelComponent::loadFavorites()
{
    favoritesSet.clear();
    const auto f = favoritesFile();
    if (! f.existsAsFile())
        return;
    juce::StringArray lines;
    f.readLines(lines);
    for (const auto& line : lines)
        if (line.trim().isNotEmpty())
            favoritesSet.insert(line.trim());
}

void BrowserPanelComponent::saveFavorites() const
{
    const auto f = favoritesFile();
    f.getParentDirectory().createDirectory();
    juce::StringArray lines;
    for (const auto& p : favoritesSet)
        lines.add(p);
    f.replaceWithText(lines.joinIntoString("\n"));
}

bool BrowserPanelComponent::isFavorite(const juce::File& file) const
{
    return favoritesSet.count(file.getFullPathName()) > 0;
}

void BrowserPanelComponent::toggleFavorite(const juce::File& file)
{
    if (file == juce::File())
        return;
    const auto key = file.getFullPathName();
    if (favoritesSet.count(key))
        favoritesSet.erase(key);
    else
        favoritesSet.insert(key);
    saveFavorites();
    if (browserSection == BrowserSection::favorites)   // the list itself changed
        refreshEntries();
    repaint();
}

juce::Rectangle<int> BrowserPanelComponent::favoriteStarBounds(juce::Rectangle<int> row) const
{
    // A small square hit zone on the right edge of the row (the heart is drawn inside it).
    constexpr int d = 16;
    return juce::Rectangle<int>(row.getRight() - 26, row.getY() + (row.getHeight() - d) / 2, d, d);
}

void BrowserPanelComponent::updateSectionButtons()
{
    loopsSectionButton.setToggleState(browserSection == BrowserSection::loops, juce::dontSendNotification);
    oneShotsSectionButton.setToggleState(browserSection == BrowserSection::oneShots, juce::dontSendNotification);
    favoritesSectionButton.setToggleState(browserSection == BrowserSection::favorites, juce::dontSendNotification);
}

void BrowserPanelComponent::resized()
{
    auto bounds = getLocalBounds().reduced(contentPadX, 0);

    // Row 1: title row. Folder roots are managed from the left navigation rail.
    auto titleRow = bounds.removeFromTop(40);
    closeButton.setBounds({});
    chooseFolderButton.setBounds({});
    auto navArea = titleRow.removeFromRight(68).reduced(0, 5);
    backButton.setBounds(navArea.removeFromLeft(30));
    navArea.removeFromLeft(4);
    forwardButton.setBounds(navArea.removeFromLeft(30));

    bounds.removeFromTop(24); // current location row, painted by the browser
    bounds.removeFromTop(browserSectionGap * 2);  // breathing room below the location label
    auto searchRow = bounds.removeFromTop(th::metrics::controlHeight);
    clearSearchButton.setBounds(searchRow.removeFromRight(28).reduced(2, 2));
    searchEditor.setBounds(searchRow);

    bounds.removeFromTop(browserSectionGap); // clear separation between search and filters
    auto sectionRow = bounds.removeFromTop(th::metrics::controlHeight);
    constexpr int pillW = 84;
    loopsSectionButton.setBounds(sectionRow.removeFromLeft(pillW));
    sectionRow.removeFromLeft(browserSectionGap);
    oneShotsSectionButton.setBounds(sectionRow.removeFromLeft(pillW));
    sectionRow.removeFromLeft(browserSectionGap);
    favoritesSectionButton.setBounds(sectionRow.removeFromLeft(pillW + 20));
    updateNavigationButtons();
}

void BrowserPanelComponent::mouseDown(const juce::MouseEvent& event)
{
    // Independent sync toggles (below the preview card).
    if (getPreviewSyncButtonBounds().contains(event.getPosition()))
    {
        previewBpmSync = !previewBpmSync;
        repaint(getPreviewSyncButtonBounds());
        if (onPreviewBpmSyncToggled)
            onPreviewBpmSyncToggled();
        return;
    }
    if (getPreviewKeySyncButtonBounds().contains(event.getPosition()))
    {
        previewKeySync = !previewKeySync;
        repaint(getPreviewKeySyncButtonBounds());
        if (onPreviewKeySyncToggled)
            onPreviewKeySyncToggled();
        return;
    }

    // Preview card play/stop button.
    if (getPreviewBarBounds().contains(event.getPosition()))
    {
        if (getPreviewWaveformBounds().contains(event.getPosition()) && onSeekPreview && ! previewPeaks.empty())
        {
            const auto wave = getPreviewWaveformBounds();
            const auto ratio = juce::jlimit(0.0f, 1.0f,
                                            static_cast<float>(event.position.x - wave.getX())
                                                / static_cast<float>(juce::jmax(1, wave.getWidth())));
            onSeekPreview(ratio);
            return;
        }

        if (getPreviewPlayButtonBounds().contains(event.getPosition()) && onTogglePreviewPlayback)
            onTogglePreviewPlayback();
        return;
    }

    // Tags are display-only for now. Consume clicks in this strip so they cannot fall through
    // to the list row underneath and accidentally trigger a sample preview.
    if (getTagsRowBounds().contains(event.getPosition()))
        return;

    // Right-click (or ctrl-click) a row → context menu.
    if (event.mods.isPopupMenu())
    {
        const auto idx = hitTestRow(event.getPosition());
        if (! idx.has_value())
            return;
        selectedIndex = idx;
        repaint();
        const auto& item = items[static_cast<std::size_t>(*idx)];
        if (item.isParentLink)
            return;

        juce::PopupMenu menu;
        orion::ui::stylePopupMenu(menu);
        if (! item.isDirectory)
        {
            menu.addItem(6, isFavorite(item.file) ? "Remove from Favorites" : "Add to Favorites");
            menu.addSeparator();
            menu.addItem(3, "Add to playlist");                  // sampler track + clip
            menu.addItem(2, "Open in sampler");                  // sampler track only
            menu.addItem(4, "Replace selected track's sample");
            menu.addItem(5, "Find similar sounds");              // timbral similarity ranking
            menu.addSeparator();
        }
        menu.addItem(1, "Open in Finder");
        const auto file = item.file;
        const auto activated = item;
        const auto screenPos = event.getScreenPosition();
        juce::Component::SafePointer<BrowserPanelComponent> safeThis(this);
        menu.showMenuAsync(juce::PopupMenu::Options()
                               .withTargetScreenArea(juce::Rectangle<int>(screenPos.x, screenPos.y, 1, 1)),
                           [safeThis, file, activated](int result)
                           {
                               if (safeThis == nullptr)
                                   return;
                               if (result == 1 && file.exists())
                                   file.revealToUser();   // reveal/select the file or folder in Finder
                               else if (result == 2 && safeThis->onOpenInSampler)
                                   safeThis->onOpenInSampler(activated);   // sampler track only, no playlist clip
                               else if (result == 3 && safeThis->onAddItemToPlaylist)
                                   safeThis->onAddItemToPlaylist(activated);   // sampler track + clip
                               else if (result == 4 && safeThis->onReplaceSelectedTrackSample)
                                   safeThis->onReplaceSelectedTrackSample(activated);
                               else if (result == 5)
                                   safeThis->enterSimilarMode(file);
                               else if (result == 6)
                                   safeThis->toggleFavorite(file);
                           });
        return;
    }

    // Clicking the favorite star toggles it (files only) — never selects/auditions the row.
    if (const auto starIdx = hitTestRow(event.getPosition()); starIdx.has_value())
    {
        const auto& it = items[static_cast<std::size_t>(*starIdx)];
        if (! it.isDirectory && ! it.isParentLink
            && favoriteStarBounds(getRowBounds(*starIdx)).contains(event.getPosition()))
        {
            toggleFavorite(it.file);
            return;
        }
    }

    dragIndex = hitTestRow(event.getPosition());
    selectedIndex = dragIndex;
    repaint();

    if (! selectedIndex.has_value())
        return;

    // Row 0 in similar mode is the "◂ Similar to …" chip → click exits.
    if (inSimilarMode() && *selectedIndex == 0)
    {
        exitSimilarMode();
        return;
    }

    grabKeyboardFocus();   // so Enter/arrows act on the browser list, not a focused button

    const auto& item = items[static_cast<std::size_t>(*selectedIndex)];
    if (item.isDirectory)
    {
        openDirectoryItem(item);
        return;
    }

    if (onPreviewItem)
        onPreviewItem(item);
}

void BrowserPanelComponent::openDirectoryItem(const BrowserItem& item)
{
    if (onPreviewCleared && ! previewPeaks.empty())
        onPreviewCleared();
    clearPreview();

    // Navigating away leaves similar mode (the following refresh rebuilds a normal listing).
    similarQuery.reset();
    similarQueryEmb.clear();
    similarDirty = false;

    if (showingLocationRoots && item.name == "Macintosh HD")
    {
        pushCurrentLocationToBackHistory();
        forwardHistory.clear();
        showingLocationRoots = false;
        showingMacRootOverview = true;
        showingUserHomeOverview = false;
        customRootActive = false;
        currentDirectory = getMacBrowseRoot();
        refreshEntries();
        updateNavigationButtons();
        return;
    }

    if (item.isParentLink)
    {
        const auto parentDirectory = currentDirectory.getParentDirectory();
        if (showingUserHomeOverview)
        {
            navigateTo(juce::File("/Users"));
        }
        else if (showingMacRootOverview
            || currentDirectory == juce::File("/Users")
            || currentDirectory == juce::File("/Applications")
            || currentDirectory == juce::File("/Library")
            || currentDirectory == juce::File("/System")
            || currentDirectory == juce::File("/Volumes")
            || parentDirectory == currentDirectory
            || ! parentDirectory.isDirectory())
        {
            showLocationRoots();
        }
        else if (item.file.isDirectory())
        {
            // Parent rows carry the resolved destination. Use it directly instead of
            // recalculating from a virtual browser state or a symlinked location.
            navigateTo(item.file);
        }
        else
            navigateTo(parentDirectory);
    }
    else
        navigateTo(item.file);
}

void BrowserPanelComponent::mouseDoubleClick(const juce::MouseEvent& event)
{
    const auto clickedIndex = hitTestRow(event.getPosition());
    if (! clickedIndex.has_value())
        return;

    selectedIndex = clickedIndex;
    repaint();

    const auto& item = items[static_cast<std::size_t>(*selectedIndex)];
    if (item.isDirectory)
        return;

    if (onActivateItem)
        onActivateItem(item);
}

void BrowserPanelComponent::mouseDrag(const juce::MouseEvent& event)
{
    if (! dragIndex.has_value())
        return;

    const auto& item = items[static_cast<std::size_t>(*dragIndex)];
    if (item.isDirectory)
        return;

    if ((event.getDistanceFromDragStartX() * event.getDistanceFromDragStartX())
            + (event.getDistanceFromDragStartY() * event.getDistanceFromDragStartY())
        < dragThresholdPx * dragThresholdPx)
    {
        return;
    }

    auto* dragContainer = juce::DragAndDropContainer::findParentDragContainerFor(this);
    if (dragContainer == nullptr)
        return;

    auto payload = juce::var(new juce::DynamicObject());
    auto* payloadObject = payload.getDynamicObject();
    payloadObject->setProperty("type", "browser-item");
    payloadObject->setProperty("name", item.name);
    payloadObject->setProperty("category", item.category);
    payloadObject->setProperty("subtitle", item.subtitle);
    payloadObject->setProperty("colour", static_cast<int>(item.colour.getARGB()));
    payloadObject->setProperty("lengthBeats", item.defaultClipLengthInBeats);
    payloadObject->setProperty("path", item.file.getFullPathName());

    auto dragPeaks = readDragWaveform(item.file, 220);
    if (dragPeaks.empty() && previewName == item.name)
        dragPeaks = previewPeaks;
    auto dragImage = makeDragClip(item, dragPeaks);

    // Stop the browser preview the moment a drag begins — the sample shouldn't keep playing
    // while it's being dragged onto the playlist.
    if (onDragStarted)
        onDragStarted();

    dragVisualContainer = dragContainer;
    dragVisualItem = item;
    dragVisualPeaks = std::move(dragPeaks);
    startTimer(1200);
    dragContainer->startDragging(payload, this, juce::ScaledImage(dragImage), true, nullptr, &event.source);
    dragIndex.reset();
}

void BrowserPanelComponent::mouseMove(const juce::MouseEvent& event)
{
    hoverIndex = hitTestRow(event.getPosition());
    repaint();
}

void BrowserPanelComponent::mouseExit(const juce::MouseEvent&)
{
    hoverIndex.reset();
    repaint();
}

void BrowserPanelComponent::mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
{
    if (! getListViewportBounds().contains(event.getPosition()))
        return;

    const auto absDeltaX = std::abs(wheel.deltaX);
    const auto absDeltaY = std::abs(wheel.deltaY);
    if (absDeltaX > absDeltaY * 1.2f && absDeltaX > 0.0001f)
    {
        const auto now = juce::Time::getMillisecondCounter();
        if (horizontalSwipeLocked)
            return;

        if (wheel.isInertial)
        {
            lastHorizontalWheelMs = now;
            return;
        }

        lastHorizontalWheelMs = now;
        horizontalWheelAccumulator += wheel.deltaX;

        if (horizontalWheelAccumulator <= -horizontalSwipeThreshold)
        {
            goForwardInBrowserHistory();
            horizontalWheelAccumulator = 0.0f;
            horizontalSwipeLocked = true;
            lastHorizontalWheelMs = now;
            swipeUnlockTimer.restart();
            return;
        }

        if (horizontalWheelAccumulator >= horizontalSwipeThreshold)
        {
            goBackInBrowserHistory();
            horizontalWheelAccumulator = 0.0f;
            horizontalSwipeLocked = true;
            lastHorizontalWheelMs = now;
            swipeUnlockTimer.restart();
            return;
        }

        return;
    }

    if (absDeltaY >= absDeltaX)
    {
        horizontalWheelAccumulator = 0.0f;
    }

    if (std::abs(wheel.deltaY) < 0.0001f)
        return;

    const auto pixelsPerWheelUnit = wheel.isSmooth ? 620.0 : 150.0;
    const auto speedGain = wheel.isSmooth
        ? juce::jlimit(1.0, 2.4, 1.0 + static_cast<double>(std::abs(wheel.deltaY)) * 0.55)
        : 1.0;
    scrollOffsetY -= static_cast<double>(wheel.deltaY) * pixelsPerWheelUnit * speedGain;
    clampScrollOffset();
    hoverIndex = hitTestRow(event.getPosition());
    lastScrollMs = juce::Time::getMillisecondCounterHiRes();
    // Once scrolling settles, repaint so the debounced ML tagging kicks in for the now-visible rows.
    juce::Component::SafePointer<BrowserPanelComponent> safe(this);
    juce::Timer::callAfterDelay(300, [safe]() mutable { if (safe != nullptr) safe->repaint(); });
    repaint();
}

bool BrowserPanelComponent::keyPressed(const juce::KeyPress& key)
{
    const auto count = static_cast<int>(items.size());

    // Up / Down: move the selection through files AND folders. Audio files auto-audition
    // (Ableton-style); folders just highlight. The selected row is scrolled into view.
    if ((key.getKeyCode() == juce::KeyPress::upKey || key.getKeyCode() == juce::KeyPress::downKey) && count > 0)
    {
        const int dir = key.getKeyCode() == juce::KeyPress::downKey ? 1 : -1;
        int next = selectedIndex.has_value() ? juce::jlimit(0, count - 1, *selectedIndex + dir)
                                             : (dir > 0 ? 0 : count - 1);
        selectedIndex = next;

        // Scroll the row fully into the viewport.
        const auto viewport = getListViewportBounds();
        const auto rowTop = next * (rowHeight + rowGap);
        if (rowTop < scrollOffsetY)
            scrollOffsetY = rowTop;
        else if (rowTop + rowHeight > scrollOffsetY + viewport.getHeight())
            scrollOffsetY = rowTop + rowHeight - viewport.getHeight();
        clampScrollOffset();

        const auto& item = items[static_cast<std::size_t>(next)];
        if (! item.isDirectory && onPreviewItem)
            onPreviewItem(item);
        repaint();
        return true;
    }

    // Left: go up one level (into the parent folder). Right: enter the selected folder.
    if (key.getKeyCode() == juce::KeyPress::leftKey)
    {
        for (const auto& it : items)
            if (it.isParentLink)
            {
                const auto parent = it;        // copy — openDirectoryItem refreshes items
                openDirectoryItem(parent);
                return true;
            }
        goBackInBrowserHistory();              // at the locations root: fall back to history
        return true;
    }
    if (key.getKeyCode() == juce::KeyPress::rightKey)
    {
        if (selectedIndex.has_value())
        {
            const auto it = items[static_cast<std::size_t>(*selectedIndex)];   // copy
            if (it.isDirectory)
                openDirectoryItem(it);
        }
        return true;
    }

    if (key.getKeyCode() != juce::KeyPress::returnKey)
        return false;

    if (! selectedIndex.has_value())
        return false;

    const auto& item = items[static_cast<std::size_t>(*selectedIndex)];
    // Enter on a folder does NOT navigate into it — use → or double-click for that. We still consume
    // the key so it never leaks to the transport (which would reset playback to the start).
    if (item.isDirectory)
        return true;

    // Enter on a sound adds a SAMPLER TRACK (and opens its UI) — no audio clip in the playlist.
    if (onOpenInSampler)
        onOpenInSampler(item);
    return true;
}

void BrowserPanelComponent::buttonClicked(juce::Button* button)
{
    if (button == &chooseFolderButton)
        chooseRootFolder();
    else if (button == &backButton)
        goBackInBrowserHistory();
    else if (button == &forwardButton)
        goForwardInBrowserHistory();
    else if (button == &clearSearchButton)
        clearSearch();
    else if (button == &loopsSectionButton)
        setBrowserSection(browserSection == BrowserSection::loops ? BrowserSection::all : BrowserSection::loops);
    else if (button == &oneShotsSectionButton)
        setBrowserSection(browserSection == BrowserSection::oneShots ? BrowserSection::all : BrowserSection::oneShots);
    else if (button == &favoritesSectionButton)
        setBrowserSection(browserSection == BrowserSection::favorites ? BrowserSection::all : BrowserSection::favorites);
    else if (button == &closeButton)
    {
        if (onCloseRequested)
            onCloseRequested();
    }
}

void BrowserPanelComponent::timerCallback()
{
    if (! isShowing())
        return;

    if (dragVisualContainer != nullptr)
    {
        if (! dragVisualContainer->isDragAndDropActive() || ! dragVisualItem.has_value())
        {
            dragVisualContainer = nullptr;
            dragVisualItem.reset();
            dragVisualPeaks.clear();
            startTimer(1200);
        }
        else
        {
            // The drag image is intentionally static: the complete clip follows the cursor
            // through JUCE's drag container without a second animation layer.
            startTimer(1200);
        }
    }

    if (similarDirty && ! similarRanked)   // rank once when the pool + query embedding are ready, then freeze
        rebuildSimilarItems();

    const auto currentTimestamp = getWatchedLocationTimestamp();
    if (currentTimestamp != watchedLocationTimestamp)
    {
        recursiveScanValid = false;   // folder changed on disk → rebuild the recursive cache
        refreshEntries();
    }
}

void BrowserPanelComponent::refreshEntries()
{
    std::vector<BrowserItem> refreshedItems;
    const bool searching = searchQuery.trim().isNotEmpty();

    // Favorites: a flat, cross-folder listing of the starred sounds (a search narrows it further).
    if (browserSection == BrowserSection::favorites && ! inSimilarMode())
    {
        const auto filter = parseBrowserSearchFilter(searchQuery);
        items.clear();
        for (const auto& p : favoritesSet)
        {
            const juce::File file(p);
            if (! file.existsAsFile() || ! isAudioFile(file))
                continue;
            BrowserItem it { file.getFileName(), file.getParentDirectory().getFileName(),
                             subtitleForFile(file), colourForEntry(file, false),
                             defaultLengthForFile(file), file, false, false };
            it.tags = deriveTags(file);
            if (searching && ! matchesBrowserSearch(it, filter))
                continue;
            items.push_back(std::move(it));
        }
        std::sort(items.begin(), items.end(),
                  [](const BrowserItem& a, const BrowserItem& b) { return a.name.compareNatural(b.name) < 0; });
        if (selectedIndex.has_value() && *selectedIndex >= static_cast<int>(items.size()))
            selectedIndex.reset();
        entrySignature = "@favorites=" + juce::String(static_cast<int>(items.size())) + "@q=" + searchQuery;
        clampScrollOffset();
        repaint();
        return;
    }

    // Global search (Ableton-style): a query searches the WHOLE library, independent of the current
    // view (works even at the locations overview). Handled here with an early return so it doesn't
    // depend on which folder branch would otherwise run.
    if (searching && ! inSimilarMode())
    {
        // Scope = the user's added library folders. If none are set, search only the current folder —
        // NEVER the whole disk (scanning all of Macintosh HD would be ruinous).
        std::vector<juce::File> roots = libraryRoots;
        if (roots.empty() && currentDirectory.isDirectory())
            roots.push_back(currentDirectory);

        const juce::String scope = "*search*" + (roots.empty() ? juce::String("none") : juce::String());
        if (! roots.empty() && ! (recursiveScanValid && recursiveScanScope == scope))
            beginRecursiveScan(roots, scope);

        const auto filter = parseBrowserSearchFilter(searchQuery);
        items.clear();
        if (recursiveScanValid && recursiveScanScope == scope)
        {
            for (const auto& it : recursiveScanItems)
            {
                if (browserSection == BrowserSection::loops && ! isLoopItem(it)) continue;
                if (browserSection == BrowserSection::oneShots && ! isOneShotItem(it)) continue;
                if (! matchesBrowserSearch(it, filter)) continue;
                items.push_back(it);
            }

            std::stable_sort(items.begin(), items.end(), [&filter](const auto& lhs, const auto& rhs)
            {
                return browserSearchScore(lhs, filter) > browserSearchScore(rhs, filter);
            });
        }
        if (recursiveScanPending && items.empty())
            items.push_back(BrowserItem { juce::String::fromUTF8("Searching\xe2\x80\xa6"), "", "",
                                          juce::Colour(0xff7a8ba0), 4.0, juce::File(), true, true });

        selectedIndex.reset();
        hoverIndex.reset();
        dragIndex.reset();
        clampScrollOffset();
        // CRITICAL: stamp the watched location, else timerCallback sees a "stale" timestamp every tick,
        // invalidates the scan cache, and re-scans the WHOLE library forever (runaway CPU + memory).
        watchedLocationTimestamp = getWatchedLocationTimestamp();
        repaint();
        return;
    }

    if (showingLocationRoots)
    {
        const juce::String systemName = "Macintosh HD";

        refreshedItems.push_back(BrowserItem {
            systemName,
            "System",
            "Open system disk",
            juce::Colour(0xff5b84d6),
            4.0,
            getMacBrowseRoot(),
            true,
            false
        });

        refreshedItems.push_back(BrowserItem {
            mountedDevicesHubName,
            "Volume",
            "Open mounted devices",
            juce::Colour(0xff7a8ba0),
            4.0,
            juce::File("/Volumes"),
            true,
            false
        });
    }
    else if (showingMacRootOverview)
    {
        refreshedItems.push_back(BrowserItem {
            "..",
            "Folder",
            "Back to locations",
            juce::Colour(0xff7a8ba0),
            4.0,
            juce::File(),
            true,
            true
        });

        const std::array<juce::String, 4> topLevelNames { "Applications", "Library", "System", "Users" };
        for (const auto& name : topLevelNames)
        {
            const auto directory = juce::File("/") .getChildFile(name);
            if (! directory.isDirectory())
                continue;

            refreshedItems.push_back(BrowserItem {
                name,
                "Folder",
                "Open folder",
                colourForEntry(directory, true),
                4.0,
                directory,
                true,
                false
            });
        }
    }
    else if (showingUserHomeOverview)
    {
        refreshedItems.push_back(BrowserItem {
            "..",
            "Folder",
            "Back to users",
            juce::Colour(0xff7a8ba0),
            4.0,
            juce::File("/Users"),
            true,
            true
        });

        const std::array<std::pair<juce::String, juce::String>, 5> homeFolders {
            std::pair { juce::String("Desktop"), juce::String("Desktop") },
            std::pair { juce::String("Documents"), juce::String("Documents") },
            std::pair { juce::String("Downloads"), juce::String("Downloads") },
            std::pair { juce::String("Music"), juce::String("Music") },
            std::pair { juce::String("Movies"), juce::String("Movies") }
        };

        for (const auto& [name, subtitle] : homeFolders)
        {
            refreshedItems.push_back(BrowserItem {
                name,
                "Folder",
                subtitle,
                juce::Colour(0xff5b84d6),
                4.0,
                currentDirectory.getChildFile(name),
                true,
                false
            });
        }
    }
    else if (currentDirectory == juce::File("/Volumes"))
    {
        refreshedItems.push_back(BrowserItem {
            "..",
            "Folder",
            "Back to locations",
            juce::Colour(0xff7a8ba0),
            4.0,
            juce::File(),
            true,
            true
        });

        juce::Array<juce::File> mountedVolumes;
        currentDirectory.findChildFiles(mountedVolumes, juce::File::findDirectories, false);
        std::sort(mountedVolumes.begin(), mountedVolumes.end(),
                  [](const juce::File& a, const juce::File& b)
                  {
                      return a.getFileName().compareNatural(b.getFileName()) < 0;
                  });

        const auto macBrowseRoot = getMacBrowseRoot();
        for (const auto& volume : mountedVolumes)
        {
            auto volumeName = volume.getFileName();
            if (volumeName.isEmpty() || volume == macBrowseRoot || volumeName == "Macintosh HD")
                continue;

            refreshedItems.push_back(BrowserItem {
                volumeName,
                "Volume",
                "Mounted device",
                juce::Colour(0xff7a8ba0),
                4.0,
                volume,
                true,
                false
            });
        }
    }
    else if (currentDirectory.isDirectory())
    {
        const auto parentDirectory = currentDirectory.getParentDirectory();
        if (! customRootActive && parentDirectory != currentDirectory && parentDirectory.exists())
        {
            refreshedItems.push_back(BrowserItem {
                "..",
                "Folder",
                "Go up one level",
                juce::Colour(0xff7a8ba0),
                4.0,
                parentDirectory,
                true,
                true
            });
        }

        // With an active search query, scan the WHOLE subtree (recursively) so "loops",
        // "one shots" or a name match files at any depth under the current folder — not just
        // the current level. Without a query, show the normal folder listing (subdirs + files).
        if (searching)
        {
            // Ableton-style: a search spans the WHOLE library (all added folders), not just the open
            // folder. If no library folders are set, fall back to searching under the current folder.
            const bool global = ! libraryRoots.empty();
            const juce::String scope = global ? juce::String("*library*") : currentDirectory.getFullPathName();

            if (recursiveScanValid && recursiveScanScope == scope)
                for (const auto& cached : recursiveScanItems)
                    refreshedItems.push_back(cached);
            else
                beginRecursiveScan(global ? libraryRoots : std::vector<juce::File> { currentDirectory }, scope);
        }
        else
        {
            juce::Array<juce::File> childDirectories;
            currentDirectory.findChildFiles(childDirectories, juce::File::findDirectories, false);
            std::sort(childDirectories.begin(), childDirectories.end(),
                      [](const juce::File& a, const juce::File& b)
                      {
                          return a.getFileName().compareNatural(b.getFileName()) < 0;
                      });

            for (const auto& directory : childDirectories)
            {
                if (! shouldShowDirectory(directory))
                    continue;

                refreshedItems.push_back(BrowserItem {
                    directory.getFileName(),
                    "Folder",
                    "Open folder",
                    colourForEntry(directory, true),
                    4.0,
                    directory,
                    true,
                    false
                });
            }

            juce::Array<juce::File> childFiles;
            currentDirectory.findChildFiles(childFiles, juce::File::findFiles, false);
            std::sort(childFiles.begin(), childFiles.end(),
                      [](const juce::File& a, const juce::File& b)
                      {
                          return a.getFileName().compareNatural(b.getFileName()) < 0;
                      });

            for (const auto& file : childFiles)
            {
                if (! isAudioFile(file))
                    continue;

                BrowserItem audioItem {
                    file.getFileName(),
                    file.getParentDirectory().getFileName(),
                    subtitleForFile(file),
                    colourForEntry(file, false),
                    defaultLengthForFile(file),
                    file,
                    false,
                    false
                };
                audioItem.tags = deriveTags(file);
                refreshedItems.push_back(std::move(audioItem));
            }
        }
    }

    juce::String newSignature;
    if (searching)
    {
        // Cheap signature while searching — the source list is the (cached) recursive scan,
        // so concatenating thousands of paths on every keystroke is what dragged it down.
        newSignature << "@dir=" << currentDirectory.getFullPathName()
                     << "|n=" << static_cast<int>(refreshedItems.size());
    }
    else
    {
        for (const auto& item : refreshedItems)
            newSignature << item.name << "|" << item.subtitle << "|" << item.file.getFullPathName() << "\n";
    }
    // Include the search query in the signature — typing in the search box doesn't
    // change the directory listing but it MUST trigger a refresh of `items`.
    newSignature << "@q=" << searchQuery;
    newSignature << "@section=" << static_cast<int>(browserSection);

    if (newSignature == entrySignature)
        return;

    entrySignature = newSignature;
    unfilteredItems = std::move(refreshedItems);

    // In "find similar" mode the list is a similarity ranking, not the folder listing.
    if (inSimilarMode())
    {
        rebuildSimilarItems();
        return;
    }

    // Apply the case-insensitive search filter. Parent-link rows (".." / "Go up")
    // always pass through so the user can keep navigating while a query is active.
    items.clear();
    items.reserve(unfilteredItems.size());
    const auto filter = parseBrowserSearchFilter(searchQuery);
    for (const auto& item : unfilteredItems)
    {
        if (item.isParentLink)
        {
            items.push_back(item);
            continue;
        }

        if (browserSection == BrowserSection::loops && ! isLoopItem(item))
            continue;

        if (browserSection == BrowserSection::oneShots && ! isOneShotItem(item))
            continue;

        if (searchQuery.isNotEmpty() && ! matchesBrowserSearch(item, filter))
            continue;

        items.push_back(item);
    }

    if (searching)
    {
        std::stable_sort(items.begin(), items.end(), [&filter](const auto& lhs, const auto& rhs)
        {
            return browserSearchScore(lhs, filter) > browserSearchScore(rhs, filter);
        });
    }

    // While the background recursive scan is still running, show a placeholder so the user
    // knows results are coming (instead of an empty list that looks like "nothing found").
    if (searching && recursiveScanPending)
        items.push_back(BrowserItem { "Searching\xe2\x80\xa6", "", "", juce::Colour(0xff7a8ba0), 4.0, juce::File(), true, true });

    selectedIndex.reset();
    hoverIndex.reset();
    dragIndex.reset();
    clampScrollOffset();
    watchedLocationTimestamp = getWatchedLocationTimestamp();
    repaint();
}

void BrowserPanelComponent::enterSimilarMode(const juce::File& query)
{
    if (! query.existsAsFile())
        return;
    similarQuery = query;
    similarQueryEmb.clear();
    similarFiles.clear();
    similarDirty = true;
    similarRanked = false;
    searchQuery = {};
    searchEditor.setText({}, juce::dontSendNotification);

    juce::Component::SafePointer<BrowserPanelComponent> safe(this);
    sampleEmbedding.requestEmbedding(query, [safe](std::vector<float> v)
    {
        if (safe == nullptr) return;
        safe->similarQueryEmb = std::move(v);
        safe->similarDirty = true;
    });

    // Candidate pool = EVERY audio file across ALL the user's added library folders. If none are set
    // (the user just browses folders), broaden from the open sub-folder to its PARENT (the whole pack)
    // so similarity spans a diverse set, not one homogeneous sub-folder.
    std::vector<juce::File> roots = libraryRoots;
    if (roots.empty())
    {
        const auto parent = currentDirectory.getParentDirectory();
        roots.push_back(parent.isDirectory() && parent != currentDirectory ? parent : currentDirectory);
    }

    juce::Thread::launch([safe, roots]
    {
        std::vector<juce::File> found;
        for (const auto& root : roots)
        {
            if (! root.isDirectory()) continue;
            auto files = root.findChildFiles(juce::File::findFiles, true, "*.wav;*.aif;*.aiff;*.mp3;*.flac;*.ogg");
            for (auto& f : files) found.push_back(f);
            if (found.size() > 8000) break;   // sanity cap for very large libraries
        }
        juce::MessageManager::callAsync([safe, found = std::move(found)]() mutable
        {
            if (safe == nullptr || ! safe->inSimilarMode()) return;
            safe->similarFiles = std::move(found);
            safe->similarDirty = true;
        });
    });

    rebuildSimilarItems();
}

void BrowserPanelComponent::exitSimilarMode()
{
    if (! similarQuery.has_value())
        return;
    similarQuery.reset();
    similarQueryEmb.clear();
    similarDirty = false;
    similarRanked = false;
    entrySignature.clear();   // force a normal listing rebuild
    refreshEntries();
}

void BrowserPanelComponent::rebuildSimilarItems()
{
    similarDirty = false;
    if (! similarQuery.has_value() || similarRanked)   // frozen after the first complete ranking
        return;

    // Candidate pool = the whole-library file scan (falls back to the current folder's files until the
    // scan lands). Request any missing embeddings; each arrival re-ranks (coalesced in timerCallback).
    std::vector<juce::File> pool = similarFiles;
    if (pool.empty())
        for (const auto& it : unfilteredItems)
            if (! it.isDirectory && ! it.isParentLink && it.file.existsAsFile())
                pool.push_back(it.file);

    juce::Component::SafePointer<BrowserPanelComponent> safe(this);
    std::vector<juce::File>         cands;
    std::vector<std::vector<float>> embs;
    int requested = 0;
    for (const auto& f : pool)
    {
        if (f == *similarQuery) continue;
        if (auto e = sampleEmbedding.cachedEmbedding(f); e && ! e->empty())
        {
            cands.push_back(f);
            embs.push_back(std::move(*e));
        }
        else if (requested < 1500)   // throttle the analysis flood on huge libraries
        {
            ++requested;
            sampleEmbedding.requestEmbedding(f, [safe](std::vector<float>) { if (safe) safe->similarDirty = true; });
        }
    }

    items.clear();
    // Header chip (row 0) — click to exit similar mode. No indexing counter (pre-indexed in background).
    items.push_back(BrowserItem { juce::String::fromUTF8("\xE2\x97\x82  Similar to ") + similarQuery->getFileNameWithoutExtension(),
                                  "", "tap to exit", theme::cool::cyan, 4.0, juce::File(), false, false });

    if (! similarQueryEmb.empty() && ! embs.empty())
    {
        const auto ranked = SampleEmbedding::rankSimilar(similarQueryEmb, embs);
        int shown = 0;
        for (const auto& m : ranked)
        {
            const auto& f = cands[(std::size_t) m.index];
            BrowserItem it {
                f.getFileNameWithoutExtension(),
                "",
                juce::String((int) std::round(m.score * 100.0f)) + "% match  •  " + f.getParentDirectory().getFileName(),
                colourForEntry(f, false),
                4.0,
                f,
                false,
                false
            };
            items.push_back(std::move(it));
            if (++shown >= 120) break;
        }

        // Freeze once we've ranked with the real pool + query embedding both ready → no visible reshuffle.
        if (! similarFiles.empty())
            similarRanked = true;
    }

    selectedIndex.reset();
    hoverIndex.reset();
    dragIndex.reset();
    clampScrollOffset();
    repaint();
}

void BrowserPanelComponent::beginRecursiveScan(std::vector<juce::File> roots, const juce::String& scopeKey)
{
    if (recursiveScanPending && scanPendingScope == scopeKey)
        return;   // a scan for this scope is already running

    recursiveScanPending = true;
    scanPendingScope = scopeKey;
    const int generation = ++scanGeneration;
    juce::Component::SafePointer<BrowserPanelComponent> safeThis(this);

    scanPool.addJob([safeThis, roots = std::move(roots), scopeKey, generation]()
    {
        // Heavy disk traversal across ALL roots (whole library on a global search) — off the message
        // thread so the UI never freezes. Dedup by path so nested roots don't double up.
        juce::Array<juce::File> found;
        std::set<juce::String> seen;
        int scanned = 0;
        bool capped = false;
        for (const auto& root : roots)
        {
            if (! root.isDirectory()) continue;
            for (const auto& entry : juce::RangedDirectoryIterator(root, true, "*", juce::File::findFiles))
            {
                const auto file = entry.getFile();
                if (file.hasFileExtension("wav;wave;aif;aiff;mp3;flac;ogg;m4a")
                    && seen.insert(file.getFullPathName()).second)
                    found.add(file);
                if (++scanned >= 200000 || found.size() >= 30000) { capped = true; break; }   // safety caps
            }
            if (capped) break;
        }
        std::sort(found.begin(), found.end(),
                  [](const juce::File& a, const juce::File& b)
                  {
                      return a.getFileName().compareNatural(b.getFileName()) < 0;
                  });

        juce::MessageManager::callAsync([safeThis, scopeKey, generation, found]()
        {
            auto* self = safeThis.getComponent();
            if (self == nullptr || generation != self->scanGeneration.load())
                return;   // component gone or superseded by a newer scan

            self->recursiveScanItems.clear();
            self->recursiveScanItems.reserve(static_cast<std::size_t>(found.size()));
            for (const auto& file : found)
                self->recursiveScanItems.push_back(BrowserItem {
                    file.getFileName(),
                    file.getParentDirectory().getFileName(),
                    self->subtitleForFile(file),
                    self->colourForEntry(file, false),
                    self->defaultLengthForFile(file),
                    file,
                    false,
                    false
                });

            self->recursiveScanScope = scopeKey;
            self->recursiveScanValid = true;
            self->recursiveScanPending = false;
            self->refreshEntries();   // rebuild now that the cache is ready
        });
    });
}

bool BrowserPanelComponent::isCustomLibraryRoot(const juce::File& directory) const
{
    const auto path = directory.getFullPathName();
    return std::any_of(libraryRoots.begin(), libraryRoots.end(), [&path](const juce::File& root)
    {
        return root.getFullPathName() == path;
    });
}

void BrowserPanelComponent::navigateTo(const juce::File& directory, bool addToHistory, bool customRoot)
{
    if (! directory.isDirectory())
        return;

    const BrowserLocationState nextState {
        directory,
        false,
        false,
        isUserHomeDirectory(directory)
    };

    if (isCurrentLocation(nextState))
        return;

    if (addToHistory)
    {
        pushCurrentLocationToBackHistory();
        forwardHistory.clear();
    }

    showingLocationRoots = false;
    showingMacRootOverview = false;
    showingUserHomeOverview = isUserHomeDirectory(directory);
    customRootActive = customRoot;
    currentDirectory = directory;
    scrollOffsetY = 0.0;
    refreshEntries();
    updateNavigationButtons();
}

void BrowserPanelComponent::showLocationRoots(bool addToHistory)
{
    const BrowserLocationState nextState {
        getMacBrowseRoot(),
        true,
        false,
        false
    };

    if (isCurrentLocation(nextState))
        return;

    if (addToHistory)
    {
        pushCurrentLocationToBackHistory();
        forwardHistory.clear();
    }

    showingLocationRoots = true;
    showingMacRootOverview = false;
    showingUserHomeOverview = false;
    customRootActive = false;
    currentDirectory = getMacBrowseRoot();
    scrollOffsetY = 0.0;
    refreshEntries();
    updateNavigationButtons();
}

BrowserPanelComponent::BrowserLocationState BrowserPanelComponent::getCurrentLocationState() const
{
    return {
        currentDirectory,
        showingLocationRoots,
        showingMacRootOverview,
        showingUserHomeOverview
    };
}

void BrowserPanelComponent::restoreLocationState(const BrowserLocationState& state)
{
    if (isMountedVolumePath(state.directory))
    {
        showLocationRoots(false);
        return;
    }

    currentDirectory = state.directory;
    showingLocationRoots = state.locationRoots;
    showingMacRootOverview = state.macRootOverview;
    showingUserHomeOverview = state.userHomeOverview;
    customRootActive = ! state.locationRoots && ! state.macRootOverview && ! state.userHomeOverview
                    && isCustomLibraryRoot(state.directory);
    scrollOffsetY = 0.0;
    refreshEntries();
}

bool BrowserPanelComponent::isCurrentLocation(const BrowserLocationState& state) const
{
    return currentDirectory == state.directory
        && showingLocationRoots == state.locationRoots
        && showingMacRootOverview == state.macRootOverview
        && showingUserHomeOverview == state.userHomeOverview;
}

void BrowserPanelComponent::pushCurrentLocationToBackHistory()
{
    const auto currentState = getCurrentLocationState();
    if (backHistory.empty() || ! (backHistory.back().directory == currentState.directory
                                  && backHistory.back().locationRoots == currentState.locationRoots
                                  && backHistory.back().macRootOverview == currentState.macRootOverview
                                  && backHistory.back().userHomeOverview == currentState.userHomeOverview))
    {
        backHistory.push_back(currentState);
    }
}

void BrowserPanelComponent::goBackInBrowserHistory()
{
    if (customRootActive || backHistory.empty())
        return;

    forwardHistory.push_back(getCurrentLocationState());
    const auto previousState = backHistory.back();
    backHistory.pop_back();
    restoreLocationState(previousState);
    updateNavigationButtons();
}

void BrowserPanelComponent::goForwardInBrowserHistory()
{
    if (customRootActive || forwardHistory.empty())
        return;

    backHistory.push_back(getCurrentLocationState());
    const auto nextState = forwardHistory.back();
    forwardHistory.pop_back();
    restoreLocationState(nextState);
    updateNavigationButtons();
}

void BrowserPanelComponent::clearSearch()
{
    if (searchEditor.getText().isEmpty())
        return;

    searchEditor.clear();
    searchEditor.grabKeyboardFocus();
}

juce::String BrowserPanelComponent::getLocationDisplayName() const
{
    if (inSimilarMode())
        return "Similar sounds";

    if (showingLocationRoots)
        return "Locations";

    if (showingUserHomeOverview)
        return "Home";

    const auto name = currentDirectory.getFileName();
    return name.isNotEmpty() ? name : currentDirectory.getFullPathName();
}

void BrowserPanelComponent::updateNavigationButtons()
{
    const auto showNavigation = ! customRootActive;
    backButton.setVisible(showNavigation);
    forwardButton.setVisible(showNavigation);
    backButton.setEnabled(! customRootActive && ! backHistory.empty());
    forwardButton.setEnabled(! customRootActive && ! forwardHistory.empty());
}

void BrowserPanelComponent::unlockHorizontalSwipeGesture() noexcept
{
    horizontalWheelAccumulator = 0.0f;
    horizontalSwipeLocked = false;
    lastHorizontalWheelMs = 0;
}

juce::Rectangle<int> BrowserPanelComponent::getRowBounds(int index) const noexcept
{
    auto listViewport = getListViewportBounds();
    const auto y = juce::roundToInt(static_cast<double>(listViewport.getY() + index * (rowHeight + rowGap)) - scrollOffsetY);
    return { listViewport.getX(), y, listViewport.getWidth(), rowHeight };
}

juce::Rectangle<int> BrowserPanelComponent::getListViewportBounds() const noexcept
{
    auto bounds = getLocalBounds().reduced(contentPadX, 0);
    bounds.removeFromTop(headerHeight);
    bounds.removeFromTop(listTopPadding);
    bounds.removeFromBottom(previewBarHeight + previewSyncRowHeight + 3 + tagsRowHeight);   // card + sync + border + tags
    return bounds;
}

juce::Rectangle<int> BrowserPanelComponent::getTagsRowBounds() const noexcept
{
    auto bounds = getLocalBounds().reduced(contentPadX, 0);
    auto region = bounds.removeFromBottom(previewBarHeight + previewSyncRowHeight + 3 + tagsRowHeight);
    return region.removeFromTop(tagsRowHeight);   // strip just above the preview border
}

juce::Rectangle<int> BrowserPanelComponent::getPreviewBarBounds() const noexcept
{
    auto bounds = getLocalBounds().reduced(contentPadX, 0);
    auto region = bounds.removeFromBottom(previewBarHeight + previewSyncRowHeight);
    region.removeFromBottom(previewSyncRowHeight);   // sync row lives below the card
    return region;
}

juce::Rectangle<int> BrowserPanelComponent::getPreviewPlayButtonBounds() const noexcept
{
    auto bar = getPreviewBarBounds().reduced(10, 10);
    return bar.removeFromLeft(bar.getHeight());   // square button on the left
}

juce::Rectangle<int> BrowserPanelComponent::getPreviewSyncButtonBounds() const noexcept
{
    auto bounds = getLocalBounds().reduced(contentPadX, 0);
    auto row = bounds.removeFromBottom(previewSyncRowHeight);
    row = row.reduced(2, 3);
    return row.removeFromLeft((row.getWidth() - 4) / 2);
}

juce::Rectangle<int> BrowserPanelComponent::getPreviewKeySyncButtonBounds() const noexcept
{
    auto bounds = getLocalBounds().reduced(contentPadX, 0);
    auto row = bounds.removeFromBottom(previewSyncRowHeight).reduced(2, 3);
    row.removeFromLeft((row.getWidth() - 4) / 2 + 4);
    return row;
}

juce::Rectangle<int> BrowserPanelComponent::getPreviewWaveformBounds() const noexcept
{
    auto bar = getPreviewBarBounds().reduced(10, 10);
    bar.removeFromLeft(bar.getHeight() + 12);      // skip play button + gap
    return bar;
}

void BrowserPanelComponent::setPreviewWaveform(const juce::String& name, std::vector<float> peaks)
{
    previewName = name;
    previewPeaks = std::move(peaks);
    previewPositionRatio = 0.0f;
    repaint(getPreviewBarBounds());
}

void BrowserPanelComponent::setPreviewPlayback(bool playing, float positionRatio)
{
    if (previewPlaying == playing && std::abs(previewPositionRatio - positionRatio) < 0.002f)
        return;
    previewPlaying = playing;
    previewPositionRatio = juce::jlimit(0.0f, 1.0f, positionRatio);
    repaint(getPreviewBarBounds());
}

void BrowserPanelComponent::setPreviewArmed(bool armed)
{
    // Always repaint while armed (even if the flag didn't change) so the pulse animates,
    // driven by the host's 60 Hz timer ticking this each frame.
    previewArmed = armed;
    repaint(getPreviewBarBounds());
}

void BrowserPanelComponent::clearPreview()
{
    previewName = {};
    previewPeaks.clear();
    previewPlaying = false;
    previewArmed = false;
    previewPositionRatio = 0.0f;
    repaint(getPreviewBarBounds());
}

void BrowserPanelComponent::clampScrollOffset() noexcept
{
    const auto listViewport = getListViewportBounds();
    const auto contentHeight = getContentHeight(static_cast<int>(items.size()));
    const auto maxScroll = static_cast<double>(juce::jmax(0, contentHeight - listViewport.getHeight()));
    scrollOffsetY = juce::jlimit(0.0, maxScroll, scrollOffsetY);
}

std::optional<int> BrowserPanelComponent::hitTestRow(juce::Point<int> position) const noexcept
{
    for (int index = 0; index < static_cast<int>(items.size()); ++index)
    {
        if (getRowBounds(index).contains(position))
            return index;
    }

    return std::nullopt;
}

bool BrowserPanelComponent::isAudioFile(const juce::File& file) const noexcept
{
    return file.hasFileExtension("wav;wave;aif;aiff;mp3;flac;ogg;m4a");
}

bool BrowserPanelComponent::shouldShowDirectory(const juce::File& directory) const noexcept
{
    const auto name = directory.getFileName();
    if (name.isEmpty())
        return false;

    if (name.startsWithChar('.'))
        return false;

    static const std::array<juce::String, 7> hiddenNames {
        "Library",
        "Applications",
        "System",
        "private",
        "cores",
        "opt",
        "tmp"
    };

    return std::find(hiddenNames.begin(), hiddenNames.end(), name) == hiddenNames.end();
}

juce::Colour BrowserPanelComponent::colourForEntry(const juce::File& file, bool isDirectory) const noexcept
{
    if (isDirectory)
        return juce::Colour(0xff5b84d6);

    const auto lowerName = file.getFileNameWithoutExtension().toLowerCase();
    if (lowerName.contains("kick") || lowerName.contains("snare") || lowerName.contains("clap") || lowerName.contains("hat"))
        return juce::Colour(0xffd97a2b);
    if (lowerName.contains("bass") || lowerName.contains("808"))
        return juce::Colour(0xffca5d54);
    if (lowerName.contains("vox") || lowerName.contains("lead") || lowerName.contains("melody"))
        return juce::Colour(0xff5b84d6);
    if (lowerName.contains("pad") || lowerName.contains("texture") || lowerName.contains("atmo"))
        return juce::Colour(0xff7b6db5);

    return juce::Colour(0xff8aa0b7);
}

double BrowserPanelComponent::defaultLengthForFile(const juce::File& file) const noexcept
{
    if (metadataTypeForFile(file) == "Loop")
        return 8.0;

    return 4.0;
}

void BrowserPanelComponent::requestSoundTags(int itemIndex)
{
    if (itemIndex < 0 || itemIndex >= static_cast<int>(items.size()))
        return;
    auto& item = items[static_cast<std::size_t>(itemIndex)];
    if (item.isDirectory || item.isParentLink || item.soundTagsRequested)
        return;
    item.soundTagsRequested = true;

    const auto path = item.file.getFullPathName();
    juce::Component::SafePointer<BrowserPanelComponent> safe(this);
    audioTagger.requestTags(item.file, [safe, path](juce::StringArray soundTags) mutable
    {
        if (safe == nullptr || soundTags.isEmpty())
            return;
        auto* self = safe.getComponent();
        for (auto& it : self->items)
        {
            if (it.isDirectory || it.file.getFullPathName() != path)
                continue;
            juce::StringArray freshForDisplay;
            for (const auto& t : soundTags)
                if (! it.tags.contains(t)) { it.tags.add(t); }
            // Append tags not already visible in the subtitle (keeps the row readable).
            for (const auto& t : soundTags)
                if (! it.subtitle.containsIgnoreCase(t)) freshForDisplay.add(t);
            if (! freshForDisplay.isEmpty())
                it.subtitle += "  •  " + freshForDisplay.joinIntoString(" ");
            break;
        }
        self->repaint();
    });
}

juce::String BrowserPanelComponent::subtitleForFile(const juce::File& file) const
{
    const auto extension = file.getFileExtension().trimCharactersAtStart(".");
    // Show the auto-derived content tags (instrument/type) inline, Ableton-style. The type tag is
    // already the leading word, so drop it from the tag list to avoid repeating it.
    auto tags = deriveTags(file);
    tags.removeString("Loop");
    tags.removeString("One-shot");
    const auto tagText = tags.isEmpty() ? juce::String() : ("  •  " + tags.joinIntoString(" "));
    return metadataTypeForFile(file) + tagText + "  •  " + extension.toUpperCase();
}

juce::Time BrowserPanelComponent::getWatchedLocationTimestamp() const
{
    if (showingLocationRoots)
        return juce::Time();

    if (showingMacRootOverview)
        return juce::Time();

    if (showingUserHomeOverview)
        return juce::Time();

    if (currentDirectory.isDirectory())
        return currentDirectory.getLastModificationTime();

    return juce::Time();
}
}  // namespace orion
