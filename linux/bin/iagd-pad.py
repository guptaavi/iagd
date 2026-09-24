#!/usr/bin/env python3
"""Toggle Item Assistant from the controller, and drive it with the pad once it is up.

Two jobs:

1. A chord (L3+R3 by default) launches Item Assistant, raises it over the game, or pushes it
   back behind.
2. While Item Assistant HAS FOCUS, the pad drives it as a keyboard - sticks and d-pad become
   arrows, A becomes Enter, and so on.

The second job borrows Steam Input's trick: nothing here teaches the WinForms UI about
controllers. The pad is captured exclusively and its events are re-emitted on a virtual
KEYBOARD, so the app sees ordinary keystrokes and cannot tell the difference.

Capture is scoped precisely to "Item Assistant is the focused window", and both halves of that
matter:

- Grabbing while the GAME is focused would take the controller away from Grim Dawn. That is the
  failure this daemon's original passive design existed to avoid.
- NOT grabbing while ITEM ASSISTANT is focused is equally wrong. Wine reads evdev directly and
  does not care which window X considers focused, so the game would keep acting on the same
  stick pushes that are scrolling the item list.

Synthetic keys go through uinput rather than `xdotool key --window`, because Wine ignores
keystrokes synthesised with XSendEvent.

Held buttons are mirrored as held keys and the OS provides autorepeat, so a held d-pad walks
the item list at the user's own repeat rate. Repeating here as well would double it.

Text entry is out of scope: no button mapping types "Albre" into the search box. The pad
browses; the keyboard types.

The pad itself is virtual and created by Sunshine per streaming session, so it appears and
disappears with the stream; this reopens it rather than assuming it is there at start.

Set IAGD_PAD_CAPTURE=0 to keep only the toggle, if the mapping ever gets in the way.
"""

import os
import select
import subprocess
import time

import evdev
from evdev import UInput, ecodes

CHORD = os.environ.get("IAGD_PAD_CHORD", "BTN_THUMBL+BTN_THUMBR")
CAPTURE = os.environ.get("IAGD_PAD_CAPTURE", "1") != "0"
LAUNCHER = os.path.expanduser("~/.local/bin/launch-iagd-dev.sh")
WINDOW_TITLE = "Grim Dawn Item Assistant"
DISPLAY = os.environ.get("DISPLAY", ":0")

# How often to ask X which window has focus. Only runs while a pad is connected, so this stays
# idle during normal desktop use.
FOCUS_POLL = 0.4

# Chosen for the native item grid: arrows move the selected item, PgUp/PgDn page it, Tab walks
# the filter controls, Space toggles the focused checkbox.
BUTTON_KEYS = {
    ecodes.BTN_SOUTH: (ecodes.KEY_ENTER,),
    ecodes.BTN_EAST: (ecodes.KEY_ESC,),
    ecodes.BTN_WEST: (ecodes.KEY_SPACE,),
    ecodes.BTN_NORTH: (ecodes.KEY_TAB,),
    ecodes.BTN_TL: (ecodes.KEY_PAGEUP,),
    ecodes.BTN_TR: (ecodes.KEY_PAGEDOWN,),
    ecodes.BTN_SELECT: (ecodes.KEY_LEFTSHIFT, ecodes.KEY_TAB),
    ecodes.BTN_START: (ecodes.KEY_HOME,),
}

# (negative direction, positive direction). evdev Y grows downward, hence UP on negative.
AXIS_KEYS = {
    ecodes.ABS_X: (ecodes.KEY_LEFT, ecodes.KEY_RIGHT),
    ecodes.ABS_Y: (ecodes.KEY_UP, ecodes.KEY_DOWN),
    ecodes.ABS_HAT0X: (ecodes.KEY_LEFT, ecodes.KEY_RIGHT),
    ecodes.ABS_HAT0Y: (ecodes.KEY_UP, ecodes.KEY_DOWN),
}

HAT_AXES = (ecodes.ABS_HAT0X, ecodes.ABS_HAT0Y)


def log(message):
    print(f"{time.strftime('%H:%M:%S')} {message}", flush=True)


def chord_codes():
    names = [n.strip() for n in CHORD.split("+")]
    codes = []
    for name in names:
        code = getattr(ecodes, name, None)
        if code is None:
            raise SystemExit(f"unknown button in IAGD_PAD_CHORD: {name}")
        codes.append(code)
    return set(codes), names


def find_pad():
    """First device that reports gamepad buttons. Names vary; capabilities do not."""
    for path in evdev.list_devices():
        try:
            device = evdev.InputDevice(path)
        except OSError:
            continue

        keys = device.capabilities().get(ecodes.EV_KEY, [])
        if ecodes.BTN_GAMEPAD in keys and ecodes.BTN_THUMBL in keys:
            return device

        device.close()
    return None


def run(*args):
    return subprocess.run(args, capture_output=True, text=True,
                          env={**os.environ, "DISPLAY": DISPLAY})


def ia_running():
    return run("pgrep", "-f", "IAGrim[.]exe").returncode == 0


def ia_window():
    out = run("wmctrl", "-l").stdout
    for line in out.splitlines():
        if WINDOW_TITLE in line:
            return line.split()[0]
    return None


def active_window():
    return run("xdotool", "getactivewindow").stdout.strip()


def ia_has_focus():
    """One subprocess, and the title comes back with it - no window-id bookkeeping."""
    result = run("xdotool", "getactivewindow", "getwindowname")
    if result.returncode != 0:
        return False

    lines = result.stdout.strip().splitlines()
    return bool(lines) and WINDOW_TITLE in lines[-1]


def toggle():
    """Launch if absent; otherwise raise it, or push it behind if it already has focus."""
    if not ia_running():
        log("launching Item Assistant")
        subprocess.Popen([LAUNCHER], start_new_session=True,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         env={**os.environ, "DISPLAY": DISPLAY})
        return

    window = ia_window()
    if window is None:
        log("running, but no window yet")
        return

    # Window ids from wmctrl are 0x0a000003; xdotool prints decimal.
    focused = active_window()
    same = focused.isdigit() and int(focused) == int(window, 16)

    if same:
        log(f"hiding {window}")
        run("xdotool", "windowminimize", str(int(window, 16)))
    else:
        log(f"raising {window}")
        run("wmctrl", "-i", "-a", window)


class Keyboard:
    """A virtual keyboard, plus enough bookkeeping to survive two sources of one key.

    The d-pad and the left stick both produce KEY_DOWN. Without reference counting, releasing
    the d-pad would release a key the stick is still holding.
    """

    def __init__(self):
        keys = sorted({key for combo in BUTTON_KEYS.values() for key in combo}
                      | {key for pair in AXIS_KEYS.values() for key in pair})
        self.ui = UInput({ecodes.EV_KEY: keys}, name="iagd-pad-keyboard")
        self.holders = {}

    def press(self, source, keys):
        for key in keys:
            holders = self.holders.setdefault(key, set())
            if not holders:
                self.ui.write(ecodes.EV_KEY, key, 1)
            holders.add(source)
        self.ui.syn()

    def release(self, source, keys):
        for key in reversed(keys):
            holders = self.holders.get(key)
            if not holders or source not in holders:
                continue

            holders.discard(source)
            if not holders:
                self.ui.write(ecodes.EV_KEY, key, 0)
        self.ui.syn()

    def release_all(self):
        """Never leave a key stuck down - a stuck key outlives this process."""
        for key, holders in self.holders.items():
            if holders:
                holders.clear()
                self.ui.write(ecodes.EV_KEY, key, 0)
        self.ui.syn()

    def close(self):
        self.release_all()
        self.ui.close()


class Mapper:
    """Translates pad events into key events while capture is on."""

    def __init__(self, keyboard, skip_codes):
        self.keyboard = keyboard
        self.skip = skip_codes
        self.axis = {}
        self.thresholds = {}

    def learn_axes(self, device):
        """Stick range varies by pad; the hat is always -1/0/1 and needs no thresholds."""
        self.thresholds.clear()
        for code in (ecodes.ABS_X, ecodes.ABS_Y):
            try:
                info = device.absinfo(code)
            except OSError:
                continue

            span = max(abs(info.min), abs(info.max)) or 32767
            # Hysteresis: cross 55% to engage, fall below 35% to let go, so a stick resting
            # near the threshold does not chatter.
            self.thresholds[code] = (int(span * 0.35), int(span * 0.55))

    def reset(self):
        self.axis.clear()
        self.keyboard.release_all()

    def button(self, code, value):
        if code in self.skip or code not in BUTTON_KEYS:
            return

        # value 2 is the device's own autorepeat; the key is already held.
        if value == 1:
            self.keyboard.press(("btn", code), BUTTON_KEYS[code])
        elif value == 0:
            self.keyboard.release(("btn", code), BUTTON_KEYS[code])

    def direction(self, code, value):
        if code in HAT_AXES:
            return (value > 0) - (value < 0)

        low, high = self.thresholds.get(code, (11000, 18000))
        if value <= -high:
            return -1
        if value >= high:
            return 1
        if -low < value < low:
            return 0

        # Inside the hysteresis band: whatever it was, it still is.
        return self.axis.get(code, 0)

    def axis_moved(self, code, value):
        if code not in AXIS_KEYS:
            return

        new = self.direction(code, value)
        old = self.axis.get(code, 0)
        if new == old:
            return

        negative, positive = AXIS_KEYS[code]
        if old:
            self.keyboard.release((code, old), (negative if old < 0 else positive,))
        if new:
            self.keyboard.press((code, new), (negative if new < 0 else positive,))

        self.axis[code] = new


def main():
    codes, names = chord_codes()
    log(f"watching for {'+'.join(names)}")

    keyboard = None
    mapper = None
    if CAPTURE:
        keyboard = Keyboard()
        mapper = Mapper(keyboard, codes)
        log("pad-as-keyboard enabled while Item Assistant has focus")

    device = None
    pressed = set()
    fired = False
    captured = False
    next_focus_check = 0.0

    def set_capture(on):
        """Grab is scoped to the focused window - see the module docstring."""
        nonlocal captured
        if on == captured or device is None:
            return

        try:
            device.grab() if on else device.ungrab()
        except OSError as error:
            log(f"could not {'grab' if on else 'ungrab'} pad: {error}")
            return

        captured = on
        log("pad captured for Item Assistant" if on else "pad released to the game")
        if not on:
            mapper.reset()

    try:
        while True:
            if device is None:
                device = find_pad()
                if device is None:
                    time.sleep(2)
                    continue

                log(f"pad connected: {device.name}")
                pressed.clear()
                fired = False
                captured = False
                if mapper:
                    mapper.learn_axes(device)
                    mapper.reset()

            if mapper:
                now = time.monotonic()
                if now >= next_focus_check:
                    next_focus_check = now + FOCUS_POLL
                    set_capture(ia_has_focus())

            try:
                ready, _, _ = select.select([device.fd], [], [], 0.2)
                if not ready:
                    continue

                for event in device.read():
                    if event.type == ecodes.EV_KEY:
                        if event.value == 1:
                            pressed.add(event.code)
                        elif event.value == 0:
                            pressed.discard(event.code)

                        if codes <= pressed:
                            # One action per chord, not one per repeat.
                            if not fired:
                                fired = True
                                toggle()
                        else:
                            fired = False

                        if captured:
                            mapper.button(event.code, event.value)

                    elif event.type == ecodes.EV_ABS and captured:
                        mapper.axis_moved(event.code, event.value)

            except OSError:
                log("pad disconnected")
                if captured and mapper:
                    captured = False
                    mapper.reset()

                try:
                    device.close()
                except Exception:
                    pass

                device = None
                time.sleep(1)

    finally:
        if keyboard:
            keyboard.close()


if __name__ == "__main__":
    main()
