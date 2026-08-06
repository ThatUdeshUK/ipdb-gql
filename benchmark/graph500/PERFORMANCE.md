# Scaling DuckGQL PageRank past 100 million edges

## What changed

DuckGQL originally ran PageRank as a single-threaded push computation. Each
source vertex scattered its contribution into a shared next-rank vector. That
layout was simple, but it prevented safe target-range partitioning and left the
machine's other cores idle.

With multiple configured threads, the current kernel is an incoming-edge pull
computation. Each task exclusively owns a range of target vertices and reads
the sources in that target's incoming adjacency. This removes writes shared
between workers and needs no atomic floating-point updates. Each task reports
its convergence difference and next dangling mass; the caller reduces those
small results in deterministic task order. With one configured thread,
DuckGQL retains the outgoing push kernel because its sequential memory access
is faster than single-threaded pull.

PageRank now asks the CSR builder for exactly two topology structures:

- incoming offsets and compact incoming-neighbor ordinals;
- one compact outgoing degree per vertex.

For graphs below 2^32 vertices, both neighbor ordinals and degrees use four
bytes. Edge IDs, edge labels, outgoing neighbors, vertex labels, postings, and
edge statistics are not materialized for unfiltered PageRank. If a compatible
full outgoing CSR already exists, its offset differences satisfy the degree
requirement without creating a duplicate sidecar.

The builder also substitutes typed constants for edge-ID, rowid, and label
columns when the requested projection does not need them. DuckDB can therefore
scan only the source and target columns for this topology-only path.

## Measurement API

Wall-clock query timing alone could not separate CSR construction from the
algorithm. The connection-local function below now exposes the latest run:

```sql
SELECT *
FROM gql_algorithm_stats('graph500', 'pagerank');
```

It reports whether the CSR was built or reused, CSR time, initialization time,
iteration time, output time, total time, task-partition count, graph size,
iteration count, and convergence. This makes cold and warm measurements
explicit rather than inferred.

## Test setup

These are local release-build measurements from 2026-08-05:

- Apple arm64 host, macOS 26.3.1;
- 24 GiB physical memory;
- DuckDB v1.5.5 with 15 configured threads;
- DuckDB `memory_limit = '10GB'`;
- official Graph500 generator, edge factor 16;
- directed tuple orientation, with no symmetrization;
- damping 0.85, tolerance 1e-8, at most 100 iterations;
- previously imported databases reused so import time is excluded.

The SQL regression suite passed 2,520 assertions in 18 test cases. PageRank
converged in 27 iterations at scale 20 and 28 iterations at scale 23. Rank sums
were 1.000000000009598 and 1.000000000096708 respectively.

## Results

| Scale | Vertices | Directed edges | CSR | Cold CSR + PageRank | Warm PageRank | Task partitions |
|---:|---:|---:|---:|---:|---:|---:|
| 20 | 1,048,576 | 16,777,216 | 76.00 MiB | 0.843 s | 0.270 s | 15 |
| 23 | 8,388,608 | 134,217,728 | 608.00 MiB | 12.155 s | 6.946 s | 15 |

At scale 20, internal cold-run timing attributes 0.565 s to CSR construction,
0.005 s to initialization, 0.268 s to iterations, and 0.002 s to output. The
warm run spends 0.261 s in iterations.

A separate scale-20 thread sweep measured the iteration phase immediately
after each cold CSR build:

| DuckDB threads / task partitions | Iteration time | Speedup vs 1 thread |
|---:|---:|---:|
| 1, serial push | 0.881 s | 1.00x |
| 2, parallel pull | 0.828 s | 1.06x |
| 4, parallel pull | 0.416 s | 2.12x |
| 8, parallel pull | 0.327 s | 2.69x |
| 15, parallel pull | 0.268 s | 3.29x |

This is one local sweep rather than a multi-run median, but it confirms that
the target partitioning produces real parallel scaling. Two-thread pull only
roughly matches the cache-friendly serial push path; the clear gains begin at
four workers and returns diminish after that on this host.

At scale 23, internal cold-run timing attributes 7.575 s to CSR construction,
0.042 s to initialization, 4.508 s to iterations, and 0.020 s to output. The
warm measured run spends 6.874 s in iterations. The difference between cold
and warm iteration time is run-to-run scheduling and thermal variability; the
benchmark does not claim a stable cache effect from a single sample.

## Before and after at scale 23

The directly comparable earlier algorithm-owned CSR run used the same imported
scale-23 graph and convergence settings:

| Measurement | Single-threaded push | Parallel pull | Improvement |
|---|---:|---:|---:|
| Cold CSR + PageRank | 22.795 s | 12.155 s | 1.88x |
| Warm PageRank | 16.506 s | 6.946 s | 2.38x |
| CSR memory | 576.00 MiB | 608.00 MiB | 32.00 MiB added |

The added memory is exactly the four-byte outgoing degree sidecar at this
scale. It avoids materializing another 512 MiB neighbor array and enables the
target-partitioned pull kernel. The trade is favorable here: 5.6% more CSR
memory for a 2.38x warm-kernel speedup.

An older scale-20 full-CSR run measured warm PageRank at 0.629 s. The new
algorithm-specific projection completes in 0.270 s, or 2.33x faster, but that
comparison also includes the change from a full snapshot to the smaller
algorithm-owned layout.

## What the large earlier runs established

Before this parallel change, the algorithm-owned push implementation completed
scale 25 in 104.347 s cold and 78.292 s warm, using a 2.25 GiB CSR. Scale 26
completed in 234.686 s cold and 161.905 s warm, using a 4.50 GiB CSR. Those
runs established local feasibility at 536,870,912 and 1,073,741,824 directed
edges. They are not new parallel-pull results and should not be used to claim a
scale-25 or scale-26 speedup.

With the degree sidecar, the estimated native CSR plus PageRank-vector memory
is about 2.88 GiB at scale 25 and 5.75 GiB at scale 26. DuckDB's memory limit
does not cap these native extension allocations, so the runner keeps a separate
physical-memory safety guard.

## Reproduce it

```sh
make release

python3 benchmark/graph500/run_pagerank.py \
  --scales 20 23 \
  --memory-limit 10GB \
  --work-dir /tmp/duckgql-graph500
```

The runner preserves imported databases, writes per-scale logs, and records
both wall-clock and internal phase measurements in `pagerank-results.json`.
One run is useful for engineering direction, but publication-quality numbers
should add warmups and report medians across repeated runs.

## Next bottleneck

At scale 23, CSR construction now dominates the cold run, while PageRank's
iteration loop dominates the warm run. The next PageRank-specific work should
therefore be measured thread-scaling and partition balancing, followed by
parallel CSR degree counting and scatter. Across DuckGQL more broadly, the same
task infrastructure can then move to frontier-based BFS/SSSP and deterministic
WCC.
