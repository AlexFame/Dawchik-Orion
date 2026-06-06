#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace orion::theme
{
namespace core
{
static const juce::Colour canvas     { 0xff0b0d0f };
static const juce::Colour deepSpace  { 0xff111417 };
static const juce::Colour studio     { 0xff16181c };
static const juce::Colour voidBlack  { 0xff0f1114 };
}  // namespace core

namespace surface
{
static const juce::Colour primary  { 0xff1b1f24 };
static const juce::Colour elevated { 0xff23272e };
static const juce::Colour panel    { 0xff2a3038 };
static const juce::Colour hover    { 0xff323841 };
}  // namespace surface

namespace line
{
static const juce::Colour subtle  { 0xff2f353d };
static const juce::Colour normal  { 0xff3e454f };
static const juce::Colour soft    { 0xff4a535e };
static const juce::Colour strong  { 0xff5b6470 };
}  // namespace line

namespace warm
{
static const juce::Colour red      { 0xffff5a4d };
static const juce::Colour coral    { 0xffff6f61 };
static const juce::Colour salmon   { 0xffff8a78 };
static const juce::Colour pink     { 0xffff9ba8 };
static const juce::Colour peach    { 0xffffb08a };
static const juce::Colour amber    { 0xffff9a32 };
}  // namespace warm

namespace cool
{
static const juce::Colour cyan      { 0xff00e5e0 };
static const juce::Colour turquoise { 0xff19d6c2 };
static const juce::Colour aqua      { 0xff4daeff };
static const juce::Colour blue      { 0xff3b82f6 };
static const juce::Colour indigo    { 0xff6a6bff };
static const juce::Colour violet    { 0xffa855f7 };
}  // namespace cool

namespace text
{
static const juce::Colour primary   { 0xfff2f4f7 };
static const juce::Colour secondary { 0xffc8cdd4 };
static const juce::Colour tertiary  { 0xff9aa3ae };
static const juce::Colour muted     { 0xff6e7781 };
static const juce::Colour disabled  { 0xff4e5660 };
static const juce::Colour inverse   { 0xff0b0d0f };
}  // namespace text

namespace status
{
static const juce::Colour success { 0xff22c55e };
static const juce::Colour warning { 0xfff59e08 };
static const juce::Colour error   { 0xffef4444 };
static const juce::Colour info    { 0xff3b82f6 };
static const juce::Colour focus   { 0xff00e5e0 };
static const juce::Colour off     { 0xff3a4048 };
}  // namespace status

namespace tracks
{
static const juce::Colour palette[] {
    warm::red.withSaturation(0.94f),
    warm::amber.withSaturation(0.92f),
    cool::turquoise.withSaturation(0.86f).darker(0.03f),
    cool::blue.withSaturation(0.90f).brighter(0.03f),
    warm::pink.withSaturation(0.88f).brighter(0.02f),
    cool::violet.withSaturation(0.88f).brighter(0.03f),
    warm::coral.withSaturation(0.90f),
    cool::aqua.withSaturation(0.84f).darker(0.03f),
    warm::peach.withSaturation(0.86f).darker(0.06f),
    cool::indigo.withSaturation(0.86f).brighter(0.02f),
    warm::salmon.withSaturation(0.88f)
};

inline juce::Colour colourForIndex(int index) noexcept
{
    constexpr auto paletteSize = static_cast<int>(sizeof(palette) / sizeof(palette[0]));
    return palette[static_cast<std::size_t>(juce::jlimit(0, paletteSize - 1, index % paletteSize))];
}
}  // namespace tracks
}  // namespace orion::theme
