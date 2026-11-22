FROM debian:bookworm-slim AS builder
RUN apt-get update && apt-get install -y --no-install-recommends g++ libsnappy-dev && rm -rf /var/lib/apt/lists/*
COPY pipeline /app/pipeline
RUN g++ -std=c++20 -O3 -march=x86-64 -fopenmp -flto -fno-semantic-interposition -fno-plt -DNDEBUG -DNATIVEPQ_SNAPPY -o /app/engine /app/pipeline/engine.cpp -Wl,-Bstatic -lsnappy -Wl,-Bdynamic \
 && g++ -std=c++20 -O3 -march=x86-64-v3 -fopenmp -flto -fno-semantic-interposition -fno-plt -DNDEBUG -DNATIVEPQ_SNAPPY -o /app/engine-v3 /app/pipeline/engine.cpp -Wl,-Bstatic -lsnappy -Wl,-Bdynamic \
 && g++ -std=c++20 -O3 -march=x86-64-v4 -fopenmp -flto -fno-semantic-interposition -fno-plt -DNDEBUG -DNATIVEPQ_SNAPPY -o /app/engine-v4 /app/pipeline/engine.cpp -Wl,-Bstatic -lsnappy -Wl,-Bdynamic \
 && g++ -O2 -march=x86-64 -o /app/dispatcher /app/pipeline/dispatcher.cpp

FROM builder AS tests
COPY tests /app/tests
RUN g++ -std=c++20 -O1 -g -fopenmp -fsanitize=address,undefined -fno-omit-frame-pointer -DNATIVEPQ_SNAPPY -o /app/engine-test /app/tests/engine_test.cpp -Wl,-Bstatic -lsnappy -Wl,-Bdynamic
CMD ["/app/engine-test"]

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends libgomp1 libstdc++6 && rm -rf /var/lib/apt/lists/*
RUN useradd --uid 1000 --no-create-home --home-dir /scratch app
COPY --from=builder /app/engine /app/engine
COPY --from=builder /app/engine-v3 /app/engine-v3
COPY --from=builder /app/engine-v4 /app/engine-v4
COPY --from=builder /app/dispatcher /app/dispatcher
RUN chmod 755 /app/engine /app/engine-v3 /app/engine-v4 /app/dispatcher
ENV OMP_PROC_BIND=true OMP_PLACES=cores
WORKDIR /app
CMD ["/app/dispatcher"]
