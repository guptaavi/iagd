# Running this fork on Linux (box64 + Wine, ARM64)

Host-side scripts for the machine this fork was built for: Grim Dawn (GOG) under
box64 + x86_64 Wine 11.16 on ARM64, streamed with Sunshine. They live outside the
repo in normal use, which makes them the easiest part of the setup to lose.

Paths below are the ones the scripts assume; adjust for another machine.

## bin/

| script | installed as | purpose |
|---|---|---|
| `gd-box64-wine.sh` | `~/.local/bin/` | runs any Windows program in the GAME'S prefix and box64/Wine stack |
| `launch-iagd-dev.sh` | `~/.local/bin/` | starts the forked build from `C:\Program Files\IAGD-dev\` |
| `launch-iagd-box64.sh` | `~/.local/bin/` | starts the stock upstream install, kept as a fallback |
| `launch-grimdawn-box64.sh` | `/usr/local/bin/` | the game launcher; also auto-starts Item Assistant |
| `iagd-pad.py` | `~/.local/bin/` | toggles Item Assistant from a chord, and drives it with the pad |

Two things in these scripts are not obvious and were both found the hard way:

- **Working directory matters.** IAGrim resolves `hibernate.sqlite.cfg.xml` relative to the
  CWD, so it must start with its install folder as the working directory or it dies at startup
  with an NHibernate configuration exception.
- **The prefix matters more.** Item Assistant has to run in the GAME's Wine prefix, or it
  cannot see the stash or inject its hook. The Lutris prefix is NOT usable: its system32 holds
  ARM64 PEs, which an x86_64 process cannot load.

## systemd/

`iagd-pad.service` runs `iagd-pad.py` as a user service. It does two things.

**The chord** launches Item Assistant, raises it over the game, or pushes it back. It is read
while the pad is NOT grabbed, so Grim Dawn sees the same presses: the chord must be one the game
ignores. Default is L3+R3; override with `IAGD_PAD_CHORD` in the unit (any of
BTN_A/B/X/Y/TL/TR/SELECT/START/MODE/THUMBL/THUMBR).

**Pad-as-keyboard** engages only while Item Assistant has focus, and is Steam Input's trick: the
pad is grabbed exclusively and re-emitted on a virtual keyboard, so the WinForms UI just sees
keystrokes. Set `IAGD_PAD_CAPTURE=0` to disable it and keep only the toggle.

| pad | key | in the item grid |
|---|---|---|
| left stick / d-pad | arrows | move the selected item, move within a filter |
| LB / RB | PgUp / PgDn | page the list |
| A | Enter | activate |
| B | Esc | back |
| X | Space | toggle the focused checkbox |
| Y / Back | Tab / Shift+Tab | next / previous control |
| Start | Home | top of the list |

Three constraints that shaped this, each a real failure mode:

- **The grab must be scoped to focus.** Grabbing while the GAME is focused takes the controller
  away from Grim Dawn. Not grabbing while ITEM ASSISTANT is focused is just as wrong: Wine reads
  evdev directly and ignores X focus, so the game would act on the same stick pushes that are
  scrolling the item list. The consequence is intended - while the window is up, the game gets
  no controller input at all.
- **Emit through uinput, not `xdotool key --window`.** Wine ignores keystrokes synthesised with
  XSendEvent. Creating the virtual keyboard needs write access to `/dev/uinput`, which the
  user's `input` group membership already grants - no root.
- **Do not add an autorepeat.** Held buttons are mirrored as held keys and the OS repeats them;
  repeating here too would double it.

No mapping types text, so the search box still wants a keyboard.

The pad is virtual and created by Sunshine per streaming session, so it appears and disappears
with the stream; the daemon watches for it rather than assuming it is present at start.

## Building and deploying

Built on another machine (an x86_64 Linux box) and copied over - the output is MSIL and
framework-dependent, so it is architecture independent:

    dotnet build IAGrim/IAGrim.csproj -c Release -p:EnableWindowsTargeting=true
    rsync -a --exclude '*.pdb' IAGrim/bin/Release/net10.0-windows/win-x64/ \
        "<prefix>/drive_c/Program Files/IAGD-dev/"

The prefix also needs the .NET 10 Desktop Runtime (x64) installed INTO it. WebView2 is not
needed: the fork does not start it (see the wine-native-grid branch).
