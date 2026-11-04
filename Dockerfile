# Illuminator core CLI image
ARG GIT_VERSION=dev
ARG GIT_COMMIT=unknown

FROM ubuntu:22.04 AS backend-builder

ARG GIT_VERSION
ARG GIT_COMMIT

ENV DEBIAN_FRONTEND=noninteractive
ENV BAZEL_VERSION=7.6.1

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates curl gnupg \
    build-essential g++-11 gcc-11 \
    clang \
    libbpf-dev libelf-dev zlib1g-dev libsqlite3-dev \
    python3 \
    git \
    && rm -rf /var/lib/apt/lists/*

# Use GCC 11 as default (full C++20 support)
RUN update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-11 100 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-11 100

# Install Bazelisk (respects .bazelversion)
RUN curl -fsSL "https://github.com/bazelbuild/bazelisk/releases/download/v1.25.0/bazelisk-linux-amd64" \
    -o /usr/local/bin/bazel && chmod +x /usr/local/bin/bazel

WORKDIR /app
COPY . .

# Override workspace_status.sh to use build args (no .git in Docker context)
RUN printf '#!/bin/bash\necho "STABLE_GIT_VERSION %s"\necho "STABLE_GIT_COMMIT %s"\necho "BUILD_TIMESTAMP %s"\n' \
    "${GIT_VERSION}" "${GIT_COMMIT}" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
    > tools/workspace_status.sh && chmod +x tools/workspace_status.sh

# Build the core runtime host
RUN bazel build //src/cli:illuminator --config=opt \
    && cp bazel-bin/src/cli/illuminator /app/illuminator-bin

# ---------------------------------------------------------------------------
# Stage 3: Runtime Image (minimal)
# ---------------------------------------------------------------------------
FROM ubuntu:22.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    libsqlite3-0 \
    libbpf0 \
    libstdc++6 \
    libelf1 \
    zlib1g \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && useradd -r -s /usr/sbin/nologin illuminator

WORKDIR /opt/illuminator

COPY --from=backend-builder /app/illuminator-bin ./illuminator
COPY illuminator.yaml.example ./illuminator.yaml

RUN mkdir -p /var/lib/illuminator \
    && chown -R illuminator:illuminator /opt/illuminator /var/lib/illuminator

USER illuminator

VOLUME ["/var/lib/illuminator"]

ENTRYPOINT ["./illuminator"]
CMD ["demo", "--config", "illuminator.yaml", "--output-dir", "/var/lib/illuminator"]
