# Benchmark: UnoCode Desktop against Visual Studio Code

Measured 20 August 2026 on `amanuensis` (Intel Xeon E5-2695 v4, 8 cores, 32 GB,
Windows 11 Pro, a VM on `leviathan`) against VS Code 1.126.0, with a 40-file
workspace. Median of 4 runs after a discarded warm-up; 3 runs for the
large-file rows. Reproduce with `tools/bench.ps1`.

## Results

| | UnoCode | VS Code (clean) | VS Code (real) | Ratio |
|---|---:|---:|---:|---:|
| Time to a usable window | 276 ms | 1126 ms | 3597 ms | 4.1x / 13.0x |
| Time to first content | 199 ms | 1034 ms | 3278 ms | 5.2x |
| Window on screen | 199 ms | 1034 ms | 1059 ms | |
| Memory, private bytes | 39.6 MB | 895.6 MB | 938.1 MB | 22.6x |
| Memory, working set | 48.6 MB | 1202.2 MB | 1230.7 MB | 24.7x |
| Processes | 1 | 8 | 8 | 8.0x |
| CPU at rest | 1.17 % | 0 % | 97.66 % | see below |
| Install size | 6.5 MB | 886 MB | 886 MB | 136x |
| Files on disk | 14 | 6,402 | 6,402 | 457x |
| 4 MB / 60k-line file, to usable | 267 ms | 1168 ms | | 4.4x |
| Memory with that file open | 47.7 MB | 1059.2 MB | | 22.2x |

"Clean" is VS Code with an empty user-data directory, no extensions and
workspace trust disabled: the fastest it can start. "Real" is this machine's
actual VS Code, which is what opens when you click the icon. Both are reported
because only one of them is what anyone experiences.

## Method

Neither application was instrumented, so neither defined its own finish line.
Both were launched the same way against the same folder and watched from
outside:

1. Start the process, start a stopwatch.
2. Repeatedly find the window belonging to the process tree, photograph the
   screen where it sits, and count distinct colours over a fixed 60x40 sample
   grid. An unpainted window scores a handful; a painted workbench scores dozens.
3. **First content** is the first sample at or above 14 distinct colours.
   **Usable** is the point after which that count stops changing for 500 ms.
   The second is the number to quote: it is threshold-insensitive and it is the
   moment a person would say the editor is up.
4. Wait 15 s so the application stops working, then sample memory across the
   whole process tree and CPU over the next 8 s.

### Two things the traces show that a single number does not

- **VS Code's window appears long before its content.** On the real profile the
  window is on screen at 1059 ms and still essentially blank (3 to 4 distinct
  colours) for more than two seconds, reaching content only at 3278 ms.
  UnoCode's window and its content arrive together, because it paints before it
  shows.
- **UnoCode is at the harness's floor.** Sampling costs about 63 ms per
  frame, and UnoCode was already fully painted at the first sample the harness
  could take. Its startup figures are therefore an **upper bound**, and every
  startup ratio here is conservative.

## Where UnoCode loses

Idle with a file open, clean VS Code uses **0%** CPU and UnoCode uses
**1.17%**. UnoCode's event loop wakes every 15 ms whether or not anything
happened, and it repaints on the caret's cadence. That is a fixable
inefficiency in the host shim rather than an architectural cost, but it is a
loss and it is recorded as one. On the real profile the same figure is
**97.66%**, which is extensions and file watchers, not the editor.

## What this does not measure

- **Capability.** VS Code ships language servers, debugging, source control,
  remote development and a marketplace; UnoCode Desktop ships none of them yet
  (see [ROADMAP.md](ROADMAP.md)). This measures the cost of the shell you sit
  in, not the work it can do.
- **Keystroke latency.** Key-to-pixel needs a high-speed camera or a hardware
  probe to measure honestly. It is the dimension users feel most, and it is
  absent rather than estimated.
- **Window size.** UnoCode opened at 1280x800, VS Code at its own default.
  Paint cost scales with area; small against these margins, but real.
- **The host is a VM**, so timings carry a few percent of noise. The margins
  here are multiples, so the noise does not reach the conclusion.
