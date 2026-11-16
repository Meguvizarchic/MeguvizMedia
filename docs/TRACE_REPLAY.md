# Synthetic timeline and deadline replay

`mgm_trace_replay --events INPUT --out OUTPUT` replays a **synthetic host
scenario** through the Media planner and admission APIs. It is not a device
event reader, runtime measurement, GPU completion proof, or presentation proof.
Each input row produces a decision row. The first output explicitly identifies
`SYNTHETIC_PLANNER_REPLAY`; input durations and retirements are assertions in
the scenario, not observed physical events.

The original no-argument and `--delayed` timeline demonstrations remain
available. For a sample scenario:

```text
mgm_trace_replay --events tests/fixtures/timeline/deadline_pressure.jsonl --out decisions.jsonl
mgm_trace_replay --events tests/fixtures/timeline/seek_speed.jsonl --out seek-speed-decisions.jsonl
```

`seek_speed.jsonl` covers a 1x-to-2x clock change and a seek; both require
new epochs. The fixture and output are synthetic and do not prove host playback.

`tests/test_seek_speed_parity.py` compares that fixture with the original BIL
Kotlin timeline and checks pending proposals during speed changes and seeks.
Pass `--exe <mgm_trace_replay>` and `--out-dir <new-directory>`; optionally use
`--legacy-repo <BIL-repository>`, `--gradle-cache <modules-2/files-2.1>`,
and `--java <java-executable>` to override the local Windows defaults. The
script verifies the frozen M0 source hashes and commit content before running;
the requested Kotlin 2.4.21 compiler/runtime jars must already be available.
Missing or invalid input paths fail before the output directory is created.
After preflight, the command ledger records argv before each launch, including
both `git show` lookups; a process-start failure retains `commands.json`, the
failure phase and stderr in the new output directory. No executable is run as
a preflight probe.

Output must not exist; it is created exclusively after the entire input has
been validated and replayed. A write error can leave a partial new file, which
is not a successful result. The program never replaces an existing output.
Inputs are regular non-symlink files up to 8 MiB, with at most 32,768 lines and
8,192 bytes per line. Output is limited to 16 MiB. Each line is one flat JSON
object with unique keys, ASCII lowercase/digit/underscore identifiers and
signed 64-bit integer values. Strings are limited to identifiers; escaped
strings, floats, booleans, null, arrays, and nested objects are rejected. Every
row must include contiguous `seq` starting at 1 and an `op`; unknown or missing
fields are rejected. The first and only first row is `config`.

`config` requires `target_fps`, `frame_window`, `max_clock_age_ns`,
`presentation_delay_ns`, `max_lead_ns`, `min_lead_ns`,
`original_tolerance_us`, `max_clock_uncertainty_us`, `phase_lock`,
`cold_cost_ns`, `safety_margin_ns`, `sample_window`, `percentile`,
`max_in_flight`, and `enabled`. The fixture shows a complete example. Other
operations have these additional fields:

| Operation | Required fields | Meaning |
| --- | --- | --- |
| `begin` | `media`, `decoder`, `surface`, `format`, `discontinuity` | Begin an epoch and report the actual planner generation and discarded frame IDs. |
| `clock` | `media_us`, `monotonic_ns`, `speed_milli`, `discontinuity`, `uncertainty_us`, `paused` | Apply a clock snapshot; speed is the integer field divided by 1000. |
| `offer` | `frame_id`, `pts_us` | Offer a source frame and report accepted/discarded IDs. |
| `plan` | `now_ns` | Report the planner's proposal or waiting reason. |
| `commit` | `now_ns` | Commit the most recent proposal and report the actual API status. |
| `reserve` | `job_id`, `now_ns`, `deadline_ns` | In an active epoch, call admission and report status, predicted cost, in-flight count and sample count. |
| `cancel` | `job_id` | Mark a simulated job as publication-suppressed; its admission slot remains held. |
| `retire` | `job_id`, `joint_ns`, `succeeded` | Assert simulated physical retirement and return the slot; cancelled jobs must use `succeeded=0`. |
| `counters` | none | Read actual planner counters. |
| `close` | none | Close the current epoch and report discarded frames and outstanding jobs. |

`reserve` requires an epoch begun by `begin` and not subsequently closed or
invalidated by a clock snapshot requiring restart. This command does not offer
a standalone admission-only mode. Existing jobs may still be cancelled and
retired after their epoch closes or requires restart.

An accepted job ID cannot be reused in the same scenario. Every accepted job
must be retired and the epoch closed before EOF; otherwise the command fails
without creating a result. A `close` can precede the last simulated retirement,
so pending work is never silently treated as retired. A planner or admission
error outside documented decision statuses fails the replay. Status values are
the public `MgcStatus` values; caller-supplied time and outcome values are not
independently verified observations.
