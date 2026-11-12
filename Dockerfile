# MiniVectorDB submission image for 2026-competition-round2.
# The database is built from source in the build stage; the runtime image only
# carries the binary and the /data directory.

# v19: binary is fully statically linked (target_link_options -static in CMakeLists).
# Build stage needs libc6-dev for libc.a; runtime image needs no libstdc++/libm/libgcc_s.

FROM ubuntu:24.04 AS build
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates g++ libc6-dev cmake make \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /build
COPY minivectordb/CMakeLists.txt /build/minivectordb/CMakeLists.txt
COPY minivectordb/src/ /build/minivectordb/src/
RUN cmake -S /build/minivectordb -B /build/out -DCMAKE_BUILD_TYPE=Release \
 && cmake --build /build/out -j"$(nproc)"

FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN mkdir -p /data
COPY --from=build /build/out/minivectordb /usr/local/bin/minivectordb
VOLUME ["/data"]
STOPSIGNAL SIGTERM
ENTRYPOINT ["/usr/local/bin/minivectordb", "--data", "/data"]
