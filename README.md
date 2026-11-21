# Shopping Sessions — native C++ engine + DuckDB Parquet output

Implements shopping-sessions-4 CSV cleaning, transitive linked IDs, 30-minute sessions, seven-day last-click attribution and campaign assists using a C++20 native engine. The final aggregated tables are converted to correctly typed Parquet using DuckDB.

Build the root Dockerfile for linux/amd64, and run with DATA_DIR, OUTPUT_DIR, and SCRATCH_DIR mounted. Logs go to stderr; intermediate aggregate CSVs are deleted after Parquet conversion.

The engine uses union-find for identities, deduplicates events **before validation**, sorts valid events by person and exact UTC millisecond, applies a linear last-click scan, and counts distinct campaign assists using merged intervals and binary searches. It does not assume any seed, fixed campaign name, product range or event window.

Local engine correctness: all 36 supplied edge-case fixtures match their expected **CSV** results. Official dev-set and Docker Parquet validation remain to be run.
