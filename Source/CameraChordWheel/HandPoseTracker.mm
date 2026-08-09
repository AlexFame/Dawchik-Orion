#include "HandPoseTracker.h"

#if JUCE_MAC
#import <Vision/Vision.h>
#import <ImageIO/ImageIO.h>
#include <vector>

namespace orion::camera
{
std::optional<juce::Point<float>> detectIndexTip (const juce::Image& frame)
{
    if (! frame.isValid() || frame.getWidth() < 2 || frame.getHeight() < 2)
        return std::nullopt;

    // Vision hand-pose is happy with a small frame, and downscaling cuts both the pixel-copy and the
    // detection cost. Cap the long edge at 256 px. (Runs on a background thread — see the component.)
    juce::Image small = frame;
    constexpr int targetW = 256;
    if (frame.getWidth() > targetW)
        small = frame.rescaled (targetW, juce::jmax (2, frame.getHeight() * targetW / frame.getWidth()),
                                juce::Graphics::lowResamplingQuality);

    const int width  = small.getWidth();
    const int height = small.getHeight();

    std::vector<uint8_t> rgba (static_cast<size_t> (width * height * 4));
    {
        const juce::Image::BitmapData pixels (small, juce::Image::BitmapData::readOnly);
        const bool argb = pixels.pixelFormat == juce::Image::ARGB && pixels.pixelStride == 4;
        for (int y = 0; y < height; ++y)
        {
            auto* dst  = rgba.data() + static_cast<size_t> (y * width * 4);
            auto* line = pixels.getLinePointer (y);
            for (int x = 0; x < width; ++x)
            {
                if (argb)   // fast path: read the packed pixel directly (no Colour object per pixel)
                {
                    const auto* px = reinterpret_cast<const juce::PixelARGB*> (line + x * 4);
                    *dst++ = px->getRed();
                    *dst++ = px->getGreen();
                    *dst++ = px->getBlue();
                    *dst++ = px->getAlpha();
                }
                else
                {
                    const auto c = pixels.getPixelColour (x, y);
                    *dst++ = c.getRed();
                    *dst++ = c.getGreen();
                    *dst++ = c.getBlue();
                    *dst++ = c.getAlpha();
                }
            }
        }
    }

    std::optional<juce::Point<float>> result;
    @autoreleasepool
    {
        CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
        auto* owned = new std::vector<uint8_t> (std::move (rgba));
        CGDataProviderRef provider = CGDataProviderCreateWithData (owned, owned->data(), owned->size(),
            [] (void* info, const void*, size_t) { delete static_cast<std::vector<uint8_t>*> (info); });
        CGImageRef cgImage = CGImageCreate (width, height, 8, 32, static_cast<size_t> (width) * 4, space,
                                            kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big,
                                            provider, nullptr, false, kCGRenderingIntentDefault);
        CGColorSpaceRelease (space);
        CGDataProviderRelease (provider);
        if (cgImage == nullptr)
            return std::nullopt;

        VNDetectHumanHandPoseRequest* request = [[VNDetectHumanHandPoseRequest alloc] init];
        request.maximumHandCount = 1;
        VNImageRequestHandler* handler = [[VNImageRequestHandler alloc] initWithCGImage:cgImage
                                                                            orientation:kCGImagePropertyOrientationUp
                                                                                options:@{}];
        NSError* error = nil;
        if ([handler performRequests:@[ request ] error:&error] && error == nil)
        {
            VNHumanHandPoseObservation* observation = request.results.firstObject;
            if (observation != nil)
            {
                VNRecognizedPoint* point = [observation recognizedPointForJointName:VNHumanHandPoseObservationJointNameIndexTip
                                                                             error:&error];
                if (point != nil && point.confidence >= 0.3)
                    result = juce::Point<float> (static_cast<float> (point.location.x),
                                                 1.0f - static_cast<float> (point.location.y));
            }
        }

        // No ARC on this .mm: balance the +1 from alloc/init ourselves.
        [handler release];
        [request release];
        CGImageRelease (cgImage);
    }
    return result;
}
}
#else
namespace orion::camera
{
std::optional<juce::Point<float>> detectIndexTip (const juce::Image&) { return std::nullopt; }
}
#endif
