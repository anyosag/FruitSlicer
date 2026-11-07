// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// macOS-only pieces. Compiled with ARC (see src/gui/CMakeLists.txt), so the
// activity token below stays alive until endBusy releases it.
#include "platform/Platform.h"

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#if !__has_feature(objc_arc)
#error "PlatformMac.mm needs ARC (-fobjc-arc)"
#endif

namespace gui::platform {

namespace {
id<NSObject> g_activity = nil;  // strong: holds off App Nap while a job runs
}

void beginBusy(const std::string& reason) {
    @autoreleasepool {
        if (g_activity) return;
        NSString* why = [NSString stringWithUTF8String:reason.c_str()];
        g_activity = [[NSProcessInfo processInfo] beginActivityWithOptions:NSActivityUserInitiated
                                                                    reason:(why ? why : @"FruitSlicer job")];
    }
}

void endBusy() {
    @autoreleasepool {
        if (!g_activity) return;
        [[NSProcessInfo processInfo] endActivity:g_activity];
        g_activity = nil;
    }
}

std::vector<Volume> removableVolumes() {
    std::vector<Volume> out;
    @autoreleasepool {
        NSArray<NSURLResourceKey>* keys = @[
            NSURLVolumeNameKey, NSURLVolumeIsRemovableKey, NSURLVolumeIsEjectableKey, NSURLVolumeIsLocalKey,
            NSURLVolumeIsReadOnlyKey, NSURLVolumeLocalizedFormatDescriptionKey, NSURLVolumeAvailableCapacityKey
        ];
        NSArray<NSURL*>* urls = [[NSFileManager defaultManager]
            mountedVolumeURLsIncludingResourceValuesForKeys:keys
                                                    options:NSVolumeEnumerationSkipHiddenVolumes];
        for (NSURL* url in urls) {
            NSDictionary<NSURLResourceKey, id>* v = [url resourceValuesForKeys:keys error:nil];
            const bool removable = [v[NSURLVolumeIsRemovableKey] boolValue];
            const bool ejectable = [v[NSURLVolumeIsEjectableKey] boolValue];
            if (!removable && !ejectable) continue;
            // Mounted disk images are ejectable but read-only; network
            // shares are not local. Neither goes to the printer.
            if (![v[NSURLVolumeIsLocalKey] boolValue] || [v[NSURLVolumeIsReadOnlyKey] boolValue]) continue;
            Volume vol;
            NSString* name = v[NSURLVolumeNameKey];
            NSString* format = v[NSURLVolumeLocalizedFormatDescriptionKey];
            const char* fsr = [[url path] fileSystemRepresentation];
            if (!fsr) continue;
            vol.name = name && [name UTF8String] ? std::string([name UTF8String]) : std::string();
            vol.path = fsr;
            vol.format = format && [format UTF8String] ? std::string([format UTF8String]) : std::string();
            vol.free_bytes = [v[NSURLVolumeAvailableCapacityKey] unsignedLongLongValue];
            out.push_back(vol);
        }
    }
    return out;
}

bool ejectVolume(const std::string& path, std::string* error) {
    @autoreleasepool {
        NSString* p = [NSString stringWithUTF8String:path.c_str()];
        if (!p) {
            if (error) *error = "The drive's name could not be read.";
            return false;
        }
        NSURL* url = [NSURL fileURLWithPath:p isDirectory:YES];
        NSError* err = nil;
        // Apple documents this call as safe from any thread.
        if ([[NSWorkspace sharedWorkspace] unmountAndEjectDeviceAtURL:url error:&err]) return true;
        const char* why = err ? [[err localizedDescription] UTF8String] : nullptr;
        if (error) *error = why ? std::string(why) : std::string("The drive is busy.");
        return false;
    }
}

void revealFile(const std::string& path) {
    @autoreleasepool {
        NSString* p = [NSString stringWithUTF8String:path.c_str()];
        if (!p) return;
        [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[ [NSURL fileURLWithPath:p] ]];
    }
}

}  // namespace gui::platform
