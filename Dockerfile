FROM debian:bookworm-slim AS builder
RUN apt-get update && apt-get install -y --no-install-recommends g++ && rm -rf /var/lib/apt/lists/*
COPY pipeline/engine.cpp /app/engine.cpp
COPY pipeline/native_parquet.hpp /app/native_parquet.hpp
COPY pipeline/csv_reader.hpp /app/csv_reader.hpp
RUN g++ -std=c++20 -O3 -march=x86-64-v3 -fopenmp -flto -fno-semantic-interposition -fno-plt -DNDEBUG -I/app -o /app/engine /app/engine.cpp

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends libgomp1 libstdc++6 && rm -rf /var/lib/apt/lists/*
RUN useradd --uid 1000 --no-create-home --home-dir /scratch app
COPY --from=builder /app/engine /app/engine
RUN chmod 755 /app/engine
ENV OMP_PROC_BIND=spread OMP_PLACES=threads OMP_DYNAMIC=false
WORKDIR /app
CMD ["/app/engine"]
