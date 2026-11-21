FROM python:3.12-slim-bookworm AS builder
RUN apt-get update && apt-get install -y --no-install-recommends g++ curl unzip ca-certificates && rm -rf /var/lib/apt/lists/*
COPY pipeline/engine.cpp /app/engine.cpp
RUN curl -fsSL https://github.com/duckdb/duckdb/releases/download/v1.5.6/libduckdb-linux-amd64.zip -o /tmp/duckdb.zip \
 && echo "b845005f5132a7d8180057c35e14a7626632258782f871a90861b19c1c03841b  /tmp/duckdb.zip" | sha256sum -c - \
 && unzip -q /tmp/duckdb.zip -d /app/duckdb-lib
RUN g++ -std=c++20 -O3 -march=x86-64 -fopenmp -flto -fno-semantic-interposition -fno-plt -DNDEBUG -I/app/duckdb-lib -L/app/duckdb-lib -Wl,-rpath,/app -o /app/engine /app/engine.cpp -lduckdb

FROM python:3.12-slim-bookworm
RUN apt-get update && apt-get install -y --no-install-recommends libgomp1 libstdc++6 && rm -rf /var/lib/apt/lists/*
RUN useradd --uid 1000 --no-create-home --home-dir /scratch app
COPY --from=builder /app/engine /app/engine
COPY --from=builder /app/duckdb-lib/libduckdb.so /app/libduckdb.so
COPY run.sh /app/run.sh
RUN chmod 755 /app/engine /app/run.sh /app/libduckdb.so
WORKDIR /app
CMD ["/app/run.sh"]
