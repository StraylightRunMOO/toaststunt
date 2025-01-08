FROM alpine:3.18 AS builder

LABEL maintainer="Damus <damus@straylightrun.org>"

# Install and configure toolchain
RUN apk add --no-cache argon2-dev aspell-dev binutils \
  build-base clang clang-dev cmake curl-dev make \
  bison gperf nettle-dev openssl-dev pcre-dev sqlite-dev \
  && clang --version

RUN ln -sf /usr/bin/clang   /usr/bin/cc
RUN ln -sf /usr/bin/clang++ /usr/bin/c++
RUN ln -sf /usr/bin/ld ld   /usr/bin/lld

RUN cc --version
RUN c++ --version

RUN mkdir -p /app/build

WORKDIR /app

# Copy source code
COPY src             /app/src/
COPY CMakeLists.txt  /app/
COPY CMakeModules    /app/CMakeModules

# Compile
RUN cd build && cmake -DCMAKE_BUILD_TYPE=Release .. && make -j4

# Start final layer
FROM alpine:3.18

# Install dependencies
RUN apk add argon2-dev aspell-dev aspell-en \
  curl-dev nettle-dev openssl-dev pcre-dev sqlite-dev

# Copy binaries
COPY --from=builder /app/build/moo ./usr/bin/moo

RUN mkdir -p /data/files
RUN mkdir -p /data/exec

COPY db /data/db

# Run it
ENTRYPOINT ["moo"]
CMD ["-o", "-i", "/data/files", "-x", "/data/exec", "/data/db/ghostcore.db", "/data/db/ghostcore.db", "7777"]
