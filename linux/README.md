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
| `iagd-pad.py` | `~/.local/bin/` | toggles Item Assistant from a controller chord |

Two things in these scripts are not obvious and were both found the hard way:

- **Working directory matters.** IAGrim resolves `hibernate.sqlite.cfg.xml` relative to the
  CWD, so it must start with its install folder as the working directory or it dies at startup
  with an NHibernate configuration exception.
- **The prefix matters more.** Item Assistant has to run in the GAME's Wine prefix, or it
  cannot see the stash or inject its hook. The Lutris prefix is NOT usable: its system32 holds
  ARM64 PEs, which an x86_64 process cannot load.

## systemd/

`iagd-pad.service` runs `iagd-pad.py` as a user service. It reads the gamepad PASSIVELY and
never grabs it - grabbing would take the controller away from the game - so Grim Dawn sees the
same presses and the chord must be one the game ignores. Default is L3+R3; override with
`IAGD_PAD_CHORD` in the unit (any of BTN_A/B/X/Y/TL/TR/SELECT/START/MODE/THUMBL/THUMBR).

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
