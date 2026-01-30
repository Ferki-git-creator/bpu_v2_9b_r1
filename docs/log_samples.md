
# BPU Runtime Log Samples

This document shows real runtime logs captured during  
dual-UART demo execution on **ESP32-WROOM**.

These logs validate the behavior described in:
- `diagram.md`
- `stats.md`

---

## Scenario 1: Normal operation

**Conditions**
- TX available
- Budget sufficient

### Log

[TICK 1024]
queue_depth=3
tx_flush=1
skipTX=0
drop_low_pri=0


### Interpretation
- Jobs are flushed immediately
- No drops observed
- System operates within budget and TX capacity

---

## Scenario 2: TX backpressure

**Conditions**
- UART TX buffer blocked

### Log

[TICK 3072]
queue_depth=12
tx_flush=0
skipTX=3
drop_low_pri=0

[TICK 3080]
queue_depth=5
tx_flush=2
skipTX=3
drop_low_pri=0


### Interpretation
- Jobs are queued while TX is blocked
- `skipTX` counter increases due to backpressure
- Recovery is observed when TX resumes
- Queue depth decreases after TX becomes available

---

## Scenario 3: Budget pressure

**Conditions**
- bytes-per-tick limit exceeded

### Log

[TICK 2048]
queue_depth=7
tx_flush=1
skipTX=0
drop_low_pri=4


### Interpretation
- Low-priority jobs (TELEM) are dropped under budget pressure
- High-priority jobs are preserved
- Degradation strategy works as designed

---

## Notes

- All counters shown above are defined in `stats.md`
- Each scenario corresponds to a control path in `diagram.md`
- Logs are emitted on the USB Serial port (115200 baud)
