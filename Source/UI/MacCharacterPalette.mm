#include "MacCharacterPalette.h"

#include <juce_gui_basics/juce_gui_basics.h>

#if JUCE_MAC
 #import <AppKit/AppKit.h>
#endif

namespace orion
{
void showSystemEmojiPalette()
{
#if JUCE_MAC
    @autoreleasepool
    {
        [[NSApplication sharedApplication] orderFrontCharacterPalette: nil];
    }
#endif
}
} // namespace orion
