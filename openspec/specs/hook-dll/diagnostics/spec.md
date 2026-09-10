## Purpose

Covers the evidence the hook leaves behind: a log that survives a hard crash and stays small enough to send, and a self-reporting crash handler that captures a symbolisable report, a minidump, and a snapshot of the log at the moment the game faults — because the people who hit these crashes cannot be asked to attach a debugger.

## Requirements

### Requirement: Log to a file the player can find and send

The hook SHALL write a log file into the client's own data folder, falling back to the system temporary folder when that folder cannot be resolved. The fallback SHALL itself be reported in the log.

The log SHALL be appended to rather than truncated: the DLL is loaded and unloaded on every aborted attach while the injector retries, and truncating would discard the previous session several times over before a player who just crashed could collect it.

Each session SHALL begin with a banner identifying the process, and the log SHALL be rotated when it grows past a bounded size, keeping exactly one previous generation.

#### Scenario: Repeated aborted attaches

- **WHEN** the injector retries several times while the game is still loading
- **THEN** each attempt appends to the same log and no earlier content is lost

#### Scenario: Log grows large

- **WHEN** the log exceeds its size limit at startup
- **THEN** it is rotated to a single previous generation and a fresh log is started

### Requirement: Make the log usable from many threads and readable after a crash

Every log line SHALL carry a timestamp, a severity, and the identifier of the thread that wrote it — the hook runs on the game's update thread, on the render thread, and on its own polling threads, and the races worth catching are only visible as an interleaving.

Logging SHALL be safe to call concurrently from all of those threads.

Anything above informational severity SHALL be flushed to disk immediately, so that the tail of the log survives a hard crash. Until initialisation completes, every line SHALL be flushed.

Consecutive identical messages SHALL be collapsed into a single line plus a repeat count, so that per-frame logging cannot fill the file.

#### Scenario: Warning before a crash

- **WHEN** a warning is logged and the game faults immediately afterwards
- **THEN** that warning is present in the log file on disk

#### Scenario: A message repeats every frame

- **WHEN** the same message is logged many times in a row
- **THEN** it appears once, followed by a line stating how many times it repeated

### Requirement: Logging must be safe from any initialisation order

The logging facility SHALL be usable from another translation unit's static initialiser, which runs before the DLL's entry point. Writing to an unconstructed log there is undefined behaviour that happens before any log file exists, leaving a crash with no trace at all.

#### Scenario: Export resolution logs during static initialisation

- **WHEN** a missing game export is reported from a static initialiser
- **THEN** the log is constructed on demand and the message is recorded

### Requirement: Write a crash report when the game faults

The hook SHALL install a crash handler once it has committed to hooking, and SHALL remove it before the module unloads — a handler pointing into an unmapped module is a guaranteed crash rather than a report of one. In particular, an aborted attach MUST NOT leave a handler installed.

On a fatal fault the handler SHALL write, under one shared timestamped name in the client's data folder: a minidump, a text report, and a snapshot of the hook log. The name SHALL be timestamped rather than numbered, because process identifiers are recycled and a collision would overwrite an earlier crash.

The number of reports written per process SHALL be bounded. Non-fatal exceptions SHALL be passed through untouched.

#### Scenario: Game faults with the hook attached

- **WHEN** a fatal exception reaches the handler
- **THEN** a minidump, a report, and a log snapshot sharing one timestamped name appear in the client's data folder

#### Scenario: Attach was aborted

- **WHEN** the DLL aborts its attach and is unloaded
- **THEN** no crash handler remains installed

#### Scenario: Non-fatal exception

- **WHEN** an exception the handler does not consider fatal occurs
- **THEN** the handler writes nothing and lets normal exception handling continue

### Requirement: The crash handler must be safe on a faulting thread

The handler runs on a thread that is already faulting. It SHALL resolve its output folder once at install time, and during the fault it SHALL NOT allocate, SHALL NOT take a lock, and SHALL NOT use the ordinary logging facility — whose lock the faulting thread may already hold. It SHALL write through the lowest-level file operations available.

On a stack overflow the handler SHALL capture only the minidump and skip building the text report, because building it on an exhausted stack turns a diagnosable fault into a second one.

#### Scenario: Fault while holding the log lock

- **WHEN** the faulting thread already holds the log's lock
- **THEN** the handler still completes and writes its files

#### Scenario: Stack overflow

- **WHEN** the fault is a stack overflow
- **THEN** a minidump is written and no text report is attempted

### Requirement: Record what the hook was doing before the fault

The hook SHALL record short breadcrumb entries at the points where it calls into the game, and replay them in the crash report. Recording a breadcrumb SHALL be cheap enough to do on every intercepted call: no allocation and no lock.

The hook SHALL additionally log a line each time the world transitions between alive and dead, carrying the pointers needed to correlate the log against a crash dump and how long ago the player last picked something up. This SHALL be logged on transitions only, so it is safe to check every frame, and SHALL be recorded independently of whether item capture is active so that "attached and active" and "attached but idle" sessions are comparable.

#### Scenario: World tears down

- **WHEN** the world transitions from alive to dead
- **THEN** exactly one line is logged recording the transition, the relevant game pointers, and the time since the last item was added

#### Scenario: World state unchanged

- **WHEN** the world state is checked on a frame where it did not change
- **THEN** nothing is logged

### Requirement: Absence of a crash report is itself a finding

Some failures — heap corruption and stack-cookie violations — raise a fail-fast that bypasses the crash handler entirely. This limitation SHALL be documented, so that repeated crashes with no report produced is read as evidence about the kind of fault rather than as a broken handler.

#### Scenario: Repeated crashes with no reports

- **WHEN** players report crashes but no crash report files are ever produced
- **THEN** the documented conclusion is that the fault is a fail-fast rather than an access violation
