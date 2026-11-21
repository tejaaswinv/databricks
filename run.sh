#!/bin/sh
set -eu
: "${DATA_DIR:=/data}" "${OUTPUT_DIR:=/output}" "${SCRATCH_DIR:=/scratch}"
export DATA_DIR OUTPUT_DIR SCRATCH_DIR
export TMPDIR="$SCRATCH_DIR" HOME="$SCRATCH_DIR"

mkdir -p "$OUTPUT_DIR/user_daily" "$OUTPUT_DIR/campaign_daily"
rm -f "$OUTPUT_DIR/user_daily.csv" "$OUTPUT_DIR/campaign_daily.csv"
mkfifo "$OUTPUT_DIR/user_daily.csv" "$OUTPUT_DIR/campaign_daily.csv"

threads="${NCPU:-8}"
mem="${MEM_GB:-24}"

# Start both Parquet consumers before the engine. They block on the FIFOs while
# native parsing/sessionization runs, hiding DuckDB startup and avoiding disk CSV I/O.
/app/duckdb -batch -c "
SET threads = $threads;
SET memory_limit = '$((mem * 4 / 10))GB';
SET temp_directory = '$SCRATCH_DIR/duckdb_user_tmp';
COPY (
  SELECT * FROM read_csv(
    '$OUTPUT_DIR/user_daily.csv',
    columns={'user_id':'VARCHAR','day':'DATE','sessions':'BIGINT','events':'BIGINT','revenue_cents':'BIGINT','top_category':'VARCHAR'},
    auto_detect=false, header=true, delim=',', new_line='\\n', parallel=false
  )
) TO '$OUTPUT_DIR/user_daily/part-0.parquet' (FORMAT PARQUET, COMPRESSION SNAPPY);
" >/dev/null &
pid_user=$!

/app/duckdb -batch -c "
SET threads = $threads;
SET memory_limit = '$((mem * 4 / 10))GB';
SET temp_directory = '$SCRATCH_DIR/duckdb_campaign_tmp';
COPY (
  SELECT * FROM read_csv(
    '$OUTPUT_DIR/campaign_daily.csv',
    columns={'campaign_id':'VARCHAR','channel':'VARCHAR','day':'DATE','sessions':'BIGINT','purchasing_sessions':'BIGINT','revenue_cents':'BIGINT','assisted_sessions':'BIGINT'},
    auto_detect=false, header=true, delim=',', new_line='\\n', parallel=false
  )
) TO '$OUTPUT_DIR/campaign_daily/part-0.parquet' (FORMAT PARQUET, COMPRESSION SNAPPY);
" >/dev/null &
pid_campaign=$!

status=0
/app/engine || status=$?
wait "$pid_user" || status=$?
wait "$pid_campaign" || status=$?
rm -f "$OUTPUT_DIR/user_daily.csv" "$OUTPUT_DIR/campaign_daily.csv"
exit "$status"
