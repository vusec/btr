# Build environment for the Firefox 143 JS shell used by the BTR artifact.
#
# The image carries its own clang and a pinned rustc so the build never runs
# `mach bootstrap`:
#   * `mach bootstrap` fetches prebuilt toolchains keyed on the current
#     mozilla-central revision; the DEVEDITION_143_0b9_BUILD1 tag has no such
#     artifact, so the fetch always fails on this tree.
#   * rustc >= 1.98 added OpenEmbedded `oe`-vendor Linux targets (rust-lang/rust
#     #157650) that collide with the `unknown`-vendor targets in FF143's
#     detect_rustc_target(), making configure die with "Don't know how to
#     translate x86_64-pc-linux-gnu for rustc". The pinned RUST_VERSION stays
#     below 1.98.
# clang 19 (from apt.llvm.org) matches the 19.1.7 the tree expects. The BTR
# page-reuse behavior depends on an exec-mem pool randomization source mod, not
# on the compiler, so a self-contained distro clang is used here.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# Base build + js-shell dependencies. build-essential supplies binutils and the
# libstdc++ headers; nodejs and cbindgen (below) are the generated-file steps
# mach bootstrap would otherwise install.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        m4 \
        nasm \
        pkg-config \
        python3 \
        python3-dev \
        python3-pip \
        python3-venv \
        nodejs \
        zlib1g-dev \
        libreadline-dev \
        ccache \
        gdb \
        git \
        curl \
        gnupg \
        ca-certificates \
        file \
        unzip \
    && rm -rf /var/lib/apt/lists/*

# clang 19 from the official LLVM apt repo. libclang-19-dev drives rust bindgen;
# the unversioned clang/clang++/ld.lld names let the mozconfigs stay generic.
RUN curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key \
        -o /etc/apt/trusted.gpg.d/apt.llvm.org.asc \
    && echo "deb http://apt.llvm.org/noble/ llvm-toolchain-noble-19 main" \
        > /etc/apt/sources.list.d/llvm19.list \
    && apt-get update && apt-get install -y --no-install-recommends \
        clang-19 \
        libclang-19-dev \
        lld-19 \
        llvm-19-dev \
    && rm -rf /var/lib/apt/lists/* \
    && ln -sf clang-19    /usr/bin/clang \
    && ln -sf clang++-19  /usr/bin/clang++ \
    && ln -sf lld-19      /usr/bin/lld \
    && ln -sf ld.lld-19   /usr/bin/ld.lld
ENV LIBCLANG_PATH=/usr/lib/llvm-19/lib

# aarch64 cross toolchain and sysroot (glibc + libstdc++ headers and libs) so
# clang --target=aarch64-linux-gnu builds the arm JS shell on an x86_64 host.
# clang derives --sysroot=/usr/aarch64-linux-gnu from the installed
# aarch64-linux-gnu-gcc, and lld links aarch64, so no separate binutils are
# needed; the rust aarch64 std is added with the rust install below.
RUN apt-get update && apt-get install -y --no-install-recommends \
        g++-aarch64-linux-gnu \
    && rm -rf /var/lib/apt/lists/*

# Pinned rust, installed system-wide. Kept < 1.98 to avoid the oe-vendor target
# regression; 1.89 matches the tuned build. Override with
# --build-arg RUST_VERSION=<x.y.z> at image build time.
ARG RUST_VERSION=1.89.0
ENV RUSTUP_HOME=/opt/rust/rustup \
    CARGO_HOME=/opt/rust/cargo \
    PATH=/opt/rust/cargo/bin:/usr/bin:/usr/local/bin:/bin:/usr/sbin:/sbin
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs \
        | sh -s -- -y --no-modify-path --profile minimal \
          --default-toolchain "${RUST_VERSION}" \
    && rustup component add rust-src rustfmt clippy \
    && rustup target add aarch64-unknown-linux-gnu \
    && cargo install cbindgen --locked \
    && chmod -R a+rwX /opt/rust

# clang is the compiler; the mozconfigs set CC/CXX to these.
ENV CC=clang \
    CXX=clang++

# Match the host UID so build products stay owned by the invoking user.
ARG UID=1000
ARG GID=1000
RUN userdel -r ubuntu 2>/dev/null || true; \
    groupadd -g ${GID} dev 2>/dev/null || true; \
    useradd -u ${UID} -g ${GID} -m -s /bin/bash dev

USER dev
