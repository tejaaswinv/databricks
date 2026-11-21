# Shopping Sessions (shopping-sessions-4)

A fast, exact shopping-session ETL implementation for the [JoinBuilders buildathon](https://www.joinbuilders.org/buildathon/tracks/shopping-sessions).

**Stack:** C++20 (native optimized engine, union-find, linear time attribution, merged campaign-assist windows) + DuckDB 1.5.6 (typed Parquet output).

## Validation

- All **36/36** official CSV edge-case fixtures passed.
- Official Rust generator, seed 1, 1,000,000 events + 250,000 clicks: **MATCH** against `dev-answer.zip`.
- `user_daily`: 117,038 rows, 0 missing/extra/different; `campaign_daily`: 27,400 rows, 0 missing/extra/different; 0 duplicate output keys.
- GitHub Actions runs the Docker image with a read-only filesystem, uid 1000, disabled networking, and capped resources. [Validation logs](https://github.com/tejaaswinv/databricks/actions).
- Native engine reported **0.819 seconds** for the official 1M dev set on a GitHub Actions runner; that figure excludes container startup and Parquet conversion. No official hidden practice score is claimed.

## Run

```bash
docker build --platform linux/amd64 -t shopping-sessions .
mkdir -p out scratch
chmod a+rwx out scratch
docker run --rm --platform linux/amd64 --network none --read-only \
  --tmpfs /tmp:rw,size=64m,mode=1777 \
  --cpus 8 --memory 24g --memory-swap 24g --user 1000:1000 \
  -e DATA_DIR=/data -e OUTPUT_DIR=/output -e SCRATCH_DIR=/scratch \
  -e HOME=/scratch -e NCPU=8 -e MEM_GB=24 \
  -v "$PWD/data:/data:ro" -v "$PWD/out:/output" -v "$PWD/scratch:/scratch" \
  shopping-sessions
```

The container writes `/output/user_daily/part-0.parquet` and `/output/campaign_daily/part-0.parquet`, with exact columns and a Parquet `DATE` type for `day`.

## Algorithm

1. Parse every CSV line using the exact dialect (quoted complete fields, no CSV `\\r` newline shortcut).
2. Compute transitive user identity and byte-wise smallest representative by **union-find**.
3. **Deduplicate events before validating** the winning, earliest-ingested copies.
4. Validate event timestamps, prices, types, product IDs, and 24-hour lateness.
5. Sort events by (person, timestamp), open sessions only on a gap **greater than** 30 minutes, and aggregate by UTC session-start date.
6. Deduplicate and validate clicks; sort by (person, click timestamp, reverse byte-wise click ID), then attribute each session to its latest qualifying click.
7. Merge seven-day intervals per (person, campaign), using binary searches into sorted session starts to count unique assisted sessions without a huge range join.
8. Emit aggregated CSVs and convert them into two correctly typed Parquet tables using DuckDB, deleting the temporary CSVs.

The implementation does not depend on the fixture names, seed, ID ranges, campaign cardinalities, or hardcoded window boundaries. Logs are written to stderr.
