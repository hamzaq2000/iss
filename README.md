# iss — Instant Space Switcher

Eliminates the macOS sliding animation when switching spaces.
Can hook trackpad 3-finger swipes, `Ctrl`+Left / `Ctrl`+Right, or both.
That includes Logitech Options+ "Switch between desktops" button actions.

## How it works

`iss` installs a session-level `CGEventTap` for the private DockControl and companion gesture events used by horizontal trackpad swipes. It suppresses the real horizontal swipe, detects its direction, and posts a synthetic gesture sequence. Vertical swipes, including Mission Control and App Exposé, pass through.

It also intercepts `Ctrl`+Left / `Ctrl`+Right and routes them through the same
synthetic dock-swipe path. This matters for Logitech Options+: its native
"Desktop left/right" actions emit those keyboard shortcuts rather than real
trackpad swipe gestures.

On macOS versions before 27, the synthetic sequence uses the original DockControl fields and high velocity. On macOS 27 and later, `CGEventPost` events need an embedded raw IOHID queue payload in serialized CGEvent field 4205. `iss` constructs the payload, including the fluid-touch and velocity records, appends it to each synthetic phase, and posts the augmented events. Began and Changed carry no progress; End follows 50 ms later with the full progress, which makes the Dock jump straight to the target space instead of animating.

Each DockControl event is paired with a companion gesture event. A passthrough counter lets those synthetic events pass through the tap without being intercepted again. The real terminal event is also allowed to complete the Dock’s native gesture state on macOS 27.

Switching past the first or last space is blocked before any event is posted. `CGSGetActiveSpace()` can lag behind the Dock after a synthetic switch, so `iss` treats the target of its own last switch as the current space until that API catches up (or 1 s passes).

No SIP disable or code injection is required. The event type and field indices are undocumented system details, and the macOS 27 IOHID payload layout is reverse-engineered.

Other keyboard shortcuts are left untouched.

## Compatibility

The pre-27 event path supports the macOS versions where the original synthetic DockControl mechanism works. macOS 27 and later use the serialized IOHID payload path described above. Direction encoding differs between macOS 26 and macOS 27, so the running OS—not the SDK used to build `iss`—determines the interpretation.

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
