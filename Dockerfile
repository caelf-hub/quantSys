FROM ubuntu:22.04 AS build

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends build-essential cmake ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY cmake ./cmake
COPY include ./include
COPY src ./src

# Same absolute path in the runtime stage so ctest's recorded binary paths work.
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j"$(nproc)"

FROM ubuntu:22.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends cmake libstdc++6 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --create-home --uid 10001 quantsys

COPY --from=build --chown=quantsys:quantsys /src/build /src/build

USER quantsys
WORKDIR /src

HEALTHCHECK --interval=30s --timeout=5s --start-period=5s --retries=1 \
    CMD ["/src/build/test_order_pool"]

CMD ["ctest", "--test-dir", "/src/build", "--output-on-failure"]
