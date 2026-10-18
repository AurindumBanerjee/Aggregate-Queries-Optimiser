# syntax=docker/dockerfile:1
FROM ubuntu:24.04 AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
      g++ cmake ninja-build libgtest-dev libsqlite3-dev ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY include ./include
COPY src ./src
COPY tests ./tests
COPY tools ./tools
COPY bench ./bench
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DAQO_BUILD_BENCH=ON \
 && cmake --build build

# `docker build --target test .` runs the full test suite.
FROM build AS test
RUN ctest --test-dir build --output-on-failure -j4

FROM ubuntu:24.04 AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends libsqlite3-0 && rm -rf /var/lib/apt/lists/* \
 && useradd -m aqo && mkdir /data && chown aqo /data
COPY --from=build /src/build/aqo_cli /src/build/aqo_bench /usr/local/bin/
USER aqo
ENV AQO_DB_PATH=/data/aqo.db
VOLUME /data
ENTRYPOINT ["aqo_cli"]
