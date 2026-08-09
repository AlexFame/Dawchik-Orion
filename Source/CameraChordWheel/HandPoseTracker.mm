#include "HandPoseTracker.h"

#if JUCE_MAC
#import <Vision/Vision.h>
#import <ImageIO/ImageIO.h>
#include <vector>

namespace orion::camera
{
std::optional<juce::Point<float>> detectIndexTip (const juce::Image& frame)
{
    if (! frame.isValid())
        return std::nullopt;

    const int width = frame.getWidth();
    const int height = frame.getHeight();
    if (width < 2 || height < 2)
        return std::nullopt;

    std::vector<uint8_t> rgba (static_cast<size_t> (width * height * 4));
    juce::Image::BitmapData pixels (frame, juce::Image::BitmapData::readOnly);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
        {
            const auto c = pixels.getPixelColour (x, y);
            const auto i = static_cast<size_t> ((y * width + x) * 4);
            rgba[i + 0] = c.getRed();
            rgba[i + 1] = c.getGreen();
            rgba[i + 2] = c.getBlue();
            rgba[i + 3] = c.getAlpha();
        }

    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    auto* owned = new std::vector<uint8_t> (std::move (rgba));
    CGDataProviderRef provider = CGDataProviderCreateWithData (owned, owned->data(), owned->size(),
        [] (void* info, const void*, size_t) { delete static_cast<std::vector<uint8_t>*> (info); });
    CGImageRef cgImage = CGImageCreate (width, height, 8, 32, width * 4, space,
                                        kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big,
                                        provider, nullptr, false, kCGRenderingIntentDefault);
    CGColorSpaceRelease (space);
    CGDataProviderRelease (provider);
    if (cgImage == nullptr)
        return std::nullopt;

    __block std::optional<juce::Point<float>> result;
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
            if (point != nil && point.confidence >= 0.35)
                result = juce::Point<float> (static_cast<float> (point.location.x),
                                             1.0f - static_cast<float> (point.location.y));
        }
    }
    CGImageRelease (cgImage);
    return result;
}
}
#else
namespace orion::camera
{
std::optional<juce::Point<float>> detectIndexTip (const juce::Image&) { return std::nullopt; }
}
#endif
