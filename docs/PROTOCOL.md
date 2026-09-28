# BLE protocol reference

This document describes everything a client needs to talk to the logger. The reference client is [`web/index.html`](../web/index.html); any BLE central (a native app, a Python script using `bleak`, ...) can implement the same protocol.

All multi-byte values are **little-endian**. All structures are **packed** (no padding).

## Advertising

| Property | Value |
|---|---|
| Device name | `IMU-LOGGER` |
| Advertised service | `7c6e0001-6f3d-4f9d-9b5e-8f4f2a1b1000` |
| Connections | 1 at a time (peripheral role only) |

## Service and characteristics

All characteristics live in the service `7c6e0001-6f3d-4f9d-9b5e-8f4f2a1b1000`.

| Name | UUID | Properties | Payload |
|---|---|---|---|
| `live` | `7c6e0002-…` | Notify | 28 bytes, `LivePacket` |
| `command` | `7c6e0003-…` | Write / Write without response | 1 byte, ASCII command |
| `status` | `7c6e0004-…` | Notify | ASCII text, up to 20 bytes |
| `offset` | `7c6e0005-…` | Notify | 24 bytes, `OffsetPacket` |
| `data` | `7c6e0006-…` | Notify | up to 180 bytes, raw download stream |
| `meta` | `7c6e0007-…` | Notify | 12 bytes, `MetaPacket` |

(The full UUID of each characteristic is `7c6e000N-6f3d-4f9d-9b5e-8f4f2a1b1000`.)

**Subscribe to all notifying characteristics right after connecting.** When a client enables notifications on `status`, `meta` or `offset`, the firmware immediately pushes the current value, so a client that connects in the middle of a recording is synchronized without asking.

## Commands

Written to the `command` characteristic as a single ASCII byte.

| Byte | Meaning | Effect |
|---|---|---|
| `C` | Calibrate | Averages the gyroscope for 5 s (board must be still). Ignored while recording. |
| `S` | Start | Starts a new recording, **discarding the previous one**. |
| `E` | Stop | Stops the current recording. Ignored if not recording. |
| `D` | Download | Streams the stored recording on `data`. Answers `BUSY` while recording/calibrating and `NO_DATA` if empty. |
| `M` | Refresh metadata | Sends a `MetaPacket` on `meta`. |

## Status messages

ASCII strings notified on `status`.

| Message | Meaning |
|---|---|
| `READY` | Idle, IMU initialized. |
| `IMU_ERROR` | The IMU did not initialize (check wiring / board variant). |
| `CALIBRATING` | Calibration started (lasts 5 s). |
| `CALIBRATED` | Calibration finished; an `OffsetPacket` was sent just before. |
| `RECORDING` | Recording started. |
| `SAVED` | Recording stopped; a `MetaPacket` was sent just before. |
| `FULL` | Buffer full (60 s). The recording was stopped and kept; `SAVED` precedes this message. |
| `DOWNLOAD` | A download is starting. |
| `DOWNLOAD_DONE` | The whole stream was sent. |
| `NO_DATA` | Download requested but no recording is stored. |
| `BUSY` | Download requested while recording or calibrating. |
| `TRANSFER_ERROR` | The download was aborted (notification queue stuck). |

## Data structures

### `LivePacket` (28 bytes) — characteristic `live`

Sent at 10 Hz (every 10th sample) while recording.

| Offset | Type | Field | Unit / scale |
|---|---|---|---|
| 0 | uint32 | `seq` | sample index since recording start |
| 4 | uint32 | `t_us` | board `micros()` timestamp |
| 8 | int16 | `ax` | g × 1000 |
| 10 | int16 | `ay` | g × 1000 |
| 12 | int16 | `az` | g × 1000 |
| 14 | int16 | `gxRaw` | °/s × 10 |
| 16 | int16 | `gyRaw` | °/s × 10 |
| 18 | int16 | `gzRaw` | °/s × 10 |
| 20 | int16 | `gxCorr` | °/s × 10, offset removed |
| 22 | int16 | `gyCorr` | °/s × 10, offset removed |
| 24 | int16 | `gzCorr` | °/s × 10, offset removed |
| 26 | int16 | `temp` | °C × 100 |

### `OffsetPacket` (24 bytes) — characteristic `offset`

| Offset | Type | Field |
|---|---|---|
| 0 / 4 / 8 | float32 | mean gyro reading at rest `x`, `y`, `z` (°/s) |
| 12 / 16 / 20 | float32 | standard deviation `sx`, `sy`, `sz` (°/s) |

### `MetaPacket` (12 bytes) — characteristic `meta`

| Offset | Type | Field |
|---|---|---|
| 0 | uint32 | `samples`: number of stored samples |
| 4 | uint32 | `bytes`: exact size of the download stream (`20 + 18 × samples`, or 0) |
| 8 | uint8 | `valid`: 1 if a recording is available |
| 9 | 3 bytes | padding |

## Download stream

After the command `D`, the firmware notifies `status = DOWNLOAD`, then streams on `data`:

```
[ Header (20 bytes) ][ Record 0 (18 bytes) ][ Record 1 ] ... [ Record N-1 ]
```

then notifies `status = DOWNLOAD_DONE`. The stream is cut into notifications of at most `min(MTU - 3, 180)` bytes; **chunk boundaries do not align with records**, so a client must concatenate all chunks before parsing. BLE preserves order, so no reordering logic is needed.

### `Header` (20 bytes)

| Offset | Type | Field |
|---|---|---|
| 0 | uint32 | magic `0x31554D49` (ASCII `IMU1`, i.e. format version 1) |
| 4 | uint32 | `count`: number of records |
| 8 / 12 / 16 | float32 | gyro offsets `x`, `y`, `z` (°/s) from the last calibration |

### `Record` (18 bytes)

| Offset | Type | Field | Convert to physical value |
|---|---|---|---|
| 0 | uint32 | `t_us` | microseconds (board `micros()`) |
| 4 | int16 | `ax` | `/ 8000` → g |
| 6 | int16 | `ay` | `/ 8000` → g |
| 8 | int16 | `az` | `/ 8000` → g |
| 10 | int16 | `gx` | `/ 64` → °/s (raw) |
| 12 | int16 | `gy` | `/ 64` → °/s (raw) |
| 14 | int16 | `gz` | `/ 64` → °/s (raw) |
| 16 | int16 | `temp` | `/ 100` → °C |

The corrected gyroscope value is `raw − offset` (offset from the header). The sequence number is implicit: it is the record index.

### Integrity check

A client should verify that:

1. the magic number matches,
2. the received byte count equals `MetaPacket.bytes` (which the firmware re-sends right before `DOWNLOAD`),
3. the parsed record count equals `Header.count`.

The reference dashboard shows a warning when any of these fail.

## Timing notes

- Samples are taken by a polling loop on the board's `micros()` clock. On the nRF52840 Arduino core this clock has a resolution of about 0.977 ms (1/1024 s), so individual timestamps are quantized. The **mean rate is exact** (100.00 Hz measured); for signal processing prefer `index / 100` as the time axis.
- The IMU output data rate is set to 104 Hz (nearest supported value) while the loop reads at 100 Hz, so roughly one sensor sample in 25 is never read.
- A new connection starts at ATT MTU 23 (20 usable bytes). The firmware requests MTU 247 and a longer data length on every connection; the live packet (28 bytes) and download chunks do not fit without it.
