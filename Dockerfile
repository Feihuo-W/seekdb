# MiniVectorDB submission image for 2026-competition-round2.
# The database is built from source in the build stage; the runtime image only
# carries the binary and the /data directory.

FROM ubuntu:24.04 AS build
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates g++ cmake make \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /build
COPY minivectordb/ /build/minivectordb/
RUN cmake -S /build/minivectordb -B /build/out -DCMAKE_BUILD_TYPE=Release \
 && cmake --build /build/out -j"$(nproc)"

FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN mkdir -p /data
COPY --from=build /build/out/minivectordb /usr/local/bin/minivectordb
VOLUME ["/data"]
ENTRYPOINT ["/usr/local/bin/minivectordb", "--data", "/data"]
