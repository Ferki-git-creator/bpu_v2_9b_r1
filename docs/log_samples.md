# BPU v2.9b-r1 — Runtime Log Samples

This document shows real runtime logs captured during  
dual-UART demo execution on **ESP32-WROOM**.

These logs validate the runtime behavior described in:
- `docs/diagram.md`
- `docs/stats.md`

---

## Scenario 1: Normal operation

**Conditions**
- TX available
- Budget sufficient

### Log

[BPU2.9b-r1][TICK 1024]
queue_depth=3  
tx_flush=1  
skipTX=0  
drop_low_pri=0  

### Interpretation
- Jobs are flushed immediately
- No drops are observed
- System operates within budget and TX capacity

---

## Scenario 2: TX backpressure

**Conditions**
- UART TX buffer blocked

### Log

[BPU2.9b-r1][TICK 3072]
queue_depth=12  
tx_flush=0  
skipTX=3  
drop_low_pri=0  

[BPU2.9b-r1][TICK 3080]
queue_depth=5  
tx_flush=2  
skipTX=3  
drop_low_pri=0  

### Interpretation
- Jobs accumulate while TX is unavailable
- `skipTX` counter increases due to backpressure
- Recovery is observed once TX resumes
- Queue depth decreases after TX becomes available

---

## Scenario 3: Budget pressure

**Conditions**
- Bytes-per-tick limit exceeded

### Log

[BPU2.9b-r1][TICK 2048]
queue_depth=7  
tx_flush=1  
skipTX=0  
drop_low_pri=4  

### Interpretation
- Low-priority jobs (TELEM) are dropped under budget pressure
- High-priority jobs continue to flush
- Degradation strategy behaves as designed

---

## Notes

- All counters shown above are defined in `docs/stats.md`
- Each scenario maps directly to a control path in `docs/diagram.md`
- Logs are emitted on the USB Serial port (115200 baud)
- Actual runtime logs are more verbose; samples above are reduced for clarity
