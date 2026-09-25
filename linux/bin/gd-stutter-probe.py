#!/usr/bin/env python3
"""Sample what the game is stalling ON, so a freeze can be attributed instead of guessed.

Run it, reproduce the freeze, read the summary. Every signal here is readable without root.

    gd-stutter-probe.py            # 60s
    gd-stutter-probe.py 30         # 30s
    gd-stutter-probe.py 60 log.tsv # also write every sample for later

The four candidates for a movement-triggered freeze, and what separates them:

    shader compilation  DXVK state cache grows, CPU pressure rises
    CPU starvation      CPU pressure rises, cache does NOT grow
    asset streaming     IO pressure rises, read bytes climb, threads sit in D
    box64 translation   none of the above move, yet frames are clearly lost

That last row is the important one: box64's dynarec has no counter to read, so it is what
remains when the other three stay flat. It is a diagnosis by elimination, not by measurement.

PSI counters are per-cgroup, so this measures the GAME's session rather than the whole box -
a busy compile on another core will not show up here and pollute the reading.
"""

import os
import subprocess
import sys
import time

INTERVAL = 0.2
DXVK_CACHE = os.path.expanduser(
    "~/Games/grim-dawn-classic-box64/dxvk-cache/Grim Dawn.dxvk-cache")

# A sample is "bad" if the game was stalled for more than a quarter of the window. At 60fps
# that is a dozen frames gone, which is well past the point of being visible.
BAD_STALL_MS = INTERVAL * 1000 * 0.25


def game_pid():
    result = subprocess.run(["pgrep", "-f", "Grim Dawn[.]exe"],
                            capture_output=True, text=True)
    pids = result.stdout.split()
    return int(pids[0]) if pids else None


def cgroup_path(pid):
    with open(f"/proc/{pid}/cgroup") as handle:
        for line in handle:
            if line.startswith("0::"):
                return "/sys/fs/cgroup" + line.strip()[3:]
    return None


def read_total(path):
    """PSI 'total' is cumulative microseconds stalled - deltas are what matter."""
    try:
        with open(path) as handle:
            for line in handle:
                if line.startswith("some"):
                    for field in line.split():
                        if field.startswith("total="):
                            return int(field[6:])
    except OSError:
        pass
    return 0


def read_chars(pid):
    try:
        with open(f"/proc/{pid}/io") as handle:
            for line in handle:
                if line.startswith("rchar:"):
                    return int(line.split()[1])
    except OSError:
        pass
    return 0


def thread_states(pid):
    """R means runnable-but-maybe-not-running (contention); D means blocked on I/O."""
    running = blocked = 0
    try:
        tids = os.listdir(f"/proc/{pid}/task")
    except OSError:
        return 0, 0

    for tid in tids:
        try:
            with open(f"/proc/{pid}/task/{tid}/stat") as handle:
                state = handle.read().rsplit(")", 1)[1].split()[0]
        except (OSError, IndexError):
            continue

        if state == "R":
            running += 1
        elif state == "D":
            blocked += 1

    return running, blocked


def cache_size():
    try:
        return os.path.getsize(DXVK_CACHE)
    except OSError:
        return 0


def main():
    duration = float(sys.argv[1]) if len(sys.argv) > 1 else 60.0
    out = open(sys.argv[2], "w") if len(sys.argv) > 2 else None

    pid = game_pid()
    if pid is None:
        raise SystemExit("Grim Dawn is not running")

    cgroup = cgroup_path(pid)
    if cgroup is None:
        raise SystemExit(f"could not resolve the cgroup of pid {pid}")

    pressure = {name: f"{cgroup}/{name}.pressure" for name in ("cpu", "io", "memory")}
    print(f"pid {pid}, sampling {duration:.0f}s - reproduce the freeze now\n")
    print(f"{'time':>6} {'cpu ms':>7} {'io ms':>7} {'mem ms':>7} "
          f"{'shader B':>9} {'read MB':>8} {'R':>3} {'D':>3}")

    previous = {name: read_total(path) for name, path in pressure.items()}
    previous_chars = read_chars(pid)
    previous_cache = cache_size()

    started = time.monotonic()
    samples = []

    while time.monotonic() - started < duration:
        time.sleep(INTERVAL)

        now = {name: read_total(path) for name, path in pressure.items()}
        stalls = {name: (now[name] - previous[name]) / 1000.0 for name in now}
        previous = now

        chars = read_chars(pid)
        read_mb = (chars - previous_chars) / 1048576.0
        previous_chars = chars

        cache = cache_size()
        shader_growth = cache - previous_cache
        previous_cache = cache

        running, blocked = thread_states(pid)
        elapsed = time.monotonic() - started
        bad = stalls["cpu"] > BAD_STALL_MS or stalls["io"] > BAD_STALL_MS

        line = (f"{elapsed:6.1f} {stalls['cpu']:7.1f} {stalls['io']:7.1f} "
                f"{stalls['memory']:7.1f} {shader_growth:9d} {read_mb:8.2f} "
                f"{running:3d} {blocked:3d}")

        if bad:
            print(line + "  <-- stall")
        if out:
            out.write(line + "\n")

        samples.append((stalls, shader_growth, read_mb, blocked))

    if out:
        out.close()

    bad_samples = [s for s in samples
                   if s[0]["cpu"] > BAD_STALL_MS or s[0]["io"] > BAD_STALL_MS]

    print(f"\n{len(bad_samples)} stalled samples out of {len(samples)}")
    if not bad_samples:
        print("Nothing stalled. Either the freeze did not happen, or it is box64 translating\n"
              "code - that blocks inside the process and never reaches these counters.")
        return

    cpu = sum(s[0]["cpu"] for s in bad_samples)
    io = sum(s[0]["io"] for s in bad_samples)
    shaders = sum(s[1] for s in bad_samples)
    read = sum(s[2] for s in bad_samples)
    blocked = sum(s[3] for s in bad_samples)

    print(f"  cpu stalled   {cpu:9.0f} ms")
    print(f"  io stalled    {io:9.0f} ms")
    print(f"  shader cache  {shaders:9d} bytes added")
    print(f"  read          {read:9.1f} MB")
    print(f"  threads in D  {blocked:9d} (summed across samples)")

    # Deliberately phrased as "points at", not "is". These signals correlate with the stall;
    # they do not prove the cause, and two of them can move together.
    if shaders > 0 and cpu > io:
        print("\nPoints at SHADER COMPILATION: the pipeline cache grew while the CPU stalled.")
    elif cpu > io:
        print("\nPoints at CPU CONTENTION with no new pipelines. Check whether DXVK's compiler\n"
              "threads are oversubscribing the 10 cores the launcher pins the game to.")
    else:
        print("\nPoints at DISK: reproduce with `iostat -x 1` alongside to confirm.")


if __name__ == "__main__":
    main()
