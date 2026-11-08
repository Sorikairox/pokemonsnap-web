# pokemonsnap-tools: the upstream matching build (splat + IDO recomp + asm-processor + MIPS binutils + pigment64)
# for Apple silicon (linux/arm64). Built by port/docker/run.sh image; the context is a staging copy of
# pyproject.toml / uv.lock / .python-version (port_work/docker-ctx), never the repo (no ROM in the context).
#
# configure.py --setup downloads the x86_64 "linux" IDO v1.1 and picks the x86_64 asm-processor on linux/aarch64,
# neither of which runs here, so the same tools are installed in their arm builds under /opt and run.sh copies them
# into tools/ido5.3, tools/ido7.1 and tools/asm_proc (gitignored upstream) before configuring. The checksum of the
# rebuilt ROM is the proof that they are equivalent.

FROM ubuntu:24.04 AS pigment
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends curl ca-certificates build-essential && \
    rm -rf /var/lib/apt/lists/*
# pigment64 0.6.3 needs Rust >= 1.85 (Ubuntu 24.04 ships 1.75) and has no linux-arm64 release binary
RUN curl -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal --default-toolchain 1.85.0
RUN /root/.cargo/bin/cargo install pigment64 --version 0.6.3 --locked --root /opt/pigment64

FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      binutils-mips-linux-gnu cpp gcc libc6-dev ninja-build git curl ca-certificates xz-utils python3 && \
    rm -rf /var/lib/apt/lists/*
COPY --from=pigment /opt/pigment64/bin/pigment64 /usr/local/bin/pigment64

# IDO 5.3 / 7.1 static recomp (decompals/ido-static-recomp v1.2, linux-arm) and asm-processor 1.0.1 (aarch64 linux)
RUN mkdir -p /opt/ido5.3 /opt/ido7.1 /opt/asm_proc && \
    curl -sSfL https://github.com/decompals/ido-static-recomp/releases/download/v1.2/ido-5.3-recomp-linux-arm.tar.gz | tar xz -C /opt/ido5.3 && \
    curl -sSfL https://github.com/decompals/ido-static-recomp/releases/download/v1.2/ido-7.1-recomp-linux-arm.tar.gz | tar xz -C /opt/ido7.1 && \
    curl -sSfL https://github.com/simonlindholm/asm-processor/releases/download/1.0.1/asm-processor-aarch64-unknown-linux-gnu.tar.xz | tar xJ -C /opt/asm_proc --strip-components=1

# uv + the project's locked Python deps in /opt/venv (outside the repo mount)
COPY --from=ghcr.io/astral-sh/uv:0.9.5 /uv /uvx /usr/local/bin/
ENV UV_PROJECT_ENVIRONMENT=/opt/venv UV_PYTHON_INSTALL_DIR=/opt/python UV_LINK_MODE=copy UV_FROZEN=1
WORKDIR /deps
COPY pyproject.toml uv.lock .python-version ./
RUN uv sync --frozen && rm -rf /root/.cache/uv
ENV PATH=/opt/venv/bin:$PATH
WORKDIR /src
