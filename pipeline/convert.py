#!/usr/bin/env python3
"""Write the exact two final Parquet tables using DuckDB's typed CSV scanner."""
import os
import time
from pathlib import Path
import duckdb

started = time.perf_counter()
out = Path(os.environ.get("OUTPUT_DIR", "/output"))
scratch = Path(os.environ.get("SCRATCH_DIR", "/scratch"))
con = duckdb.connect()
con.execute(f"SET threads = {int(os.getenv('NCPU', '8'))}")
con.execute(f"SET memory_limit = '{int(os.getenv('MEM_GB', '24')) * 8 // 10}GB'")
con.execute(f"SET temp_directory = '{str(scratch / 'duckdb_tmp').replace(chr(39), chr(39)*2)}'")

schemas = {
    "user_daily": [
        ("user_id", "VARCHAR"),
        ("day", "DATE"),
        ("sessions", "BIGINT"),
        ("events", "BIGINT"),
        ("revenue_cents", "BIGINT"),
        ("top_category", "VARCHAR"),
    ],
    "campaign_daily": [
        ("campaign_id", "VARCHAR"),
        ("channel", "VARCHAR"),
        ("day", "DATE"),
        ("sessions", "BIGINT"),
        ("purchasing_sessions", "BIGINT"),
        ("revenue_cents", "BIGINT"),
        ("assisted_sessions", "BIGINT"),
    ],
}

for table, columns in schemas.items():
    directory = out / table
    directory.mkdir(parents=True, exist_ok=True)
    source = out / f"{table}.csv"
    destination = directory / "part-0.parquet"
    schema = "{" + ", ".join(f"'{name}':'{dtype}'" for name, dtype in columns) + "}"
    src = str(source).replace("'", "''")
    dst = str(destination).replace("'", "''")
    # Typed CSV decoding avoids a second VARCHAR -> DATE/BIGINT conversion.
    # Snappy is a fast, valid Parquet compression codec.
    con.execute(
        f"COPY (SELECT * FROM read_csv('{src}', columns={schema}, "
        f"auto_detect=false, header=true, delim=',', new_line='\\n')) "
        f"TO '{dst}' (FORMAT PARQUET, COMPRESSION SNAPPY)"
    )
    source.unlink()

import sys
print(f"parquet conversion time={time.perf_counter()-started:.6f}s", file=sys.stderr)
