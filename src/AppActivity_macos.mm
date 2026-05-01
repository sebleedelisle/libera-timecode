#import "AppActivity.h"

#import <Foundation/Foundation.h>

namespace libera_timecode {

namespace {
id<NSObject> g_activityToken = nil;
}

void disableAppNap() {
    if (g_activityToken) return;
    @autoreleasepool {
        NSActivityOptions options = NSActivityUserInitiated
                                  | NSActivityLatencyCritical;
        id<NSObject> token = [[NSProcessInfo processInfo]
            beginActivityWithOptions:options
                              reason:@"Libera Timecode realtime output"];
        // beginActivity returns an autoreleased token; retain it for the
        // lifetime of the process so the activity stays active.
        g_activityToken = [token retain];
    }
}

} // namespace libera_timecode
