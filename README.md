# BPU v2.9b-r1

**BPU (Binary Perception Unit)** is a lightweight, perception-oriented data pipeline
designed for low-power embedded devices such as ESP32-class MCUs.

This repository contains **v2.9b-r1**, a stable Arduino demo implementation.

---

## What problem does this solve?

Typical embedded pipelines focus on raw data throughput.
BPU instead focuses on **what the receiver can meaningfully perceive** under constraints:

- Limited bandwidth
- Small buffers
- Partial updates
- Human-perceptual latency tolerance

BPU prioritizes *perceptual freshness* over strict completeness.

---

## Key ideas in this version

- Event → Job pipeline separation
- Coalescing of redundant events
- Per-tick TX budget enforcement
- Graceful degradation under congestion
- Deterministic, inspectable behavior (debug counters)

This is **not a library**, but a **reference architecture**.

---

## File

- `bpu_v2_9b_r1.ino`  
  Final Arduino demo for ESP32 (Dual UART, perceptual scheduling)

---

## Status

This version is considered **stable** and used as a reference
for rebuilding the PTE (Perceptual Transport Engine) golden code.
