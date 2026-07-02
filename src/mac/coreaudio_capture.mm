/**
 * macOS Audio Capture via ScreenCaptureKit (macOS 13 Ventura+)
 *
 * Process-level audio isolation using SCStream:
 * - Include mode: SCContentFilter with includingApplications → only target app audio
 * - Exclude mode: SCContentFilter with excludingApplications → all audio except target
 *
 * Audio is delivered as raw PCM float32, stereo, 48kHz — matching the Windows WASAPI output.
 */

#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <CoreMedia/CoreMedia.h>
#import <AudioToolbox/AudioToolbox.h>
#import <Foundation/Foundation.h>
#include "coreaudio_capture.h"
#include <dispatch/dispatch.h>
#include <iostream>
#include <thread>

// --- SCStream delegate that forwards audio samples to the C++ callback ---

@interface AudioStreamDelegate : NSObject <SCStreamOutput, SCStreamDelegate>
@property (nonatomic, assign) CoreAudioCapture::DataCallback dataCallback;
@property (nonatomic, assign) std::atomic<bool>* isCapturingRef;
@end

@implementation AudioStreamDelegate

- (void)stream:(SCStream *)stream didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer ofType:(SCStreamOutputType)type {
    if (type != SCStreamOutputTypeAudio) return;
    if (!self.isCapturingRef || !self.isCapturingRef->load()) return;
    if (!self.dataCallback) return;

    CMBlockBufferRef blockBuffer = CMSampleBufferGetDataBuffer(sampleBuffer);
    if (!blockBuffer) return;

    size_t totalLength = 0;
    char* dataPointer = NULL;
    OSStatus status = CMBlockBufferGetDataPointer(blockBuffer, 0, NULL, &totalLength, &dataPointer);
    if (status != kCMBlockBufferNoErr || !dataPointer || totalLength == 0) return;

    // Extract format from the sample buffer
    CMFormatDescriptionRef formatDesc = CMSampleBufferGetFormatDescription(sampleBuffer);
    const AudioStreamBasicDescription* asbd = CMAudioFormatDescriptionGetStreamBasicDescription(formatDesc);

    CoreAudioCapture::AudioMetadata meta;
    meta.sampleRate = (uint32_t)asbd->mSampleRate;
    meta.channels = (uint16_t)asbd->mChannelsPerFrame;
    meta.bitsPerSample = (uint16_t)asbd->mBitsPerChannel;
    meta.isFloat = (asbd->mFormatFlags & kAudioFormatFlagIsFloat) != 0;

    self.dataCallback((const uint8_t*)dataPointer, totalLength, meta);
}

- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error {
    if (error) {
        std::cerr << "[electron-native-screenshare] SCStream stopped with error: "
                  << error.localizedDescription.UTF8String << std::endl;
    }
}

@end

// --- Pimpl implementation ---

struct CoreAudioCapture::Impl {
    SCStream* stream = nil;
    AudioStreamDelegate* delegate = nil;
    dispatch_queue_t captureQueue = nil;
    uint32_t targetPid = 0;
    bool includeMode = false;
};

CoreAudioCapture::CoreAudioCapture() : pImpl(new Impl()) {
    pImpl->captureQueue = dispatch_queue_create("com.electron-native-screenshare.audio", DISPATCH_QUEUE_SERIAL);
}

CoreAudioCapture::~CoreAudioCapture() {
    Stop();
    if (pImpl) {
        if (pImpl->delegate) {
            pImpl->delegate = nil;
        }
        if (pImpl->captureQueue) {
            pImpl->captureQueue = nil;
        }
        delete pImpl;
        pImpl = nullptr;
    }
}

int CoreAudioCapture::Initialize(uint32_t processId, bool isIncludeMode, std::string& outError) {
    pImpl->targetPid = processId;
    pImpl->includeMode = isIncludeMode;

    __block int result = 0;
    __block std::string blockError;

    // ScreenCaptureKit requires async content enumeration — bridge to sync with semaphore
    dispatch_semaphore_t sema = dispatch_semaphore_create(0);

    [SCShareableContent getShareableContentExcludingDesktopWindows:NO
                                              onScreenWindowsOnly:NO
                                                completionHandler:^(SCShareableContent* _Nullable content, NSError* _Nullable error) {
        if (error || !content) {
            blockError = error ? std::string(error.localizedDescription.UTF8String)
                               : "Failed to get shareable content";
            result = -1;
            dispatch_semaphore_signal(sema);
            return;
        }

        // Find the target application by PID
        SCRunningApplication* targetApp = nil;
        for (SCRunningApplication* app in content.applications) {
            if (app.processID == (pid_t)processId) {
                targetApp = app;
                break;
            }
        }

        if (!targetApp) {
            blockError = "Target process not found in running applications (PID: "
                         + std::to_string(processId) + ")";
            result = -2;
            dispatch_semaphore_signal(sema);
            return;
        }

        // Build content filter
        SCContentFilter* filter = nil;
        SCDisplay* primaryDisplay = content.displays.firstObject;
        if (!primaryDisplay) {
            blockError = "No display found for content filter";
            result = -3;
            dispatch_semaphore_signal(sema);
            return;
        }

        if (isIncludeMode) {
            // Include mode: capture ONLY the target app's audio
            filter = [[SCContentFilter alloc] initWithDisplay:primaryDisplay
                                        includingApplications:@[targetApp]
                                             exceptingWindows:@[]];
        } else {
            // Exclude mode: capture everything EXCEPT the target app
            filter = [[SCContentFilter alloc] initWithDisplay:primaryDisplay
                                       excludingApplications:@[targetApp]
                                            exceptingWindows:@[]];
        }

        // Configure for audio capture (48kHz stereo float32, matching Windows WASAPI output)
        SCStreamConfiguration* config = [[SCStreamConfiguration alloc] init];
        config.capturesAudio = YES;
        config.excludesCurrentProcessAudio = NO; // handled by the filter
        config.sampleRate = 48000;
        config.channelCount = 2;

        // Minimize video overhead — we only need audio
        config.width = 2;
        config.height = 2;
        config.minimumFrameInterval = CMTimeMake(1, 1); // 1 fps minimum

        // Create the stream
        pImpl->stream = [[SCStream alloc] initWithFilter:filter
                                                 configuration:config
                                                      delegate:nil];

        dispatch_semaphore_signal(sema);
    }];

    dispatch_semaphore_wait(sema, DISPATCH_TIME_FOREVER);

    if (result != 0) {
        outError = blockError;
    }
    return result;
}

void CoreAudioCapture::Start(DataCallback callback) {
    if (isCapturing.load() || !pImpl->stream) return;

    onData = callback;
    isCapturing.store(true);

    // Set up the delegate
    pImpl->delegate = [[AudioStreamDelegate alloc] init];
    pImpl->delegate.dataCallback = onData;
    pImpl->delegate.isCapturingRef = &isCapturing;

    NSError* addOutputError = nil;
    [pImpl->stream addStreamOutput:pImpl->delegate
                              type:SCStreamOutputTypeAudio
                sampleHandlerQueue:pImpl->captureQueue
                             error:&addOutputError];

    if (addOutputError) {
        std::cerr << "[electron-native-screenshare] Failed to add stream output: "
                  << addOutputError.localizedDescription.UTF8String << std::endl;
        isCapturing.store(false);
        return;
    }

    dispatch_semaphore_t startSema = dispatch_semaphore_create(0);
    __block bool startSuccess = false;

    [pImpl->stream startCaptureWithCompletionHandler:^(NSError* _Nullable error) {
        if (error) {
            std::cerr << "[electron-native-screenshare] SCStream start failed: "
                      << error.localizedDescription.UTF8String << std::endl;
        } else {
            startSuccess = true;
        }
        dispatch_semaphore_signal(startSema);
    }];

    dispatch_semaphore_wait(startSema, DISPATCH_TIME_FOREVER);

    if (!startSuccess) {
        isCapturing.store(false);
    }
}

void CoreAudioCapture::Stop() {
    if (!isCapturing.load()) return;
    isCapturing.store(false);

    if (pImpl->stream) {
        dispatch_semaphore_t stopSema = dispatch_semaphore_create(0);
        [pImpl->stream stopCaptureWithCompletionHandler:^(NSError* _Nullable error) {
            dispatch_semaphore_signal(stopSema);
        }];
        dispatch_semaphore_wait(stopSema, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC));
        pImpl->stream = nil;
    }
    pImpl->delegate = nil;
}

// --- getPidFromWindowId using CGWindowListCopyWindowInfo ---

uint32_t getPidFromWindowId(uint32_t windowId) {
    CFArrayRef windowList = CGWindowListCopyWindowInfo(
        kCGWindowListOptionIncludingWindow, (CGWindowID)windowId);

    if (!windowList) return 0;

    uint32_t pid = 0;
    CFIndex count = CFArrayGetCount(windowList);
    for (CFIndex i = 0; i < count; i++) {
        CFDictionaryRef windowInfo = (CFDictionaryRef)CFArrayGetValueAtIndex(windowList, i);

        CFNumberRef windowNumber = (CFNumberRef)CFDictionaryGetValue(windowInfo, kCGWindowNumber);
        int wid = 0;
        CFNumberGetValue(windowNumber, kCFNumberIntType, &wid);

        if ((uint32_t)wid == windowId) {
            CFNumberRef ownerPid = (CFNumberRef)CFDictionaryGetValue(windowInfo, kCGWindowOwnerPID);
            int p = 0;
            CFNumberGetValue(ownerPid, kCFNumberIntType, &p);
            pid = (uint32_t)p;
            break;
        }
    }

    CFRelease(windowList);
    return pid;
}
