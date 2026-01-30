# BPU Documentation

This folder contains design notes, examples, and runtime observations
for the BPU (Batch Processing Unit) engine.

- log_samples.md : example runtime logs and interpretation
- design_notes.md : scheduler and coalescing design notes

## Validation & Runtime Evidence

This engine has been validated with real runtime logs:

[Runtime log samples and interpretation](log_samples.md)


These logs demonstrate queueing, backpressure handling,
budget-based dropping, and recovery behavior.

This project is a validated engine snapshot, not a finalized API.
