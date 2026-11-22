# Shopping Sessions (shopping-sessions-4)

An exact C++20/OpenMP implementation for the [JoinBuilders Shopping Sessions track](https://www.joinbuilders.org/buildathon/tracks/shopping-sessions). It reads every input CSV and writes the two final Parquet tables directly. Snappy is linked statically; no Python, DuckDB, network access, or intermediate output conversion is needed at runtime.

The practice workload is about 20 million events; the final is about 60 million. A sub-second dev-set result is not a practice score. Only an official run establishes leaderboard placement.

## Run

```bash
docker build --platform linux/amd64 -t shopping-sessions .
mkdir -p out scratch
chmod a+rwx out scratch
docker run --rm --platform linux/amd64 --network none --read-only \
  --tmpfs /tmp:rw,size=64m,mode=1777 \
  --cap-drop ALL --security-opt no-new-privileges \
  --cpus 8 --memory 24g --memory-swap 24g --user 1000:1000 \
  -e DATA_DIR=/data -e OUTPUT_DIR=/output -e SCRATCH_DIR=/scratch \
  -e HOME=/scratch -e NCPU=8 -e MEM_GB=24 \
  -v "$PWD/data:/data:ro" -v "$PWD/out:/output" -v "$PWD/scratch:/scratch" \
  shopping-sessions
```

Use an empty output directory for each run. The image writes one or more Parquet files directly into `user_daily/` and `campaign_daily/`, including schema-only files when a table is empty. `day` is a Parquet DATE and `top_category` is nullable. The same Dockerfile also supports native ARM64 builds for development; submit a `linux/amd64` image.

## Algorithm

1. Scan CSV delimiters, newlines, and quotes together. Runtime dispatch selects AVX-512BW, AVX2, SSE2, or NEON, with bounded reads and a scalar fallback for long lines and final fragments. Validate the exact dialect, including ordinary carriage returns and quoted empty fields.
2. Intern linked identities, compute transitive components, and select the byte-wise smallest name. Packed numeric indexes have an exact string fallback and promote to full-width internal indexes when necessary.
3. Parse compact event candidates, preserving invalid candidates until deduplication chooses the earliest ingestion. Reuse already validated ingestion dates where they match the event date.
4. Deduplicate independent event-ID partitions and validate each partition's winners while the data is local. Hash-table lookup uses bits beyond the partition selector, avoiding correlated probe chains.
5. Resolve standalone numeric identities in independent shards. Sort surviving events by person/time, then count and fill session output ranges in parallel over whole people.
6. Attribute each person's sessions and merge seven-day campaign-assist intervals in the same parallel work unit. Reduce private campaign totals once. Sum purchase categories per person/day with exact byte-wise tie-breaking.
7. Write Snappy-compressed Parquet pages directly from final aggregate arrays, with correct compressed and uncompressed metadata lengths.

Fast paths do not normalize IDs or depend on a dataset seed, fixture name, date window, or particular dimension cardinality. Progress and timing logs go to stderr.

## Validation and benchmarking

The workflow checks all 36 official edge fixtures, compares the seed-1 1M dev set with the official answer, compares a second seed with the previous implementation, and benchmarks 20M events plus 5M clicks against the previous main commit. Parser differential fuzzing, identity index promotion, timestamp boundaries, and sorting under an OpenMP thread limit run with AddressSanitizer and UndefinedBehaviorSanitizer.

```bash
docker build --platform linux/amd64 --target tests -t shopping-sessions:tests .
docker run --rm --platform linux/amd64 --network none \
  -e OMP_THREAD_LIMIT=2 shopping-sessions:tests

python3 scripts/benchmark.py --data bench --output benchmark-results \
  --image shopping-sessions --baseline-image shopping-sessions:baseline \
  --cpus 8 --memory 24g --runs 5 \
  --compare-script starter-duckdb/scripts/compare.py
```

The benchmark alternates execution order, warms both implementations, creates fresh output directories, and records all runs and phase medians in `results.json`. It reports engine time and launcher wall time separately. Docker wall time includes the CLI/container launch and exit; native timings from another CPU architecture are not scorer predictions. A native binary can be measured with `--engine PATH --baseline-engine PATH` instead of the image options.
