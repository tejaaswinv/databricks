FROM python:3.12-slim-bookworm AS builder
RUN apt-get update && apt-get install -y --no-install-recommends g++ && rm -rf /var/lib/apt/lists/*
COPY pipeline/engine.cpp /app/engine.cpp
RUN g++ -std=c++20 -O3 -march=x86-64 -fopenmp -DNDEBUG -o /app/engine /app/engine.cpp
FROM python:3.12-slim-bookworm
RUN apt-get update && apt-get install -y --no-install-recommends libgomp1 libstdc++6 && rm -rf /var/lib/apt/lists/*
RUN useradd --uid 1000 --no-create-home --home-dir /scratch app
RUN pip install --no-cache-dir duckdb==1.5.6
COPY --from=builder /app/engine /app/engine
COPY pipeline/convert.py /app/convert.py
COPY run.sh /app/run.sh
RUN chmod 755 /app/engine /app/run.sh
ENV PYTHONDONTWRITEBYTECODE=1 PYTHONUNBUFFERED=1
WORKDIR /app
CMD ["/app/run.sh"]
