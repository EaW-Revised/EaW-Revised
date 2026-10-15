FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends cmake ninja-build g++ libssl-dev ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /source
COPY include/eawr/core include/eawr/core
COPY include/eawr/net include/eawr/net
# transport.hpp (via gns_room.hpp) takes max_match_seats from this one constants header.
COPY include/eawr/sim/seat.hpp include/eawr/sim/seat.hpp
COPY include/eawr/platform/relay_credentials.hpp include/eawr/platform/relay_credentials.hpp
COPY include/eawr/presentation/ui/lan_lobby.hpp include/eawr/presentation/ui/lan_lobby.hpp
COPY src/platform/relay_credentials.cpp src/platform/relay_credentials.cpp
COPY src/net src/net
COPY cmake cmake
COPY apps/relay_server apps/relay_server
RUN cmake -S apps/relay_server -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /build --parallel 2

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends libssl3 libstdc++6 ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 10001 --no-create-home relay
COPY --from=build /build/eawr_relay_server /usr/local/bin/eawr_relay_server
USER 10001:10001
WORKDIR /config
EXPOSE 27020/tcp
HEALTHCHECK --interval=30s --timeout=12s --start-period=10s --retries=3 \
    CMD ["eawr_relay_server", "--health", "--config", "/config/relay.local.conf"]
ENTRYPOINT ["eawr_relay_server"]
CMD ["--config", "/config/relay.local.conf"]
