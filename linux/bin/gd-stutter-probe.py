#!/usr/bin/env python3
"""Sample what the game is stalling ON, so a freeze can be attributed instead of guessed.

Run it, reproduce the freeze, read the summary. Every signal here is readable without root.

    gd-stutter-probe.py                  # 60s
    gd-stutter-probe.py 30               # 30s
    gd-stutter-probe.py 60 run.tsv       # also write every sample for later

THE DECISIVE QUESTION IS WHETHER THE GAME IS COMPUTING OR WAITING.

A freeze while burning CPU and a freeze while idle have completely different causes, and every
other signal here only refines the answer:

    computing  shader compilation, box64 translating code, CPU contention
    waiting    the Wine message pump, an input event that never arrives, the driver

That matters on this box specifically. The launcher's own comments record an earlier bout of
"freeze on area transitions with the Xbox pad, unfreezing when the mouse nudges the message
pump" - which is the WAITING kind, and would look like nothing at all to a probe that only
watched pressure counters.

So the headline column is `used ms`: CPU milliseconds the game actually consumed in each
window. Near zero while the screen is frozen means it is blocked, not working.

    shader compilation   used ms high, DXVK state cache grows
    box64 translating    used ms high, cache flat, cpu pressure low
    CPU contention       used ms suppressed, cpu pressure high
    blocked / pump       used ms near zero, all pressure flat, threads in S
    asset streaming      io pressure high, threads in D, read MB climbs

PSI counters are read per-cgroup, so this measures the GAME's session rather than the whole
box - a busy compile elsewhere cannot pollute the reading.
"""

import os
import statistics
import subprocess
import sys
import time

INTERVAL = 0.2
TICKS = os.sysconf("SC_CLK_TCK")
DXVK_CACHE = os.path.expanduser(
    "~/Games/grim-dawn-classic-box64/dxvk-cache/Grim Dawn.dxvk-cache")

# A sample is stalled if the game lost more than a quarter of the window. At 60fps that is a
# dozen frames, well past being visible.
BAD_STALL_MS = INTERVAL * 1000 * 0.25

# ... and it is "idle" if it used less than a quarter of its own typical CPU. A frozen screen
# with idle threads is the signature of waiting rather than working.
IDLE_FRACTION = 0.25


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


def cpu_ms(pid):
    """utime + stime for the whole process, including every thread that has exited."""
    try:
        with open(f"/proc/{pid}/stat") as handle:
            fields = handle.read().rsplit(")", 1)[1].split()
    except (OSError, IndexError):
        return 0.0

    # After the comm field, index 11 and 12 are utime and stime.
    return (int(fields[11]) + int(fields[12])) * 1000.0 / TICKS


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
    """R is runnable-but-maybe-not-running (contention); D is blocked on I/O."""
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


def classify(samples):
    """Name the most likely cause of the flagged samples, without overstating it."""
    flagged = [s for s in samples if s["flagged"]]
    if not flagged:
        return None

    stalled_cpu = sum(s["cpu"] for s in flagged)
    stalled_io = sum(s["io"] for s in flagged)
    shaders = sum(s["shader"] for s in flagged)
    blocked = sum(s["blocked"] for s in flagged)
    idle = sum(1 for s in flagged if s["idle"])

    if stalled_io > stalled_cpu and blocked:
        return ("DISK", "IO pressure dominated and threads sat in uninterruptible sleep.")

    if idle > len(flagged) / 2:
        return ("BLOCKED", "The game used almost no CPU while frozen, so it was waiting, not\n"
                           "working. Shader compilation and box64 translation are both ruled\n"
                           "out - they burn CPU. This matches the input-thread / message-pump\n"
                           "stall the launcher's comments describe. Try experiment 4 (play\n"
                           "with the pad unplugged) to confirm the input path is involved.")

    if shaders > 0:
        return ("SHADERS", "The pipeline cache grew while the game stalled on CPU.")

    if stalled_cpu > 0:
        return ("CPU CONTENTION", "The game burned CPU and still lost frames, with no new\n"
                                  "pipelines. DXVK's 20 compiler threads share the 10 fast\n"
                                  "cores the launcher pins the game to - try experiment 3.")

    return ("COMPUTING", "The game was busy but nothing external stalled it. That is what\n"
                         "box64 translating new code looks like: it blocks inside the\n"
                         "process and reaches no counter here. Try experiment 2.")


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
    header = (f"{'time':>6} {'used ms':>8} {'cpu ms':>7} {'io ms':>7} {'mem ms':>7} "
              f"{'shader B':>9} {'read MB':>8} {'R':>3} {'D':>3}")

    print(f"pid {pid}, sampling {duration:.0f}s - reproduce the freeze now\n")
    print(header)
    if out:
        out.write(header + "\n")

    previous = {name: read_total(path) for name, path in pressure.items()}
    previous_chars = read_chars(pid)
    previous_cache = cache_size()
    previous_cpu = cpu_ms(pid)

    started = time.monotonic()
    samples = []

    while time.monotonic() - started < duration:
        time.sleep(INTERVAL)

        now = {name: read_total(path) for name, path in pressure.items()}
        stalls = {name: (now[name] - previous[name]) / 1000.0 for name in now}
        previous = now

        used = cpu_ms(pid)
        used_delta = used - previous_cpu
        previous_cpu = used

        chars = read_chars(pid)
        read_mb = (chars - previous_chars) / 1048576.0
        previous_chars = chars

        cache = cache_size()
        shader_growth = cache - previous_cache
        previous_cache = cache

        running, blocked = thread_states(pid)

        samples.append({
            "elapsed": time.monotonic() - started,
            "used": used_delta,
            "cpu": stalls["cpu"],
            "io": stalls["io"],
            "memory": stalls["memory"],
            "shader": shader_growth,
            "read": read_mb,
            "running": running,
            "blocked": blocked,
        })

    # Idleness is relative to how much CPU this machine's Grim Dawn normally uses, which is
    # why it is decided after the run rather than against a guessed constant.
    typical = statistics.median([s["used"] for s in samples]) if samples else 0.0

    for sample in samples:
        sample["idle"] = typical > 0 and sample["used"] < typical * IDLE_FRACTION
        sample["flagged"] = (sample["cpu"] > BAD_STALL_MS
                             or sample["io"] > BAD_STALL_MS
                             or sample["idle"])

        line = (f"{sample['elapsed']:6.1f} {sample['used']:8.1f} {sample['cpu']:7.1f} "
                f"{sample['io']:7.1f} {sample['memory']:7.1f} {sample['shader']:9d} "
                f"{sample['read']:8.2f} {sample['running']:3d} {sample['blocked']:3d}")

        if sample["flagged"]:
            print(line + ("  <-- idle" if sample["idle"] else "  <-- stall"))
        if out:
            out.write(line + "\n")

    if out:
        out.close()

    flagged = [s for s in samples if s["flagged"]]
    print(f"\ntypical CPU use {typical:.0f} ms per {INTERVAL * 1000:.0f} ms window")
    print(f"{len(flagged)} flagged samples out of {len(samples)}")

    verdict = classify(samples)
    if verdict is None:
        print("\nNothing flagged. Either the freeze did not happen during the run, or it is\n"
              "shorter than one 200 ms sample.")
        return

    name, reason = verdict
    print(f"\nPoints at {name}:\n{reason}")


if __name__ == "__main__":
    main()
