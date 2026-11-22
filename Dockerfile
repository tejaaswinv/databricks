FROM debian:bookworm-slim AS builder
RUN apt-get update && apt-get install -y --no-install-recommends g++ && rm -rf /var/lib/apt/lists/*
COPY pipeline /app/pipeline
RUN g++ -std=c++20 -O3 -fopenmp -flto -fno-semantic-interposition -fno-plt -DNDEBUG -o /app/engine /app/pipeline/engine.cpp

FROM builder AS tests
COPY tests /app/tests
RUN g++ -std=c++20 -O1 -g -fopenmp -fsanitize=address,undefined -fno-omit-frame-pointer -o /app/engine-test /app/tests/engine_test.cpp
CMD ["/app/engine-test"]

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends libgomp1 libstdc++6 && rm -rf /var/lib/apt/lists/*
RUN useradd --uid 1000 --no-create-home --home-dir /scratch app
COPY --from=builder /app/engine /app/engine
RUN chmod 755 /app/engine
ENV OMP_PROC_BIND=true OMP_PLACES=cores
WORKDIR /app
CMD ["/app/engine"]
