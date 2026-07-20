#pragma once

// Shared palette + layout constants for MainComponent's translation units.
//
// These used to live in an anonymous namespace inside MainComponent.cpp. Once that file was
// split up (MainComponentWiring.cpp and friends), several units needed the same values, so they
// moved here rather than being duplicated. Internal to the MainComponent implementation — not
// part of any public interface.

#include "OrionTheme.h"
#include "TransportBarComponent.h"

namespace orion
{
inline const auto backgroundColour       = theme::core::canvas;
inline const auto panelColour            = theme::core::studio;
inline const auto accentColour           = theme::accent::activeCoral;
inline const auto panelStroke            = theme::line::subtle;
inline const auto mutedText              = theme::text::muted;
inline const auto transportShelfColour   = theme::core::voidBlack;
inline const auto transportShelfStroke   = theme::line::subtle;
inline const auto transportButtonColour  = theme::core::studio;
inline const auto transportButtonText    = theme::text::secondary;
inline const auto transportDarkPanel     = theme::core::voidBlack;
inline const auto transportSectionFill   = theme::core::canvas;
inline const auto transportSectionStroke = theme::line::subtle.withAlpha(0.45f);
inline const auto recordAccent           = theme::status::error;

inline constexpr int minBrowserPanelWidth = 220;
inline constexpr int maxBrowserPanelWidth = 520;
inline constexpr int browserResizeHandleWidth = 10;
inline constexpr int transportShelfHeight = TransportBarComponent::preferredHeight;
// The transport panel is already vertically centred inside the shelf with an 8 px inset.
// Keep the workspace flush to the shelf so the panel has the same visual air above and below.
inline constexpr int workspaceTopGap = 0;
inline constexpr int transportBrandWidth = 210;
inline constexpr int transportClusterWidth = 264;
inline constexpr int transportTempoWidth = 178;   // BPM + KEY combined card
inline constexpr int transportModeWidth = 152;
inline constexpr int transportUtilityWidth = 302;
inline constexpr int transportSectionGap = 12;
inline constexpr int transportControlHeight = 46;
inline constexpr int transportSectionHeight = 54;
inline constexpr int transportContentVerticalNudge = 0;
inline constexpr int samplerPanelHeight = 350;   // shared by the sampler and clip editor
inline constexpr const char* sidebarFoldersSettingsKey = "sidebar.customFolders";
} // namespace orion
