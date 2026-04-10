# iss — Instant Space Switcher

Eliminates the macOS sliding animation when switching spaces.
Can hook trackpad 3-finger swipes, `Ctrl`+Left / `Ctrl`+Right, or both.
That includes Logitech Options+ "Switch between desktops" button actions.

## How it works

A CGEventTap intercepts trackpad dock-swipe gestures before the Dock sees them,
suppresses the originals, and posts synthetic Begin+End gesture events with
high velocity (±400). The Dock treats these as instantaneous swipes and switches
spaces with no animation.

It also intercepts `Ctrl`+Left / `Ctrl`+Right and routes them through the same
synthetic dock-swipe path. This matters for Logitech Options+: its native
"Desktop left/right" actions emit those keyboard shortcuts rather than real
trackpad swipe gestures.

**No SIP disable required.** CGEventTap and CGEventPost are public, supported APIs — the same mechanism macOS uses for Accessibility features. No code injection, no DYLD tricks, no system file modification. The only undocumented parts are the CGEvent field indices (55, 110, 132, etc.) used to read/write gesture metadata, which are just integer constants passed to public functions. SIP has nothing to protect here.

Vertical swipes (Mission Control, App Exposé) are left untouched.
Other keyboard shortcuts are left untouched.

## Compatibility

Should work on all macOS since 10.11 El Capitan.
The private event field indices and dock-swipe HID type have been stable since 3-finger swipe was introduced.

## Install

```sh
# interactive: prompts for both / trackpad / shortcuts
make install

# non-interactive
make install MODE=both
make install MODE=trackpad
make install MODE=shortcuts

# use sudo make install PREFIX=/usr/local if you want but it's not necessary
```

`make install` writes the selected mode into the launch agent, so it persists
across reboots. If stdin is not interactive and `MODE` is omitted, it defaults
to `both`.

Runs automatically at login via launchd. Grant Accessibility permission when prompted.

## Uninstall

```
make uninstall
```
