#!/bin/sh
set -eu
: "${DATA_DIR:=/data}" "${OUTPUT_DIR:=/output}" "${SCRATCH_DIR:=/scratch}"
export DATA_DIR OUTPUT_DIR SCRATCH_DIR
export TMPDIR="$SCRATCH_DIR" HOME="$SCRATCH_DIR"

/app/engine

mkdir -p "$OUTPUT_DIR/user_daily" "$OUTPUT_DIR/campaign_daily"
threads="2"
mem="${MEM_GB:-24}"
/app/duckdb -batch -c "
SET threads = $threads;
SET memory_limit = '$((mem * 8 / 10))GB';
SET temp_directory = '$SCRATCH_DIR/duckdb_tmp';
COPY (
  SELECT * FROM read_csv(
    '$OUTPUT_DIR/user_daily.csv',
    columns={'user_id':'VARCHAR','day':'DATE','sessions':'BIGINT','events':'BIGINT','revenue_cents':'BIGINT','top_category':'VARCHAR'},
    auto_detect=false, header=true, delim=',', new_line='\\n'
  )
) TO '$OUTPUT_DIR/user_daily/part-0.parquet' (FORMAT PARQUET, COMPRESSION SNAPPY);
COPY (
  SELECT * FROM read_csv(
    '$OUTPUT_DIR/campaign_daily.csv',
    columns={'campaign_id':'VARCHAR','channel':'VARCHAR','day':'DATE','sessions':'BIGINT','purchasing_sessions':'BIGINT','revenue_cents':'BIGINT','assisted_sessions':'BIGINT'},
    auto_detect=false, header=true, delim=',', new_line='\\n'
  )
) TO '$OUTPUT_DIR/campaign_daily/part-0.parquet' (FORMAT PARQUET, COMPRESSION SNAPPY);
" >/dev/null
rm -f "$OUTPUT_DIR/user_daily.csv" "$OUTPUT_DIR/campaign_daily.csv"
