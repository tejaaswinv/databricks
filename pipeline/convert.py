#!/usr/bin/env python3
"""Convert compact C++ aggregate rows into exactly typed Parquet outputs."""
import os
from pathlib import Path
import duckdb

out = Path(os.environ.get("OUTPUT_DIR", "/output"))
scratch = os.environ.get("SCRATCH_DIR", "/scratch")
con = duckdb.connect()
con.execute(f"SET threads = {int(os.getenv('NCPU','8'))}")
con.execute(f"SET memory_limit = '{int(os.getenv('MEM_GB','24'))*8//10}GB'")
con.execute(f"SET temp_directory = '{scratch}/duckdb_tmp'")

for tab, cols in {
    "user_daily": "user_id::VARCHAR AS user_id, CAST(day AS DATE) AS day, CAST(sessions AS BIGINT) AS sessions, CAST(events AS BIGINT) AS events, CAST(revenue_cents AS BIGINT) AS revenue_cents, top_category::VARCHAR AS top_category",
    "campaign_daily": "campaign_id::VARCHAR AS campaign_id, channel::VARCHAR AS channel, CAST(day AS DATE) AS day, CAST(sessions AS BIGINT) AS sessions, CAST(purchasing_sessions AS BIGINT) AS purchasing_sessions, CAST(revenue_cents AS BIGINT) AS revenue_cents, CAST(assisted_sessions AS BIGINT) AS assisted_sessions"
}.items():
    (out/tab).mkdir(parents=True, exist_ok=True)
    path = str(out/f"{tab}.csv").replace("'", "''")
    target = str(out/tab/"part-0.parquet").replace("'", "''")
    names = ("user_id,day,sessions,events,revenue_cents,top_category" if tab == "user_daily"
             else "campaign_id,channel,day,sessions,purchasing_sessions,revenue_cents,assisted_sessions")
    schema = "{" + ",".join(f"'{x}':'VARCHAR'" for x in names.split(",")) + "}"
    con.execute(f"COPY (SELECT {cols} FROM read_csv('{path}', columns={schema}, "
                f"auto_detect=false, header=true, delim=',', "
                f"new_line='\\n')) TO '{target}' (FORMAT PARQUET, COMPRESSION ZSTD)")
    (out/f"{tab}.csv").unlink()
