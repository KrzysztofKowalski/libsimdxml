# libsimdxml

![libsimdxml](logo.webp)

SIMD-accelerated XML parser with full XPath 1.0 for **C++23** — single-pass
indexing, zero-copy by design, runtime SIMD dispatch.

## Why

- **One binary, every CPU.** AVX2, SSE4.2, NEON and scalar backends are all
  compiled into the library and picked at run time — no `-mavx2` build flags,
  no fat binaries, no recompiles per deployment target. Structural character
  classification runs 32 bytes per instruction (PSHUFB nibble classification,
  the simdjson technique).
- **Zero copy.** The index is a structure-of-arrays of `(offset, length)`
  ranges over the input buffer. Tag names, text and attribute values are
  `string_view`s into the original bytes — nothing is allocated per node.
- **Exact allocation.** Structural counts come from SIMD popcounts, not
  `size/128` guesses — result vectors are reserved once, so large documents
  don't pay reallocation storms.
- **Vectorized tree construction.** Parent/child CSR indices are built with an
  AVX2 prefix-sum; ancestor queries are a single comparison via a post-order
  array.
- **Full XPath 1.0** — compile once, evaluate many times, over the index.

## Benchmarks

### Test data

All inputs come from the same deterministic generator (fixed seed), so runs
are reproducible:

| File              | Size   | What's inside                                                                                                                                                            |
|-------------------|--------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `sample_1mb.xml`  | ~1 MB  | synthetic XML: 16,824 elements, nesting depth up to 22, attributes on most elements, XML entities, CDATA sections, comments, processing instructions                     |
| `sample_5–30mb.xml` | 5–30 MB | the same profile scaled to the full benchmark matrix (5, 10, 15, 20, 30 MB)                                                                                              |
| `sample_1mb.html` | ~1 MB  | synthetic HTML with a realistic tag mix and ~9% unclosed `<li>` elements — robustness checks only (libsimdxml does not target HTML)                                      |

### Parse speed (MB/s)

Measured on an Intel Core i7-4850HQ (Haswell, 2013 laptop) at the CPU's
deterministic base clock (2.3 GHz, no turbo). Sequential benchmark
orchestrator: fresh process per run, cold runs discarded, medians of 9 reps
(2 cold discarded) — ratios within one run are the point.

| Workload                                          | sample_1mb.xml          | sample_5mb.xml         | sample_10mb.xml        |
|---------------------------------------------------|-------------------------|------------------------|------------------------|
| libsimdxml — parse (structural index)             | **245.7 MB/s** (4.08 ms)| **261.4 MB/s** (19.1 ms)| **285.2 MB/s** (35.1 ms)|
| pugixml 1.16, in-situ (reference)                 | 206.3 MB/s (4.85 ms)    | 227.8 MB/s (21.95 ms)  | 229.2 MB/s (43.64 ms)  |

Speedup of libsimdxml over pugixml 1.16: **1.19× / 1.15× / 1.24×**
(1 MB / 5 MB / 10 MB), from the final 15.09 full-native matrix
(`rerun_crosslang_20260915_165913`: 18 parsers, 7 languages —
C++/Python/Node/Java/Rust/Ruby/Go — in one run, Java natively via javac). The
numbers reproduce across independent runs within ~3% noise. libsimdxml
nodes/attributes counts are bit-identical to pugixml and expat at every size.

Earlier in-process A/B harness (100+ alternating runs, pre-fusion code):
parse 351/374 MB/s (1.26×/1.36× vs pugixml), full index 285/300 MB/s
(1.02×/1.10×), structural classification only ~4.3–4.8 GB/s; runtime dispatch
measured in-binary: scalar 0.8 GB/s → AVX2 3.1–4.3 GB/s.

### Cross-language context

Same `sample_1mb.xml` file, same base-clock run 165913 (15.09) — all rows
measured in one native orchestrator run, Java and Go included:

| Parser             | Language | Parse time  | Throughput    |
|--------------------|----------|-------------|---------------|
| **libsimdxml**     | C++      | **4.08 ms** | **245.7 MB/s**|
| pugixml 1.16       | C++      | 4.853 ms    | 206.3 MB/s    |
| quick-xml 0.36     | Rust     | 5.127 ms    | 195.2 MB/s    |
| expat (SAX)        | C++      | 15.495 ms   | 64.6 MB/s     |
| roxmltree 0.20     | Rust     | 17.906 ms   | 55.9 MB/s     |
| lxml               | Python   | 33.534 ms   | 29.9 MB/s     |
| libxml2 (DOM)      | C++      | 35.168 ms   | 28.5 MB/s     |
| nokogiri           | Ruby     | 44.890 ms   | 22.3 MB/s     |
| lxml.html          | Python   | 50.966 ms   | 19.6 MB/s     |
| gosax              | Go       | 49.697 ms   | 20.1 MB/s     |
| cheerio            | Node.js  | 98.650 ms   | 10.2 MB/s     |
| Xerces (DOM)       | Java     | 145.499 ms  | 6.9 MB/s      |
| sax (Node)         | Node.js  | 130.058 ms  | 7.7 MB/s      |
| xml.etree          | Python   | 158.380 ms  | 6.3 MB/s      |
| jsoup              | Java     | 254.958 ms  | 3.9 MB/s      |
| dom4j              | Java     | 300.545 ms  | 3.3 MB/s      |
| html.parser        | Python   | 1548.936 ms | 0.7 MB/s      |
| rexml              | Ruby     | 1679.903 ms | 0.6 MB/s      |

Rust's quick-xml is the fastest non-C++ parser (~195 MB/s, flat across sizes;
5–7% behind pugixml @1 MB, ~15% @5–10 MB) — libsimdxml beats it by
1.26×/1.35×/1.47× (1 MB/5 MB/10 MB). Go's `encoding/xml` (gosax, event-based)
sits mid-table at ~20 MB/s flat — over 10× behind quick-xml, with
bit-identical node/attribute counts. Versions: Java natively on this host
(javac, JDK 27); Go 1.27; quick-xml 0.36.2, roxmltree 0.20.0; nokogiri 1.19.4,
rexml 3.4.4. tinyxml2 and fast-xml-parser error out on the `<?bench-pi…?>`
corpus (a pre-existing entity limit / PI-node limitation, not a bug) and are
excluded.

### Wall time, CPU and memory in one table (24.09.2026 run)

Same run as the memory tables below: C++, `sample_1mb.xml` + `sample_10mb.xml`. Wall is the
**median** of 3 counted repetitions, CPU is the **MIN** of `getrusage` user/system of the
parser process (the runner measures it itself), memory is the kernel high-water (`peak`) and
the sampled lower bound (`low`/`avg`). `MB/s` = MiB/s of the input file.

| Parser | Corpus | wall ms | MB/s | CPU user ms | CPU sys ms | user+sys ms | peak RSS MiB | low MiB | avg MiB | B/node (peak) | `ratio_mem_peak` |
|---|---|---|---|---|---|---|---|---|---|---|---|
| **libsimdxml** | 1 MB | **3.537** | **283.0** | **1.80** | 0.18 | 1.98 | **9.39** | 6.77 | 8.29 | **269.1** | **0.881** |
| pugixml 1.16 | 1 MB | 3.693 | 271.1 | 1.86 | 0.01 | 1.87 | 10.67 | 6.81 | 8.72 | 344.0 | 1.000 |
| expat (SAX) | 1 MB | 11.525 | 86.9 | 10.97 | 0.09 | 11.06 | 7.38 | 6.43 | 7.38 | 142.5 | 0.692 |
| libxml2 (DOM) | 1 MB | 26.507 | 37.8 | 18.92 | 3.95 | 22.87 | 24.81 | 6.79 | 14.47 | 1,228.7 | 2.326 |
| **libsimdxml** | 10 MB | **33.727** | **296.5** | 24.11 | 7.46 | **31.57** | **46.72** | 15.73 | 33.63 | **260.3** | **0.806** |
| pugixml 1.16 | 10 MB | 35.916 | 278.5 | 17.77 | 11.46 | 29.23 | 58.00 | 15.52 | 38.35 | 330.3 | 1.000 |
| expat (SAX) | 10 MB | 121.381 | 82.4 | 113.55 | 3.95 | 117.50 | 25.44 | 15.45 | 25.19 | 127.0 | 0.439 |
| libxml2 (DOM) | 10 MB | 263.196 | 38.0 | 208.35 | 44.65 | 253.00 | 197.37 | 15.74 | 105.89 | 1,199.9 | 3.403 |

Two things worth reading straight off that table, without spin:

* **Memory is a win that grows with size**: 0.881 → 0.806 of pugixml's peak (B/node
  269 → 260 vs pugixml's 344 → 330), while `low` stays at parity — at the bottom of the
  parse both processes hold just the input.
* **CPU is not a win at 10 MB**: libsimdxml is 6% faster in wall time but spends ~8% *more*
  total CPU than pugixml (31.6 vs 29.2 ms), and pugixml's share of system time is higher
  (11.5 vs 7.5 ms). At 1 MB the two are within noise of each other. A single-threaded parse
  of this size is memory-bandwidth-bound, so wall time and CPU time do not have to move
  together — but it is the CPU number, not the wall number, that says how much work the
  parser actually did, and here libsimdxml does slightly more of it.


> **Numbers below come from a real run** (`ab_quick_20260924_001320`, 24.09.2026) — never
> from prose. Scope of that run: **C++ only, `sample_1mb.xml` + `sample_10mb.xml`**, 3
> counted repetitions in the timing loop (5 run, first 2 discarded as cold) and 3 memory
> repetitions (`--mem-reps=3`, sampling interval 500 µs). Rows the run did not cover
> (`sample_5mb.xml`, the 15–30 MB corpora, the non-C++ languages) still read
> `NO MEASUREMENT YET` — those arrive from the full-matrix run.
>
> **Do not mix these with the tables above.** That run went at ~3.3 GHz (governor
> `schedutil`, turbo on, `scaling_max_freq` 3.5 GHz) with load ≈ 2.0 at start; the speed
> tables above were taken at the 2.3 GHz base clock without turbo. The higher throughput
> below is the higher clock, not a faster parser — compare rows **within** a run.
>
> Every value is a **MIN over repetitions, per field** (the estimator without the
> allocator's upward-only error). The sampled `low`/`avg` carry the window coverage of
> their own series: **98–100% at 10 MB, 70–98% at 1 MB** (a 3.5 ms parse is shorter than
> the sampler's wake-up, so the beginning of a short parse is not always covered — the
> `low`/`avg` of those rows are a *weaker* lower bound, `peak` is unaffected).

### Memory (peak RSS, cross-language)

**Only one memory number is comparable across languages.** The `memory_peak_mb` field
in the runner JSON means something different in every language — C++ `VmHWM`,
Python `tracemalloc` (Python objects only), Node `heapUsed` (a live value, not even a
peak), Java used heap — so it must never go into a cross-language table. The legacy
`memory MB` column in the earlier harness speed reports is exactly that metric
(`memory_peak_mb`), kept for reference only and **not** comparable with the table below.

The shared metric is **`memory_peak_rss_mb`** — process peak RSS, read the same way in
every language: `VmHWM` from `/proc/self/status` (C++, Java),
`resource.getrusage(RUSAGE_SELF).ru_maxrss` (Python), `process.resourceUsage().maxRSS`
(Node). Only this one enters the cross-language table.

**Three numbers, not one: `low` / `avg` / `peak`.** Every (parser, file) gets three values,
because a single number cannot describe a parser's memory — how much the TREE holds versus
how much the COPY holds only shows up in the breakdown:

| number | JSON key | what it is | how measured |
|---|---|---|---|
| `low` | `memory_low_rss_mb` | **MIN** of the RSS samples over the `parse + count` phase | sampled → **lower bound** |
| `avg` | `memory_avg_rss_mb` | **mean** of the same samples | sampled → **lower bound** |
| `peak` | `memory_peak_rss_mb` | kernel high-water: `VmHWM` / `ru_maxrss` / `maxRSS` | **kernel — the authority on peak** |

`low` and `avg` are **sampled**, so they can only under-report: a sampler may miss a jump
between two samples, while the **kernel does not lose the peak** (a high-water mark only
grows, never shrinks). `peak` is therefore the authority on peak, and `low`/`avg` are read
as a **lower bound** — if any sample came out *above* `peak`, that is not a memory paradox
but a **measurement error** (the `⚠ SPRZECZNE` guard; see Guards below).

**Why `low` matters:** for a **streaming** parser `low` ≈ baseline + just the input buffer
(it holds no tree), for a **DOM** parser `low` ≈ baseline + input buffer + the **whole tree
in memory** — so `low` is the **stream/DOM difference expressed as one number**. `avg` shows
what the parser holds *while working* (growth, reallocation), and `peak` shows what the
process **must** have to get through at all.

**Memory record fields — the literal JSON keys** (these are real keys, read by the
orchestrator and the renderer, not descriptive labels):

| JSON key | meaning |
|---|---|
| `memory_low_rss_mb` | MIN of the RSS samples (lower bound) |
| `memory_avg_rss_mb` | mean of the RSS samples (lower bound) |
| `memory_peak_rss_mb` | kernel high-water (`VmHWM` / `ru_maxrss` / `maxRSS`) — the authority on peak |
| `memory_rss_baseline_mb` | **baseline: ONE RSS read after warmup, BEFORE loading the input file** (before any allocation for this run) |
| `memory_rss_after_mb` | ONE read **after the sampler stops**, before the structures are freed |
| `memory_samples` | how many samples were actually collected |
| `memory_sample_interval_us` | sampling interval (µs) — from the `--mem-interval-us` flag (default 500, range 100–2000) |
| `sampled_pid` | the PID actually sampled — the **leaf** of the process tree under the root PID, not the subshell's PID |
| `sampled_comm` | the sampled process's name (`/proc/<pid>/comm`) — proof that the parser was sampled, not `bash`/`timeout` |
| `pid_resolution` | how the PID was found: `"descendant"` (walked the process tree down to the leaf) or `"self_no_child"` (there was no child) |
| `n_resolves` | how many times the PID was resolved — re-resolved when the leaf disappeared while the root lived |
| `window_coverage_pct` | what fraction of the `[parse_start, parse_end]` window the sample series covers |
| `phase_ts_us` | monotonic phase markers (`baseline`, `parse_start`, `parse_end`, `after_free`, plus one per step) — the harness **cuts the sampler series by them** (one clock for every language) |
| `memory_samples_phase` | sampling window; value `"parse+count"` |
| `memory_sampled_method` | a **harness-owned** field with exactly two values: `"statm_external"` (series from the separate `tools/rss_sampler.py` process — the default path) or `"none"` (the sampler could not be attached) |
| `memory_sampler_error` | a short string, standing **next to** `"none"`; the `BRAK PRÓBEK` guard accompanies it (never zero in the metrics) |
| `memory_sampled_method_inproc` | **runner-owned, and only for cpp / java** (the `--mem-inproc` control path). What really occurs: `"statm_inproc"` (cpp, java) and `"unavailable_nonlinux"` (off Linux). The literal `"unsupported_runner"` stays in the contract as a **reserved (defensive)** value — **no runner emits it**, so it is not a normal case |
| `memory_series_mb` | the **raw sample series** (not just the statistics); the full series also lives as files under `results/mem_series/` |
| `memory_sampled_inconsistent` | bool — the sampled maximum exceeded the kernel high-water (the `⚠ SPRZECZNE` guard) |
| `memory_avg_over_baseline_mb` | `avg − baseline` |
| `memory_peak_over_baseline_mb` | `peak − baseline` |
| `bytes_per_node_avg` | B/node from the `avg` delta (main table column) |
| `bytes_per_node_peak` | B/node from the `peak` delta (descriptive table) |
| `ratio_mem_peak` / `ratio_mem_avg` / `ratio_mem_low` | the three ratios against pugixml |

**Runner fields ≠ the harness record.** The runners' JSONs carry fields the harness
contract does not list — that is not a discrepancy, just the layer below:
`memory_series_source` (where the series came from), `memory_peak_rss_method` (what the peak
was read with), `memory_inproc_enabled` (whether the control path was switched on),
`memory_steps` (C++, `--mem-steps`), plus the `*_inproc` fields and
`memory_inproc_sampler_cpu_ms` (how much CPU the in-process sampler ate). The key sentence:
**the runner DELIBERATELY does not fill `memory_low_rss_mb` / `memory_avg_rss_mb`** — the
source of truth for `low`/`avg` is **exclusively the external sampler**, and the series in
the runners' JSONs are **empty**, so nobody takes them for a measurement.

**The phase-marker clock.** The `phase_ts_us` markers are an **absolute `CLOCK_MONOTONIC` in
ALL languages**, and the **`clock` field tells the truth about the source**. A fallback to
`CLOCK_REALTIME` (off Linux, or a failed read) makes the window **suspect** — the series is
then computed from the whole run with a `window_used` label. We do not assume up front that
the cut will succeed: **`clock` and `window_used` will show it**. Rust is not an exception
here — it takes an absolute clock through `extern "C"` to `clock_gettime` (no `libc` crate,
no `Cargo.toml` changes), so after this fix all 7 languages publish the same clock.

**Method.** Peak RSS per (parser, file), one process per parser — nothing is summed
across libraries. The memory phase runs `--mem-reps=N` (default 3, range 1–10) and
reports the **MIN per field**, not a mean or a median: `malloc_trim(0)` returns only the
contiguous top of the heap, so a block freed in the middle of the arena stays in RSS and
the error has one direction only (upward) — MIN is the estimator without that artefact.
All raw reads are kept (`*_all` arrays) next to `spread_pct` = (max−min)/min × 100;
`spread_pct > 20%` renders a `⚠ ROZRZUT` marker. The 1 MB read is **bimodal** — identical
node counts and identical work, yet the reported peak can differ substantially between
identical runs — an allocator artefact, not parser behaviour, so a memory number is never
announced from a single shot: take the MIN of `--mem-reps` and look at `spread_pct`.

**How `low`/`avg` are sampled.** The sampler is a **SEPARATE PROCESS**
(`tmp/benchmark/tools/rss_sampler.py`), **not a thread** inside the measured process. The
reason, stated plainly: the measured process **pays nothing** (no thread, no wakeups, no
locks), and on a 2-core CPU a sampling thread would **steal cycles from the parser**. A
side effect that is a real advantage: the method is **identical across all 7 languages**,
so the GIL/GVL problem in Python and Ruby **disappears** — it only ever existed for
sampling *inside* the process. A sample is `/proc/<PID>/statm`, field 2 (resident) × page
size, read from the outside by PID. **Which PID matters — not the subshell's:** the harness
publishes the **root** PID (`--pid-file`, atomic write) and the sampler **walks the process
tree down to the leaf** (`/proc/<pid>/task/<tid>/children`), because a runner starts as
`bash → timeout → binary`, so `$!` from `exec_runner … &` is the **bash subshell's** PID (a
small, near-constant RSS), not the parser's. When the leaf disappears while the root lives,
the PID is **resolved again**; the number of resolves rides in `n_resolves`, and the outcome
(`root_pid`, `sampled_pid`, `sampled_comm`, `pid_resolution` = `descendant` /
`self_no_child`) goes into the series metadata. **The series unit is kB**
(`<ts_us>\t<rss_kb>`), stated explicitly as `rss_unit: "kB"` in that metadata; the harness
converts kB → MiB **exactly once** — a missing or different `rss_unit` raises
**`⚠ NIEZNANA JEDNOSTKA SERII`**, never a silent assumption (reading those numbers as MiB
inflated them by a factor of 1024). A new sampler `stop_reason` is **`no_pid_file`** — the
wait for the PID file timed out. **The interval has a flag: `--mem-interval-us=N`**
(default **500 µs**, range **100–2000**) — it covers **the external sampler in the harness**
and **`-Dbench.mem.interval_us=<N>` for Java**. **No runner knows this flag** (`grep
mem-interval-us` across `runners/**` = 0 hits), so **it is not sent to them**; the built-in
control path in C++ has **its own fixed interval (~150 µs)** and does not take an interval
from the CLI. The interval actually used is reported in `memory_sample_interval_us`.
**At 1 MB use `--mem-interval-us=100`:** a parse there takes ~4 ms, so with a 500 µs
interval only a few samples fit inside the parse window — the window coverage is weak (see
`window_coverage_pct`), and the series computed from the whole run carries a `window_used`
label. **Protocol:** the series goes to a **file under
`results/mem_series/`** and **NEVER to stdout** — the runner's stdout is the JSON protocol
(writing the series there would break the result parser); the runner publishes only
**monotonic markers** `phase_ts_us` (`baseline`, `parse_start`, `parse_end`, `after_free`,
plus one per step), and the harness **cuts the series by those markers** — one system clock
for every language. **Start order: the sampler comes up BEFORE the runner.** The sampler
starts first and announces readiness through a `--ready-file`; **only then** does the harness
publish the PID (`--pid-file`, atomic write) and launch the runner. The reason: Python's boot
takes longer than a fast parser's whole parse (milliseconds), so the old "in parallel" order
produced series with zero samples (`BRAK PRÓBEK`) and ate CPU inside the parse window.
**Waiting for the sampler's readiness is OUTSIDE the measurement** — that is instrument
startup, not the memory phase. **Window:** the sampler **lives only around one memory-phase
run**, and the series is cut by the `phase_ts_us` markers, so the `[parse_start, parse_end]`
window covers **file read + parse + count** (`memory_samples_phase` = `"parse+count"`); the
fraction of that window actually covered by samples is reported in `window_coverage_pct`.
The **single reads do not perturb anything**:
`memory_rss_baseline_mb`, `memory_rss_after_mb` and `memory_peak_rss_mb` are **one read
each** (baseline before the input file is loaded, `after` once the sampler has stopped,
`peak` from the kernel). The **raw series stays in the result** (`memory_series_mb` plus the
files under `results/mem_series/`) — without it you cannot tell "the parser did not
allocate" from "the sampler was not looking", nor see the growth (reallocation) phase.
**`BRAK PRÓBEK` is now a FAILURE, not a normal case:** with an external sampler the Node
"JS timer does not tick" issue and the Python/Ruby GIL/GVL issue **stop being problems** —
we read another process from the outside, so a purely interpreted parser that never yields
control still gets sampled. `BRAK PRÓBEK` now means the **external sampler died / did not
start / wrote no series**, not "that language is like that"; the render prints it
explicitly and **never zero** (a zero would be a claim about memory that we do not have).
**A note on the values:** `memory_sampled_method` is a **harness-owned** field with exactly
two values — `"statm_external"` (series from `tools/rss_sampler.py`, the default path) or
`"none"` (the sampler could not be attached; `memory_sampler_error` stands next to it with
the `BRAK PRÓBEK` guard, never zero in the metrics). The literals
`"statm_thread"` / `"unavailable_gil"` / `"unavailable_nonlinux"` do **NOT** belong to that
field — they describe the control path and live under a separate key
**`memory_sampled_method_inproc`**: `"statm_inproc"` / `"unavailable_gil"` /
`"unavailable_nonlinux"` / `"unsupported_runner"`. Of that set, **only `"statm_inproc"`**
(cpp, java) **and `"unavailable_nonlinux"`** (off Linux) really occur; `"unsupported_runner"`
is **reserved (defensive) and no runner emits it**, and **the field being absent for the
other five languages is CORRECT** — the control path does not exist there at all (the flag is
not passed), so the render emits a warning plus a "uwagi" column, never a zero or a
fictitious value. The **`--mem-inproc` control path exists ONLY in cpp and java**, gated by
the **`ORCH_MEM_INPROC_LANGS`** list (default `cpp,java`; adding a language means adding it to
that list) — for the other five the flag **is not passed at all** (Python runs `argparse`
with `required=True` and no `parse_known_args` → `exit 2`, i.e. the whole run is lost).
The `_inproc` suffix exists because
**both runs can be present in
ONE record** (cross-checking the external sampler), and "two truths in one table" has
already cost us once. Reliability rides along in the reliability columns —
`memory_samples` (how many samples were actually collected), `memory_sample_interval_us`
and `window_coverage_pct` (what fraction of the parse window the series covers) — plus the
**sampler honesty fields** (`sampled_pid`, `sampled_comm`, `pid_resolution`, `n_resolves`);
**fewer than 5 samples renders `avg` as `ORIENTACYJNY`**
("indicative") — the number stays visible, but labelled. `memory_sampled_inconsistent`
(bool) is set when the sampled maximum exceeds the kernel high-water.

**Hard guarantee: the timing verdict is computed WITHOUT the sampler.** In the **timing
loop the sampler does not exist** — it starts and is killed **only around the memory
phase**. The timing verdict comes **exclusively** from the sampler-free phase;
`cpu_source: mem_parse` **is not the verdict**. That is why the memory phase can be
expensive without harming the result: its cost enters no timing ratio.

**Disturbance is MEASURED, not assumed.** We do not assume the sampler is harmless — we
check it: `results/mem_<STAMP>.md` and `results/machine_<STAMP>.md` carry a comparison of
**`parse_time` from the memory phase vs from the timing phase**, plus the **sampler's own
CPU** and its **actual interval and gaps** (whether it holds the requested
`--mem-interval-us`). Crossing **5%** raises the guard **`⚠ PRÓBKOWANIE ZABURZA POMIAR`**:
the memory result is then suspect and must be re-run (or sampled less often) — not waved
away as noise.

**"Top per measurement", but MANY measurements.** Per-repetition series stay **raw** —
**top / min / avg per measurement**, with no reduction to a single number (that is the
direct answer to "many measurements, not just the top value"). **Key caveat:** within a
**single** process `VmHWM` is **cumulative and monotonic** — once raised it never falls, so
a "per-repetition peak" read from `VmHWM` **is meaningless** (the second rep would see the
first rep's peak). In that case: **top per repetition comes from the SERIES**
(`memory_series_mb`), and **`memory_peak_rss_mb` is ONE number** — the whole-process
high-water (the authority on the run's peak), not a single rep's peak.
**`--mem-fresh` is a NO-OP in THIS harness, and it has to be written that way.** The flag
exists for harnesses whose memory reps **share one process**: there, OFF means "top per rep
from the series, `VmHWM` = one number for the whole process" and ON switches to a process
per rep. **In libsimdxml there is nothing to fix — a memory rep is ALREADY a separate runner
process**, so the per-rep `VmHWM` is a true per-process peak. The flag is **accepted,
recorded as `mem_fresh` in `ABMEM`, and does not change the measurement**; the doc must not
claim it switches anything.

**Consequence (an asymmetry, not a detail):** since **every memory rep is a separate
process**, the JVM and Node **pay warmup in EVERY rep** — unlike the timing loop, where reps
share one process and the runtime is already warm. **The memory phase therefore measures a
COLD runtime, the timing phase a WARM one** — these are not the same conditions (the
baseline delta keeps the runtime out of the parser figure, but it does not warm the runtime
up).

**Across repetitions (`--mem-reps`).** Every statistic is computed **per field**: MIN /
MAX / mean / median / `spread_pct`. **MIN stays the leading estimator** (the trim artefact
can only inflate), but the rest must stay visible — **without MAX, mean and median the
1 MB bimodality is invisible**, because MIN alone always picks the lower branch of the
distribution and hides the fact that another shot produced a completely different number.

**Parser memory vs runtime overhead (delta from baseline).** Raw process `avg`/`peak` mix
**parser memory** with **runtime overhead** (JVM startup, V8 heap, the Python interpreter).
Hence the deltas `memory_avg_over_baseline_mb` and `memory_peak_over_baseline_mb`, and
**B/node is computed from the DELTA**, not from process RSS — otherwise the JVM and Node,
with their high startup, look falsely memory-hungry and the table ends up comparing
**runtimes, not parsers**. Two variants exist: `bytes_per_node_avg` (from the `avg` delta —
the main table column) and `bytes_per_node_peak` (from the `peak` delta — descriptive
table).

**`baseline` = ONE RSS read after warmup, BEFORE loading the input file**
(`memory_rss_baseline_mb`) — i.e. before any allocation for this run. The consequence,
read literally: **`avg − baseline` = input buffer + structures TOGETHER**. That is not a
flaw — **every parser reads the same file**, so the buffer is part of a fair comparison;
splitting "buffer vs structures" only comes from `--mem-steps` in C++ (below).

**Per-step breakdown (C++, `--mem-steps`).** Peak RSS split into input buffer → parser
structures → residue left after destroying them; **each step has its OWN sampling window
and its OWN sample series**, collected in `memory_steps`. Plus `bytes_per_node_avg` /
`bytes_per_node_peak` (a size-independent figure, so parsers can be compared across corpus
sizes) and `heap_inuse_mb` (`mallinfo2`).

**`ratio_mem`** = parser RSS / pugixml RSS. pugixml is the reference (same language, same
class of task), so the convention is `< 1.0` → our parser uses less memory. It is computed
from the MINs of both sides.

There are **three ratios** — `ratio_mem_peak`, `ratio_mem_avg`, `ratio_mem_low`, each from
the MINs of both sides — because they answer three different questions: what the process
must have to get through (`peak`), what the parser uses typically (`avg`), and the lower
bound (`low` — ≈ baseline + buffer for a streaming parser, ≈ baseline + buffer + the full
tree for a DOM one). A single unlabelled `ratio_mem` would not say which of the three it
compares.

**No mode is blind to CPU.** The memory phase also measures CPU (user+sys) and wall time of
its own parses, plus the machine context — but that CPU is explicitly *not* a timing
verdict: the context line carries `cpu_source: mem_parse`, while the verdict comes only
from the timing phase (`cpu_source: ab_parse`). `--mem-only` (a memory sweep without the
A/B loop) still emits the full machine context.

**Outputs.** `results/mem_<STAMP>.md` (the memory table **plus the `parse_time` comparison
memory phase vs timing phase**), `results/machine_<STAMP>.md` (machine context **plus the
sampler's CPU and its actual interval/gaps**), `results/mem_series/` (**raw sample series
files** — raw TSV in **kB** with `rss_unit: "kB"` in the metadata, converted kB → MiB exactly
once by the harness; they never go to stdout, that is the JSON protocol), the `ABMEM` line in
the run's result files (including the sampler honesty fields `sampled_pid`, `sampled_comm`,
`pid_resolution`, `n_resolves`, `window_coverage_pct`), and a memory section in the aggregate
`REPORT.md`. The run summary carries **both axes** (time and memory).

**Guards — explicit strings, never a silent zero.**

| string | when it appears | what it means |
|---|---|---|
| `BRAK PRÓBEK` | the **external sampler died / did not start / wrote no series** — `memory_sampled_method` = `"none"` with `memory_sampler_error` next to it (not "that language is like that": GIL/GVL and the JS timer are not issues with an external sampler) | no `low`/`avg` measurement — **not zero** |
| `⚠ SPRZECZNE (próbkowany max > VmHWM)` | any RSS sample came out above the kernel high-water (sets `memory_sampled_inconsistent`) | **measurement error**: `VmHWM` only grows, so no real sample can exceed it |
| `⚠ PRÓBKOWANIE ZABURZA POMIAR` | `parse_time` from the memory phase differs from the timing phase by **more than 5%** | the memory phase is loading the measurement — the memory result is suspect, to be re-run / sampled less often |
| `NIEPEŁNE (n/M) — patrz .json` | a field is missing from the memory record | incomplete record (same class as `cpu_ms NIEPEŁNE (n/14)` on the timing side) |
| `⚠ ROZRZUT` | `spread_pct > 20%` | unstable reading across repetitions; the value is still a MIN, but the marker stays |
| `⚠ PRÓBKOWANY PROCES TO NIE PARSER (sampled_comm=…)` | `sampled_comm` is `bash` / `sh` / `timeout` / `docker` | a **subshell** was sampled, not the parser — the series describes the wrong process |
| `⚠ NIE ZNALEZIONO POTOMKA (pid_resolution=self_no_child)` | the PID resolved to itself because there was no child under the root | nothing to sample — a record without a series `low`/`avg` |
| `⚠ POKRYCIE OKNA <x%` | `window_coverage_pct` below the threshold | the samples cover only part of the parse window — `low`/`avg` are a weak lower bound |
| `⚠ JEDNA PRÓBKA W OKNIE PARSE` | exactly **one** sample fell inside `[parse_start, parse_end]` | one point gives no distribution — read `avg` as `ORIENTACYJNY` |
| `⚠ NIEZNANA JEDNOSTKA SERII` | the series metadata has no `rss_unit` key, or a value other than `"kB"` | the unit is never assumed silently — without `rss_unit` the series is not converted |
| `BRAK linii ABMEM dla tego pliku` | a run without the memory phase (older log / `--no-mem`) | no measurement — **not zero and not a silent dash** |

One row per (corpus, parser) — `low` / `avg` / `peak` plus the deltas from baseline:

| Corpus | parser | `memory_low_rss_mb` | `memory_avg_rss_mb` | `memory_peak_rss_mb` | `memory_avg_over_baseline_mb` | `memory_peak_over_baseline_mb` | `bytes_per_node_avg` | `spread_pct` | `memory_samples` | `memory_sample_interval_us` | nodes |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `sample_1mb.xml` | libsimdxml | 6.77 | 8.29 | **9.39** | 3.12 | 4.32 | 194.5 | 1.66 | 13 | 501 | 16,824 |
| `sample_1mb.xml` | pugixml (reference) | 6.81 | 8.72 | 10.67 | 3.54 | 5.52 | 220.9 | 0.29 | 17 | 501 | 16,824 |
| `sample_1mb.xml` | expat (SAX) | 6.43 | 7.38 | 7.38 | 2.24 | 2.29 | 139.8 | 0.75 | 25 | 500 | 16,824 |
| `sample_1mb.xml` | libxml2 (DOM) | 6.79 | 14.47 | 24.81 | 9.33 | 19.71 | 581.6 | 0.38 | 77 | 500 | 16,824 |
| `sample_5mb.xml` | libsimdxml | NO MEASUREMENT YET | NO MEASUREMENT YET | NO MEASUREMENT YET | NO MEASUREMENT YET | NO MEASUREMENT YET | NO MEASUREMENT YET | NO MEASUREMENT YET | NO MEASUREMENT YET | NO MEASUREMENT YET | NO MEASUREMENT YET |
| `sample_5mb.xml` | pugixml (reference) | NO MEASUREMENT YET | NO MEASUREMENT YET | NO MEASUREMENT YET | NO MEASUREMENT YET | NO MEASUREMENT YET | NO MEASUREMENT YET | NO MEASUREMENT YET | NO MEASUREMENT YET | NO MEASUREMENT YET | NO MEASUREMENT YET |
| `sample_10mb.xml` | libsimdxml | 15.73 | 33.63 | **46.72** | 28.46 | 41.70 | 177.7 | 0.42 | 121 | 500 | 167,972 |
| `sample_10mb.xml` | pugixml (reference) | 15.52 | 38.35 | 58.00 | 33.24 | 52.91 | 207.5 | 0.15 | 113 | 500 | 167,972 |
| `sample_10mb.xml` | expat (SAX) | 15.45 | 25.19 | 25.44 | 20.09 | 20.34 | 125.4 | 0.40 | 251 | 500 | 167,972 |
| `sample_10mb.xml` | libxml2 (DOM) | 15.74 | 105.89 | 197.37 | 100.70 | 192.21 | 628.6 | 0.03 | 729 | 500 | 167,972 |

Reading the rows: `low` separates the two families exactly as expected — the SAX-style
parsers (libsimdxml, pugixml, expat) sit at 6.4–6.8 MiB at 1 MB, i.e. **baseline + input
buffer and nothing else**, while the DOM parser (libxml2) climbs to 24.8 MiB at 1 MB and
197.4 MiB at 10 MB. `avg` shows what each parser holds while working; the delta columns are
in MB. tinyxml2 has no row: it **errors out on this corpus** (`XML_ERROR_PARSING_DECLARATION`
on the processing-instruction line — the pre-existing limitation noted above), so its
memory numbers would describe a failed parse.

Ratios against pugixml — **one per number** (MINs of both sides):

| Corpus | `ratio_mem_peak` | `ratio_mem_avg` | `ratio_mem_low` |
|---|---|---|---|
| `sample_1mb.xml` | **0.881** | **0.950** | 0.993 |
| `sample_5mb.xml` | NO MEASUREMENT YET | NO MEASUREMENT YET | NO MEASUREMENT YET |
| `sample_10mb.xml` | **0.806** | **0.877** | 1.013 |

Below 1.0 means libsimdxml uses **less** memory than the reference. The peak advantage grows
with size (0.881 → 0.806): libsimdxml's index is a flat buffer of offsets, while pugixml
allocates a node object per element. `low` is at parity (0.99 / 1.01) — at the bottom of the
parse both processes are just holding the input.

> **Why `low` is in the table:** for a streaming parser `low` ≈ baseline + just the input
> buffer (it holds no tree), for a DOM parser ≈ baseline + buffer + the full tree — the
> **stream/DOM difference as one number**.

The larger corpora (15–30 MB) come from the same matrix. The tables are filled in **only**
from a `results/mem_<STAMP>.md` produced by a run — never by hand. The 24.09.2026 run above
covered `sample_1mb.xml` and `sample_10mb.xml`; `sample_5mb.xml` and everything above 10 MB
fill in from the full-matrix run, which is one command:

```bash
bash tmp/benchmark/orchestrator.sh --mem --mem-reps=3        # --mem is on by default
bash tmp/benchmark/orchestrator.sh --mem-only --mem-reps=3   # memory sweep only, no A/B loop

# sampler tuning (the sampler is an external process; none of this touches the timing loop)
bash tmp/benchmark/orchestrator.sh --mem --mem-interval-us=500   # 100–2000 µs, default 500 (use 100 at 1 MB)
bash tmp/benchmark/orchestrator.sh --mem --mem-fresh             # NO-OP here (memory reps are already separate processes)
bash tmp/benchmark/orchestrator.sh --mem --mem-inproc            # control path: cpp/java only (ORCH_MEM_INPROC_LANGS, default cpp,java)
```

## Usage

```cmake
add_subdirectory(libsimdxml)
target_link_libraries(my_app PRIVATE simdxml)
```

```cpp
#include <simdxml/simdxml.hpp>
#include <simdxml/index/xml_index.hpp>
#include <iostream>
#include <span>
#include <string>

int main() {
    std::string xml = R"(<?xml version="1.0"?>
<catalog>
  <book id="1"><title>XML in Practice</title></book>
  <book id="2"><title>SIMD for Everyone</title></book>
</catalog>)";

    auto bytes = std::as_bytes(std::span{xml.data(), xml.size()});

    // Parse: single pass, zero copy, runtime-dispatched SIMD.
    auto idx = rai::xml::parse(bytes);
    if (!idx) {
        std::cerr << "parse error\n";
        return 1;
    }

    // Precompute tree indices (CSR children) and the name index.
    idx->ensure_indices();
    idx->build_name_index();

    std::cout << idx->tag_count() << " tags, max depth "
              << idx->max_depth() << "\n";

    // All <title> elements, in document order.
    for (auto t : idx->tags_by_name("title"))
        std::cout << idx->tag_name(t) << "\n";
}
```

XPath 1.0:

```cpp
#include <simdxml/xpath/xpath.hpp>

auto xp  = rai::xml::CompiledXPath::compile("//book/title");
auto res = xp->eval(*idx);
if (res) {
    for (auto const& node : *res) { /* ... */ }
}
```

## Scope & status

- Lenient parser: does not validate XML well-formedness (same behavior as the
  original crate's fast path). Garbage in does not crash it, but the index of
  malformed documents is best-effort.
- Documents nesting deeper than 32 levels are indexed with degraded parent
  links (fixed-size parent stack in `index/structural.cpp`).
- The `.sxi` persistence format validates a hash and re-parses on mismatch,
  but is intended for trusted cache files, not hostile input.
- HTML is not a supported input.

## Credits & License

- Code: MIT License — © 2026 KrzysztofKowalski (see `LICENSE`).
- Derived from the Rust crate
  [simdxml](https://github.com/simdxml/simdxml) by Christopher Grainger
  (MIT OR Apache-2.0). The original copyright notice is retained in `NOTICE`
  as required by its license.