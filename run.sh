#!/bin/sh
set -eu
: "${DATA_DIR:=/data}" "${OUTPUT_DIR:=/output}" "${SCRATCH_DIR:=/scratch}"
export DATA_DIR OUTPUT_DIR SCRATCH_DIR
export TMPDIR="$SCRATCH_DIR" HOME="$SCRATCH_DIR"

# The C++ engine writes the required Parquet datasets directly:
#   $OUTPUT_DIR/user_daily/*.parquet
#   $OUTPUT_DIR/campaign_daily/*.parquet
# No intermediate CSV or DuckDB conversion.
exec /app/engine
