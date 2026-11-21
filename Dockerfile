FROM debian:bookworm-slim AS builder
RUN apt-get update && apt-get install -y --no-install-recommends g++ curl unzip ca-certificates && rm -rf /var/lib/apt/lists/*
COPY pipeline/engine.cpp /app/engine.cpp
RUN g++ -std=c++20 -O3 -march=x86-64-v3 -mtune=generic -fopenmp -flto -fno-semantic-interposition -fno-plt -DNDEBUG -o /app/engine /app/engine.cpp
RUN curl -fsSL https://github.com/duckdb/duckdb/releases/download/v1.5.6/duckdb_cli-linux-amd64.zip -o /tmp/duckdb.zip \
 && echo "6e89deac1ebbc36eed0291caf8b567b030c7b86ac35998f71854e22b3c5d5e2f  /tmp/duckdb.zip" | sha256sum -c - \
 && unzip -q /tmp/duckdb.zip -d /app \
 && chmod 755 /app/duckdb

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends libgomp1 libstdc++6 && rm -rf /var/lib/apt/lists/*
RUN useradd --uid 1000 --no-create-home --home-dir /scratch app
COPY --from=builder /app/engine /app/engine
COPY --from=builder /app/duckdb /app/duckdb
COPY run.sh /app/run.sh
RUN chmod 755 /app/engine /app/duckdb /app/run.sh
ENV OMP_PROC_BIND=true OMP_PLACES=cores
WORKDIR /app
CMD ["/app/run.sh"]
