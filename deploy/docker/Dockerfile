FROM ubuntu:24.04 AS build

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates cmake g++ make pkg-config \
    libgrpc++-dev libgrpc-dev libprotobuf-dev protobuf-compiler protobuf-compiler-grpc \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .
RUN cmake -S . -B /build -DCMAKE_BUILD_TYPE=Release -DVDB_BUILD_TESTS=OFF -DVDB_BUILD_TOOLS=ON -DVDB_BUILD_GRPC=ON \
    && cmake --build /build --target vectordb-shardd vectordb-coordinatord -j"$(nproc)"

FROM build AS runtime
RUN useradd --system --create-home --home-dir /var/lib/vectordb vectordb
WORKDIR /var/lib/vectordb
USER vectordb
COPY --from=build /build/vectordb-shardd /usr/local/bin/vectordb-shardd
COPY --from=build /build/vectordb-coordinatord /usr/local/bin/vectordb-coordinatord

