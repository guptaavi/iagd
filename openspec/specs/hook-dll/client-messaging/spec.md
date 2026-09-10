## Purpose

Defines how the injected DLL sends messages to the Item Assistant client without blocking the game: a thread-safe queue drained by a dedicated worker thread, delivered over a Windows message natively and over atomically-published files under Wine, using a message-type numbering shared with the client.

## Requirements

### Requirement: Never send to the client from a game thread

Code running inside a game hook SHALL only enqueue a message and signal the worker thread; it MUST NOT deliver the message itself. Delivery is synchronous and can block, and blocking a game thread stalls the game.

The queue SHALL be safe for concurrent use by the game's update thread, the render thread, and the DLL's own polling threads.

#### Scenario: A hook reports an event

- **WHEN** a hooked game function needs to report something to the client
- **THEN** it pushes a message onto the queue, signals the worker thread, and returns to the game immediately

#### Scenario: Concurrent producers

- **WHEN** several threads enqueue messages at the same time
- **THEN** every message is retained and none corrupt the queue

### Requirement: Deliver queued messages from a dedicated worker thread

The DLL SHALL run one worker thread that waits on a signal, then drains the queue completely before waiting again. The thread SHALL survive errors: any exception escaping message delivery is logged and MUST NOT terminate the process.

The worker thread SHALL announce itself with a "worker thread launched" message when it starts.

#### Scenario: Burst of messages

- **WHEN** several messages are enqueued before the worker thread wakes
- **THEN** all of them are delivered in one drain pass

#### Scenario: Delivery throws

- **WHEN** delivering a message raises an exception
- **THEN** the failure is logged and the worker thread continues

### Requirement: Track the client window natively, and drop messages when it is gone

When running natively the DLL SHALL locate the client's window by its window class and re-check for it at most once per second. Messages SHALL be delivered to that window as a copy-data message carrying the message type and payload.

If no client window is present, queued messages SHALL be discarded rather than accumulated — the client is not running, so nothing is waiting for them.

The presence or absence of the client window SHALL also drive whether item capture is active, so that the game does not capture items while the client is closed.

#### Scenario: Client is closed

- **WHEN** no client window can be found
- **THEN** queued messages are discarded, and item capture is deactivated

#### Scenario: Client starts after the game

- **WHEN** the client window appears while the game is already running
- **THEN** it is detected within roughly a second and item capture is activated

### Requirement: Bridge messages through files under Wine

Copy-data messages do not cross the Wine boundary, so when configured for Wine the DLL SHALL write each message as a binary file into a dedicated bridge folder instead of sending it to a window.

Each message file SHALL contain the message type, the payload length, and the payload bytes, in that order. Files SHALL be published atomically: written under a temporary name and renamed to the name the client polls for, so the client never observes a partially written message. Each file SHALL have a unique name.

In Wine mode the DLL SHALL NOT search for a client window, and item capture SHALL be active for the lifetime of the attach.

#### Scenario: Message published under Wine

- **WHEN** the worker thread delivers a message in Wine mode
- **THEN** a uniquely named temporary file is written and then renamed to its final name, and the client only ever sees complete files

#### Scenario: Publishing fails

- **WHEN** the temporary file cannot be created or cannot be renamed
- **THEN** the failure is logged and the temporary file is removed if it exists

### Requirement: Keep message-type numbering in lockstep with the client

Message types are a numeric contract shared with the Item Assistant client. The numbering SHALL be treated as append-only: an existing value MUST NOT be reused for a different meaning, and any addition or change SHALL be mirrored in the client's matching enumeration in the same change.

The DLL SHALL be able to report, as distinct message types: worker thread launched, hardcore flag observed, hardcore flag observed at game-info construction, generic hook failure with the failing hook's identifier, generic hook success with the same identifier, injection cancelled, and the seed-info error cases for "no game" and "no item".

#### Scenario: A new message type is introduced

- **WHEN** a new message type is added to the DLL
- **THEN** the client's enumeration gains the same name and the same numeric value, and no existing value changes meaning

#### Scenario: Hook installation is reported

- **WHEN** a hook is installed or fails to install
- **THEN** a success or failure message carrying that hook's identifier is queued for the client

#### Scenario: A hook identifier is reported to the client

- **WHEN** the client receives a hook success or failure message and resolves its payload identifier for display
- **THEN** the identifier is drawn from the same shared numbering as the message types, so that identifiers used only as hook identifiers and never as message types are still defined on both sides
