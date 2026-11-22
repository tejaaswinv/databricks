FROM debian:bookworm-slim AS builder
RUN apt-get update && apt-get install -y --no-install-recommends g++ libsnappy-dev && rm -rf /var/lib/apt/lists/*
COPY pipeline /app/pipeline
RUN g++ -std=c++20 -O3 -fopenmp -flto -fno-semantic-interposition -DNDEBUG -DNATIVEPQ_SNAPPY \
    -static -o /app/engine /app/pipeline/engine.cpp -lsnappy -pthread

FROM builder AS tests
COPY tests /app/tests
RUN g++ -std=c++20 -O1 -g -fopenmp -fsanitize=address,undefined -fno-omit-frame-pointer -DNATIVEPQ_SNAPPY \
    -o /app/engine-test /app/tests/engine_test.cpp -Wl,-Bstatic -lsnappy -Wl,-Bdynamic
CMD ["/app/engine-test"]

FROM scratch
COPY --from=builder /app/engine /app/engine
USER 1000:1000
ENV OMP_PROC_BIND=true OMP_PLACES=cores
WORKDIR /app
ENTRYPOINT ["/app/engine"]
