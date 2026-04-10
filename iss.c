// iss — Instant Space Switcher
//
// Eliminates the macOS sliding animation when 3-finger swiping between spaces.
//
// How it works:
//   1. A CGEventTap intercepts trackpad dock-swipe gesture events (private
//      CGS event type 30, HID type 23) before the Dock sees them.
//      It also intercepts Ctrl+Left / Ctrl+Right, which is how Logitech
//      Options+ emits "switch desktop" button actions.
//   2. Real gesture and matching keyboard shortcut events are suppressed
//      (callback returns NULL).
//   3. On the first event with a clear direction, or on Ctrl+Left/Right, a
//      synthetic Begin+End
//      gesture pair is posted with high velocity, causing the Dock
//      to switch spaces instantly — no animation.
//   4. A passthrough counter prevents the tap from re-intercepting its own
//      synthetic events (CGEvent field tags don't survive CGEventPost).
//   5. Companion kCGSEventGesture events are also suppressed during an
//      active swipe to keep the event stream consistent.
//
// Vertical swipes (Mission Control, App Exposé) are left untouched.
// Does not require disabling SIP.

#include <ApplicationServices/ApplicationServices.h>
#include <CoreFoundation/CoreFoundation.h>
#include <float.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

// --- Undocumented CGS/IOKit constants ----------------------------------------
// These are private CGEvent integer fields used by the WindowServer and Dock
// for trackpad gesture routing. Discovered via reverse engineering.

static const CGEventField kCGSEventTypeField            = 55;  // real event type
static const CGEventField kCGEventGestureHIDType        = 110; // IOHIDEventType
static const CGEventField kCGEventGestureScrollY        = 119;
static const CGEventField kCGEventGestureSwipeMotion    = 123; // horiz vs vert
static const CGEventField kCGEventGestureSwipeProgress  = 124; // cumulative distance
static const CGEventField kCGEventGestureSwipeVelocityX = 129;
static const CGEventField kCGEventGestureSwipeVelocityY = 130;
static const CGEventField kCGEventGesturePhase          = 132; // began/changed/ended
static const CGEventField kCGEventScrollGestureFlagBits = 135; // direction hint
static const CGEventField kCGEventGestureZoomDeltaX     = 139; // required, reason unknown
static const double kInstantSwipeProgress               = 2.0;
static const double kInstantSwipeVelocity               = 2000.0;

enum { kCGSEventGesture = 29, kCGSEventDockControl = 30 };
enum { kIOHIDEventTypeDockSwipe = 23 };
enum { kCGGestureMotionHorizontal = 1 };
enum { kGestureBegan = 1, kGestureChanged = 2, kGestureEnded = 4, kGestureCancelled = 8 };
enum { kVK_LeftArrow = 123, kVK_RightArrow = 124 };

extern int CGSMainConnectionID(void);
extern uint64_t CGSGetActiveSpace(int cid);
extern CFArrayRef CGSCopyManagedDisplaySpaces(int cid);

// --- State -------------------------------------------------------------------

static CFMachPortRef tap;
static bool swipeTracking, swipeFired;
static bool interceptTrackpad = true, interceptShortcuts = true;
static int  passthrough; // synthetic events remaining to let through
static int  shortcutTrackingKey = -1;

// --- Synthetic gesture posting ------------------------------------------------
// Posts a complete Begin+End dock swipe with high velocity. The Dock treats
// this as an instantaneous swipe and switches without animating.

// Create a DockControl event with fields common to both Begin and End phases.
static CGEventRef make_dock_event(int phase, bool right) {
    CGEventRef ev = CGEventCreate(NULL);
    if (!ev) return NULL;
    CGEventSetIntegerValueField(ev, kCGSEventTypeField, kCGSEventDockControl);
    CGEventSetIntegerValueField(ev, kCGEventGestureHIDType, kIOHIDEventTypeDockSwipe);
    CGEventSetIntegerValueField(ev, kCGEventGesturePhase, phase);
    CGEventSetIntegerValueField(ev, kCGEventScrollGestureFlagBits, right ? 1 : 0);
    CGEventSetIntegerValueField(ev, kCGEventGestureSwipeMotion, kCGGestureMotionHorizontal);
    CGEventSetDoubleValueField(ev, kCGEventGestureScrollY, 0);
    CGEventSetDoubleValueField(ev, kCGEventGestureZoomDeltaX, FLT_TRUE_MIN);
    return ev;
}

// Post a paired (companion gesture + dock control) event to the session tap.
static bool post_pair(CGEventRef dock) {
    CGEventRef companion = CGEventCreate(NULL);
    if (!companion) { CFRelease(dock); return false; }
    CGEventSetIntegerValueField(companion, kCGSEventTypeField, kCGSEventGesture);
    CGEventPost(kCGSessionEventTap, dock);
    CGEventPost(kCGSessionEventTap, companion);
    CFRelease(dock); CFRelease(companion);
    return true;
}

// Check whether there is a space to switch to in the given direction.
// Queries the private CGS API for the per-display space list and finds
// the active space's position within it.
static bool can_switch(bool right) {
    int cid = CGSMainConnectionID();
    uint64_t active = CGSGetActiveSpace(cid);
    CFArrayRef displays = CGSCopyManagedDisplaySpaces(cid);
    if (!displays) return true;

    bool can = true;
    for (CFIndex i = 0; i < CFArrayGetCount(displays); i++) {
        CFDictionaryRef display = CFArrayGetValueAtIndex(displays, i);
        CFArrayRef spaces = CFDictionaryGetValue(display, CFSTR("Spaces"));
        if (!spaces) continue;
        CFIndex count = CFArrayGetCount(spaces);
        for (CFIndex j = 0; j < count; j++) {
            CFDictionaryRef space = CFArrayGetValueAtIndex(spaces, j);
            CFNumberRef sid = CFDictionaryGetValue(space, CFSTR("ManagedSpaceID"));
            if (!sid) continue;
            int64_t val;
            CFNumberGetValue(sid, kCFNumberSInt64Type, &val);
            if ((uint64_t)val == active) {
                if (right && j == count - 1) can = false;
                if (!right && j == 0) can = false;
                goto done;
            }
        }
    }
done:
    CFRelease(displays);
    return can;
}

static void post_switch(bool right) {
    if (!can_switch(right)) return;
    double sign = right ? 1.0 : -1.0;

    CGEventRef begin = make_dock_event(kGestureBegan, right);
    if (!begin) return;

    CGEventRef end = make_dock_event(kGestureEnded, right);
    if (!end) { CFRelease(begin); return; }
    CGEventSetDoubleValueField(end, kCGEventGestureSwipeProgress, sign * kInstantSwipeProgress);
    CGEventSetDoubleValueField(end, kCGEventGestureSwipeVelocityX, sign * kInstantSwipeVelocity);
    CGEventSetDoubleValueField(end, kCGEventGestureSwipeVelocityY, 0);

    if (interceptTrackpad) passthrough += 4; // 2 pairs × 2 events
    post_pair(begin);
    post_pair(end);
}

static bool set_mode(const char *mode) {
    if (strcmp(mode, "both") == 0) {
        interceptTrackpad = true;
        interceptShortcuts = true;
        return true;
    }
    if (strcmp(mode, "trackpad") == 0) {
        interceptTrackpad = true;
        interceptShortcuts = false;
        return true;
    }
    if (strcmp(mode, "shortcuts") == 0) {
        interceptTrackpad = false;
        interceptShortcuts = true;
        return true;
    }
    return false;
}

static const char *mode_name(void) {
    if (interceptTrackpad && interceptShortcuts) return "both";
    if (interceptTrackpad) return "trackpad";
    return "shortcuts";
}

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s [--mode both|trackpad|shortcuts]\n", prog);
}

static bool is_space_shortcut(CGEventRef ev, int *keycode_out) {
    int keycode = (int)CGEventGetIntegerValueField(ev, kCGKeyboardEventKeycode);
    if (keycode != kVK_LeftArrow && keycode != kVK_RightArrow) return false;

    CGEventFlags mods = CGEventGetFlags(ev);
    CGEventFlags relevant = mods & (kCGEventFlagMaskShift |
                                    kCGEventFlagMaskControl |
                                    kCGEventFlagMaskAlternate |
                                    kCGEventFlagMaskCommand);
    if (relevant != kCGEventFlagMaskControl) return false;

    if (keycode_out) *keycode_out = keycode;
    return true;
}

// --- Event tap callback ------------------------------------------------------
// Intercepts real horizontal dock swipes. Direction is determined from swipe
// progress (Changed phase) or velocity (Ended phase, fallback for discrete
// swipes that skip Changed entirely). Returns NULL to suppress the original
// event, or ev to pass it through.

static CGEventRef cb(CGEventTapProxy proxy, CGEventType type, CGEventRef ev, void *ctx) {
    (void)proxy; (void)ctx;

    // System disabled our tap (callback too slow) — re-enable
    if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
        CGEventTapEnable(tap, true);
        return ev;
    }

    if (interceptShortcuts && (type == kCGEventKeyDown || type == kCGEventKeyUp)) {
        int keycode;
        if (is_space_shortcut(ev, &keycode)) {
            if (type == kCGEventKeyDown) {
                shortcutTrackingKey = keycode;
                if (!CGEventGetIntegerValueField(ev, kCGKeyboardEventAutorepeat)) {
                    post_switch(keycode == kVK_RightArrow);
                }
                return NULL;
            }

            if (shortcutTrackingKey == keycode) {
                shortcutTrackingKey = -1;
                return NULL;
            }
        } else if (type == kCGEventKeyUp && shortcutTrackingKey != -1) {
            shortcutTrackingKey = -1;
        }
    }

    int et = (int)CGEventGetIntegerValueField(ev, kCGSEventTypeField);

    // Let our own synthetic events pass through untouched
    if (passthrough > 0 && (et == kCGSEventDockControl || et == kCGSEventGesture)) {
        passthrough--;
        return ev;
    }

    // Only intercept horizontal dock swipes (not Mission Control, App Exposé, etc.)
    if (interceptTrackpad
        && et == kCGSEventDockControl
        && (int)CGEventGetIntegerValueField(ev, kCGEventGestureHIDType) == kIOHIDEventTypeDockSwipe
        && (int)CGEventGetIntegerValueField(ev, kCGEventGestureSwipeMotion) == kCGGestureMotionHorizontal) {

        int phase = (int)CGEventGetIntegerValueField(ev, kCGEventGesturePhase);

        if (phase == kGestureBegan) {
            swipeTracking = true; swipeFired = false; return NULL;
        }
        if (phase == kGestureChanged && swipeTracking) {
            if (!swipeFired) {
                double p = CGEventGetDoubleValueField(ev, kCGEventGestureSwipeProgress);
                if (p != 0.0) { swipeFired = true; post_switch(p > 0); }
            }
            return NULL;
        }
        if (phase == kGestureEnded && swipeTracking) {
            if (!swipeFired) {
                double v = CGEventGetDoubleValueField(ev, kCGEventGestureSwipeVelocityX);
                if (v != 0.0) post_switch(v > 0);
            }
            swipeTracking = swipeFired = false; return NULL;
        }
        if (phase == kGestureCancelled) {
            swipeTracking = swipeFired = false; return NULL;
        }
        return swipeTracking ? NULL : ev;
    }

    // Suppress companion gesture events paired with the dock swipe
    if (interceptTrackpad && et == kCGSEventGesture && swipeTracking) return NULL;
    return ev;
}

static volatile sig_atomic_t running = 1;
static void stop(int s) { (void)s; running = 0; }

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        const char *mode = NULL;

        if (strcmp(arg, "--help") == 0) {
            usage(argv[0]);
            return 0;
        }
        if (strncmp(arg, "--mode=", 7) == 0) {
            mode = arg + 7;
        } else if (strcmp(arg, "--mode") == 0) {
            if (i + 1 >= argc) {
                usage(argv[0]);
                return 1;
            }
            mode = argv[++i];
        } else {
            usage(argv[0]);
            return 1;
        }

        if (mode && !set_mode(mode)) {
            fprintf(stderr, "Unknown mode: %s\n", mode);
            usage(argv[0]);
            return 1;
        }
    }

    // Prompt for Accessibility permission if not already granted
    const void *k[] = { kAXTrustedCheckOptionPrompt }, *v[] = { kCFBooleanTrue };
    CFDictionaryRef opts = CFDictionaryCreate(NULL, k, v, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    bool ok = AXIsProcessTrustedWithOptions(opts);
    CFRelease(opts);
    if (!ok) { fprintf(stderr, "Grant Accessibility permission, then re-run.\n"); return 1; }

    CGEventMask mask = 0;
    if (interceptShortcuts) {
        mask |= (1ULL << kCGEventKeyDown) | (1ULL << kCGEventKeyUp);
    }
    if (interceptTrackpad) {
        mask |= (1ULL << kCGSEventGesture) | (1ULL << kCGSEventDockControl);
    }

    // Listen for gesture + dock control events
    tap = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap,
        kCGEventTapOptionDefault,
        mask,
        cb, NULL);
    if (!tap) { fprintf(stderr, "Failed to create event tap.\n"); return 1; }

    CFRunLoopSourceRef src = CFMachPortCreateRunLoopSource(NULL, tap, 0);
    CFRunLoopAddSource(CFRunLoopGetMain(), src, kCFRunLoopCommonModes);
    CGEventTapEnable(tap, true);
    signal(SIGINT, stop); signal(SIGTERM, stop);

    fprintf(stderr, "iss: instant swipe active (%s)\n", mode_name());
    while (running) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, true);

    CGEventTapEnable(tap, false);
    CFRunLoopRemoveSource(CFRunLoopGetMain(), src, kCFRunLoopCommonModes);
    CFRelease(src); CFRelease(tap);
    return 0;
}
