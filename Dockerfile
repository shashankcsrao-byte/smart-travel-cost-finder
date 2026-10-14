# Smart Travel Cost Finder - the C backend in a container (used by Render).
#
#   docker build -t smart-travel .
#   docker run -p 8000:10000 -e OLA_MAPS_API_KEY=... smart-travel

# ---- 1. build the server with a C compiler and the three libraries ----
FROM debian:bookworm-slim AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends build-essential libcurl4-openssl-dev libsqlite3-dev zlib1g-dev \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY backend-c/ backend-c/
RUN make -C backend-c clean all

# ---- 2. a small image with only what the server needs to run ----
FROM debian:bookworm-slim
RUN apt-get update \
 && apt-get install -y --no-install-recommends libcurl4 libsqlite3-0 zlib1g ca-certificates \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --create-home app
WORKDIR /app
COPY --from=build /src/backend-c/build/smart-travel /app/smart-travel
COPY web/ web/
COPY data/bmrcl-gtfs/bmrcl.zip data/bmrcl-gtfs/bmrcl.zip
# HOST 0.0.0.0: reachable from outside the container. FORWARDED_ALLOW_IPS=*: trust the proxy's
# X-Forwarded-For so rate limits see each visitor's real IP. The history database goes to /tmp.
ENV HOST=0.0.0.0 PORT=10000 FORWARDED_ALLOW_IPS=* TRAVEL_DB_PATH=/tmp/travel_history.db
USER app
EXPOSE 10000
CMD ["/app/smart-travel"]
