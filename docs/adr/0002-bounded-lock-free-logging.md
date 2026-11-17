# ADR-0002: Bounded lock-free logging with an engine-owned log thread

- Status: Accepted (M1)
- Date: 2026-11-17

## Context

Logging is needed from every thread, including future real-time audio and
video threads that must never block, lock or perform I/O. Logs must also be
structured (machine-readable) and unify native and managed records.

## Decision

- Producers format into fixed-size slots of a bounded lock-free MPMC queue
  (no locks, no allocation, no syscalls after first use on a thread).
- A full queue **drops the newest record and counts it**; logging never
  applies backpressure.
- One engine-owned thread drains the queue and delivers to a JSON-lines file
  and an optional host callback. It polls every 10 ms rather than being
  signalled, because signalling would put a syscall on the producer path.
- The managed host forwards its `ILogger` output into the engine log, so one
  ordered file covers the application; native records go to the UI through a
  bounded `Channel` (drop-oldest) and never back into `ILogger`.

## Consequences

- + Safe for real-time threads; verified non-blocking under a stalled sink.
- + Single structured log for support.
- − Up to ~10 ms delivery latency; messages > 479 bytes are truncated (marked).
- − Under extreme load records can be dropped (counted in diagnostics).
- The same `BoundedMpmcQueue` is the building block for M2 media queues.
