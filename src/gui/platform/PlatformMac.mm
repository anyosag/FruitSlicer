// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// macOS-only pieces. Compiled with ARC (see src/gui/CMakeLists.txt), so the
// activity token below stays alive until endBusy releases it.
#include "platform/Platform.h"

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

}  // namespace gui::platform
