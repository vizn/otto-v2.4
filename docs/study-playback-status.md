# Study Playback Status

> Scope: `ivy-robot-v2` learning cards and course playback.
> Validation record: 2026-10-14; firmware version remains `2.5.1`.

## Overview

Learning playback now reports its actual asynchronous lifecycle instead of relying on a successful MCP tool return. The entry point is `self.ivy.play_course`, with `key`, optional `word`, and `course` arguments. An empty key is resolved by the device. A successful tool response is not proof that audio started or completed.

The implementation supports single cards, FaceFonics two-round playback, automatic course continuation, explicit cancellation, failure reporting, and retained status queries. It uses the existing `Application::SendMcpMessage()` path. MIOT remains disabled; no additional connection or forced audio-channel opening is required.

## Notification Contract

The device sends a JSON-RPC notification without a request ID:

```json
{
  "jsonrpc": "2.0",
  "method": "notifications/ivy/study_status",
  "params": {
    "boot_id": "4e739ac120d689ab",
    "playback_id": "4e739ac120d689ab-7",
    "seq": 19,
    "state": "playing",
    "key": "kc-001",
    "round": 2,
    "total_rounds": 2,
    "course": false,
    "error": null
  }
}
```

| Field | Meaning |
|---|---|
| `boot_id` | Nonempty, 16-character hexadecimal ID generated for the current boot. It is an ordering scope, not an authentication credential. |
| `playback_id` | Nonempty `boot_id-request_number` string. The request number increments for each new learning request. All rounds and lessons in one course run retain this ID. |
| `seq` | Positive integer increasing globally within the same boot. It does not reset for a new request, round, or lesson. |
| `state` | `loading`, `playing`, `paused`, `completed`, `stopped`, or `failed`. |
| `key` | Current card key. It may be empty while resolving the initial card or when that lookup fails. |
| `round` | Positive, one-based round number for the current lesson. |
| `total_rounds` | `2` for FaceFonics `kc-001` through `kc-109`; otherwise `1`. |
| `course` | Boolean indicating whether the request is a course-continuation run. |
| `error` | `null`, or an object containing `code` and `message`. Messages are fixed, safe descriptions, not raw URLs, credentials, or response bodies. |

Typical failure codes include `card_failed`, `preparation_failed`, `audio_queue_failed`, `audio_failed`, and `next_course_failed`. Audio failure and next-lesson lookup failure are distinct outcomes. Missing optional images, GIFs, or subtitles do not by themselves fail audio playback.

## Lifecycle Semantics

| State | Trigger |
|---|---|
| `loading` | A learning request is accepted, a new round is prepared, or the next course lesson is being resolved/prepared. |
| `playing` | The first nonempty PCM output has been submitted to the codec. Resuming from a reported pause also reports `playing` when PCM output resumes. |
| `paused` | The player observes a pause after PCM playback has started. This includes the existing remote-TTS pause path. |
| `completed` | All required rounds of a single-card request finish naturally, or course continuation receives an explicit server `done` result after natural playback completion. |
| `stopped` | The learning run is explicitly cancelled, replaced, or interrupted by another playback request. |
| `failed` | A required lookup, preparation task, audio queue, or audio playback operation fails. |

The first FaceFonics round must not report `completed`: it transitions to `loading` for the cartoon round, then back to `playing`. Course lesson boundaries similarly use `loading` and `playing` with the same `playback_id`; individual lesson endings are not whole-run completion events.

Only natural audio completion can advance to another round or lesson. Stop and failure must not trigger continuation. A stale event must not change a newer request's state.

`playing` means PCM reached the output path, not that a person heard it. Likewise, state completion does not certify speaker quality, visual synchronization, or subtitle accuracy.

## Queries and Recovery

Both `self.get_device_status` and `self.ivy.media_status` expose:

| Query field | Meaning |
|---|---|
| `study_boot_id` | Always identifies the current boot, including before any learning request. |
| `study_playback` | The latest retained notification snapshot, or `null` before the first learning request in this boot. Terminal snapshots remain available. |

A missing `study_boot_id` can indicate older firmware. A present boot ID with a null snapshot means this boot has not started learning. Do not carry a previous boot's active run into that case.

When the existing MCP session receives `notifications/initialized`, a board-registered initialization callback resends the retained non-null snapshot. This is a replay with the same `seq`, not a new lifecycle event. Receivers should handle duplicate snapshots idempotently and compare sequence numbers only within the same boot.

Notifications remain best-effort: there is no ACK or historical event queue. Keep the existing connection strategy. After a service restart or a missed notification, use the next MCP initialization replay or query the status tools; do not open a voice channel just to force a report.

## Concurrency and Failure Guards

- One preparation worker handles initial-card lookup, next-lesson lookup, cover download, and GIF preparation. Its single-slot queue retains only the newest pending request.
- One subtitle worker also uses a single-slot queue. Replacing a request does not spawn an unbounded number of tasks.
- Main-task code dispatches learning preparation and applies copied results; it does not perform those HTTP lookups or cover downloads synchronously.
- Generation checks protect queued work, copied results, audio commands, and final image/GIF commits. Stop or replacement invalidates old work before it can restart playback or overwrite a newer preview.
- A locked retained snapshot supports cross-task queries. Lifecycle decisions remain on the main task.
- Generic playback-stop callbacks carry an owner and are deduplicated. A redundant queued stop does not notify twice, and an old learning stop callback cannot cancel a newer music/dance owner.
- HTTP truncation, incomplete MP3 frames, and unrecognized nonzero EOF tails are failures, not successful completion. Supported metadata tails are checked without destructively scanning through possible APE binary items.
- The learning encoded/candidate-tail buffer is limited to 64 KiB. Oversized or unresolvable candidates fail rather than growing without bound.

`MusicPlayer` emits numeric diagnostics for buffer-limit failures, the first tail candidate, decoder failures, rejected EOF, and HTTP read failures. These include byte offsets, received/expected lengths, buffer sizes, decoder error codes, and consumed lengths. They do not change the notification schema or relax completion rules.

Primary implementation locations are `main/boards/ivy-robot-v2/miot_client.*`, `music_player.*`, `playback_logic.h`, and `otto_robot.cc`. The generic MCP initialization hook is in `main/mcp_server.*` and has no dependency on a concrete board.

## Recorded Validation

The following hardware results were reported from LAN MCP tests on 2026-10-14:

| Scenario | Reported result |
|---|---|
| `kc-001`, rounds 1 and 2 | Final `completed` after about 89.6 seconds; no completion event at the first-round boundary. |
| Stop during `loading` | `stopped`; no delayed second playback from old preparation work. |
| Missing `kc-999` | `failed` after about 4.4 seconds, with the same boot ID; subsequent error recovery succeeded. |
| `kc-208`, `course=true` | Final `completed` after about 251.65 seconds. |
| Ownership and WebSocket correlation | Expected owner/request and WebSocket response correlation matched. |

These are protocol/state observations, not a manual listening or visual acceptance test. Subtitle content and timing were not manually audited.

The long-audio `total=30` timeout fix was made in the upstream server/proxy, not in this firmware. Do not attribute that server-side fix to a relaxed firmware EOF check. Earlier representative `kc-208` downloads from the original service and the proxy were byte-identical; strict host decoding and shared-parser replay accepted the complete media.

A separate historical boot change during a missing-card investigation was not proven to be a firmware panic. The serial summary showed `USB_UART_CHIP_RESET` without a panic; opening or closing a monitor can affect reset behavior. That observation alone is not a code-panic diagnosis.

### Artifact Identity

Version `2.5.1` alone does not distinguish these working-tree builds. The recorded diagnostic artifacts have the following SHA-256 hashes:

| Artifact | SHA-256 |
|---|---|
| `build/ivy-robot.bin` | `aa236d2d1bfe9191dc24e9b408b6f6f9f563cbbbc1e7b6e6b37634e48b06f573` |
| `build/ivy-robot.elf` | `e330a0110b1f90e00f8591ac2023ceaf1375da1b01e4eb942e1d30339fd74292` |

These identify the recorded artifacts; rebuilding or changing dependencies may produce different hashes. No deployment is implied by this document.

## Build and Host Tests

Run from the firmware repository, using the intended ESP-IDF installation:

```sh
source /home/asin/esp/esp-idf/export.sh
idf.py --version
idf.py build
python3 -m unittest discover -s scripts/tests -v
git diff --check
```

The validated local SDK is ESP-IDF v6.0.2. Host playback tests require a C++17 compiler available as `c++`. They compile and execute shared C++ logic rather than asserting source text.

There are five standard playback-logic tests: EOF/metadata validation, cancellation of work/results, stop deduplication and owner isolation, remaining HTTP read budget, and headerless APE through the frame scanner. One additional artifact test is optional and skipped unless `IVY_STUDY_AUDIO_ARTIFACT` is set.

To exercise a real downloaded MP3, the artifact must actually exist locally:

```sh
test -f /path/to/downloaded-kc208.mp3 && \
  IVY_STUDY_AUDIO_ARTIFACT=/path/to/downloaded-kc208.mp3 \
  python3 -m unittest discover -s scripts/tests -v
```

The artifact is read through C++ `istream` and replayed across multiple input chunk sizes. Large media files are not checked into Git. Shared-parser tests do not replace the ESP decoder or hardware tests.

The existing release-matrix count test has a known unrelated failure: actual IDF 5 variant count `172` versus the fixed expected count `171`. It was left unchanged. With the real artifact enabled, the recorded full suite had 13 passes and this one failure.

## Backup and Update Notes

This is an operational record for the tested ESP32-S3 native-USB unit, not a universal backup/restore recipe.

1. The default stub in esptool 5.3.1 was unreliable for reads on this setup. The successful backup used the legacy `--stub-version 1`, baud rate `921600`, and `--before no-reset` / `--after no-reset`. Do not assume identical behavior on another chip, USB path, or esptool version.
2. The 16 MiB flash was read as 64 chunks of 256 KiB (262144 bytes). Record each chunk's actual flash offset and verify every expected chunk, length, and index before merging.
3. Merge in numeric index or flash-offset order. A shell wildcard uses lexical ordering, where `part-10` can precede `part-2`; do not concatenate an unverified wildcard expansion. Verify that the merged length is exactly `16777216` bytes, then calculate and retain its SHA-256.
4. A full-flash backup contains NVS and may contain credentials or other device-specific data. Use a restricted directory and private file permissions, such as directory mode `0700` and backup file mode `0600`. Do not publish the backup with ordinary build artifacts.
5. Before any application-only update, verify the exact device, partition table, application capacity, asset compatibility, and active OTA selection. An application write does not by itself select the OTA slot or update assets.
6. In the recorded update, only the `ota_0` application at `0x20000` was targeted; NVS and the other existing flash regions were preserved. That offset is a fact about this device's layout, not an instruction for arbitrary devices.

Do not blindly restore a full backup onto another unit or infer a restore command from the application offset. Encryption, secure boot, partition layout, OTA metadata, and device-specific NVS must be checked first. This document deliberately provides no generic device-write or restore command.

## Known Limitations

- Learning mode still suppresses incoming remote-TTS audio. The existing pause/resume path can pause learning while that TTS audio is discarded; this behavior has not been expanded or fixed here.
- Subtitle timing does not freeze with a media pause and has not received manual timing validation.
- Cancellation is cooperative, not a preemptive abort of an in-flight network read. State and generation invalidation do not block the main task, but a worker can wait for the remaining read budget or underlying connection/header timeout before servicing the newest request.
- Preparation and subtitle workers have firmware lifetime. Dynamic deletion/recreation of their owning client is not supported.
- Initialization replay and GET recovery are not guaranteed-delivery event transport. They intentionally require no change to connection policy.
