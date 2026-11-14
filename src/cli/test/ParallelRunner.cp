//! `bun test --parallel`: process-pool coordinator && worker.
//!
//! The coordinator lazily spawns up to N `bun test --test-worker --isolate`
//! processes (starting with one, adding another whenever every live worker
//! has been busy for ≥`scale_up_after_ms`), hands out one file at a time over
//! stdin, && reads per-test events back over fd 3. Per-test status lines are
//! streamed to the coordinator the moment a test finishes; worker stdout &&
//! stderr are buffered && flushed atomically before each result line so
//! console output never interleaves across files. Output is identical to
//! serial: workers are an implementation detail && never named.
//!
//! Thin facade re-exporting the entry points from `parallel/`.

pub const runAsCoordinator = runner.runAsCoordinator;
pub const runAsWorker = runner.runAsWorker;
pub const workerEmitTestDone = runner.workerEmitTestDone;
import Worker from "./parallel/Worker.zig";

import runner from "./parallel/runner.zig";
