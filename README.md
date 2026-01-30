> **BPU (Batch Processing Unit)** is a small embedded scheduling core designed to
> keep outgoing data stable under pressure.
>
> It focuses on **runtime behavior**, not API completeness:
> backpressure handling, budget-based degradation, and observable recovery.


# BPU v2.9b-r1 — Dual UART Demo (ESP32)
## Documentation
- [Design notes](docs/design_notes.md) — core scheduler, backpressure, degradation strategy
- [Runtime log samples](docs/log_samples.md) — real ESP32 logs with interpretation
- [Runtime statistics](docs/stats.md) — observability counters and pressure signals
- [Data stability flow diagram](docs/diagram.md) — high-level data flow under pressure



BPU is a small embedded scheduling core that keeps outgoing data stable under:
- **Budget pressure** (bytes-per-tick limit)
- **TX backpressure** (UART TX buffer not ready)

This repository includes a **dual-UART demo**:
- `Serial` (USB, 115200): human-readable logs
- `Serial1` (UART, 921600): binary frames (COBS + CRC16)

- Runtime behavior is observable via explicit counters (`stats.md`) and validated with real logs.


## What this demo proves
- When budget is insufficient → jobs are requeued, low-priority jobs may drop (TELEM), stats show `skipB`, `degrade_*`.
- When TX is blocked → jobs requeue, stats show `skipTX`, queue growth and recovery.
- Coalescing keeps only the newest of certain event types.

## Hardware
ESP32-WROOM
- Serial1 TX: GPIO17
- Serial1 RX: GPIO16 (unused)

## When to use BPU

BPU is useful when:

- You must **never block producers**, even when output is slow
- Output bandwidth is limited or bursty (UART, BLE, radio)
- Some data is **more important than others**
- You want **observable, explainable drops**, not silent failure

Typical targets:
- ESP32 / MCU telemetry pipelines
- Dual-UART or UART + BLE systems
- Sensor aggregation under bandwidth limits

## What BPU is NOT

- Not a general-purpose RTOS scheduler
- Not a message queue library
- Not a finalized public API

This repository represents a **validated engine snapshot**
focused on behavior under pressure.

## How to validate this demo

1. Build and upload the firmware to ESP32-WROOM
2. Open USB Serial Monitor at 115200 baud
3. Observe runtime logs during:
   - normal operation
   - UART TX blocking
   - budget pressure (high event rate)
4. Compare observed logs with:
   - `docs/log_samples.md`
   - `docs/stats.md`
   - `docs/diagram.md`


## Build & Run
1. Open the `.ino` in Arduino IDE
2. Select **ESP32 Dev Module** (or your target WROOM board)
3. Upload and open Serial Monitor at **115200**

## Frame format (OUT)
`0x00` delimiter + `COBS( [0xB2, type, seq, len, payload..., crc16] )`

## License
TBD (will be set to MIT)
