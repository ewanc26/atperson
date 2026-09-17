# Build stage
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    ninja-build \
    git \
    libcurl4-openssl-dev \
    libssl-dev \
    ca-certificates \
    pkg-config \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# Configure and build atperson with network support enabled
RUN cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DATPERSON_BUILD_NETWORK=ON \
    -DATPERSON_BUILD_TESTS=OFF && \
    cmake --build build -j$(nproc) && \
    mkdir -p /usr/local/lib && \
    find /src/build -name "*.so*" -exec cp -d {} /usr/local/lib/ \;

# Runtime stage
FROM ubuntu:24.04 AS runner

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    libcurl4 \
    libssl3 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

RUN (userdel -r ubuntu 2>/dev/null || true) && \
    useradd -m -u 1000 -s /bin/bash atperson && \
    mkdir -p /var/lib/atperson && \
    chown -R atperson:atperson /var/lib/atperson

COPY --from=builder /usr/local/lib/ /usr/local/lib/
COPY --from=builder /src/build/atperson /usr/local/bin/atperson
RUN ldconfig

USER atperson
WORKDIR /var/lib/atperson

ENV ATPERSON_HOME=/var/lib/atperson

VOLUME ["/var/lib/atperson"]

ENTRYPOINT ["/usr/local/bin/atperson"]
CMD ["stats"]
