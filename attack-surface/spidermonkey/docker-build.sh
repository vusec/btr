#!/usr/bin/env bash
# Build the Firefox 143 JS shell inside a container that carries its own
# clang and a pinned rustc, so the build never runs `mach bootstrap` and never
# hits the rustc >= 1.98 target-detection regression.
#
#   ./docker-build.sh image             build the container image
#   ./docker-build.sh build [args...]   run ./mach build
#   ./docker-build.sh shell             interactive shell, starts in the source tree
#   ./docker-build.sh versions          report the container toolchain versions
#
# Knobs (environment):
#   FIREFOX_SRC   source tree           (default: ./firefox)
#   ARCH          x86 or arm            (default x86; picks docker/mozconfig/jsshell-$ARCH)
#                 arm cross-compiles aarch64 from the host arch with the image's
#                 clang --target=aarch64-linux-gnu, so an x86_64 host builds the
#                 arm shell without a native arm machine
#   OBJDIR        build output (objdir)  (default docker/build/obj-$ARCH; kept
#                 out of the source tree, one per arch so they sit side by side)
#   MOZBUILD      mach state + HOME dir  (default <FIREFOX_SRC>/.mozbuild-docker)
#   JOBS          parallel compile jobs  (default: let mach decide)
#   MOUNTS        extra host paths to mount at the same path, space separated
#   IMAGE         image tag             (default spidermonkey:ff143)
#
# Every mount keeps its host path inside the container, so the objdir's absolute
# paths in build artifacts and debug info read the same on both sides.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CTX="$HERE/docker"                       # image definition, mozconfigs, build output
FIREFOX_SRC="${FIREFOX_SRC:-$PWD/firefox}"
ARCH="${ARCH:-x86}"
OBJDIR="${OBJDIR:-$CTX/build/obj-$ARCH}"
MOZBUILD="${MOZBUILD:-$FIREFOX_SRC/.mozbuild-docker}"
JOBS="${JOBS:-}"
MOUNTS="${MOUNTS:-}"
IMAGE="${IMAGE:-spidermonkey:ff143}"

case "$ARCH" in
    x86|arm) ;;
    *) echo "ARCH must be x86 or arm, got: $ARCH" >&2; exit 2 ;;
esac
MOZCONFIG_FILE="$CTX/mozconfig/jsshell-$ARCH"

# Commands that mount and build the tree need a real checkout.
need_tree() {
    [ -f "$FIREFOX_SRC/mach" ] || {
        echo "FIREFOX_SRC=$FIREFOX_SRC is not a Firefox checkout (no ./mach)." >&2
        echo "Put the tree at ./firefox, or set FIREFOX_SRC=/path/to/firefox." >&2
        exit 2
    }
}

docker_run() {
    local tty_flags=() mount_flags=() path
    [ -t 0 ] && [ -t 1 ] && tty_flags=(-it)

    mkdir -p "$OBJDIR" "$MOZBUILD"
    for path in "$CTX" "$FIREFOX_SRC" "$OBJDIR" "$MOZBUILD" $MOUNTS; do
        mount_flags+=(-v "$path:$path")
    done

    docker run --rm "${tty_flags[@]}" \
        "${mount_flags[@]}" \
        -e "MOZCONFIG=$MOZCONFIG_FILE" \
        -e "MOZ_OBJDIR=$OBJDIR" \
        -e "MOZBUILD_STATE_PATH=$MOZBUILD" \
        -e "HOME=$MOZBUILD" \
        -w "$FIREFOX_SRC" \
        --cap-add=SYS_PTRACE \
        --security-opt seccomp=unconfined \
        "$IMAGE" "$@"
}

cmd="${1:-}"; shift || true
case "$cmd" in
image)
    docker build \
        --build-arg "UID=$(id -u)" \
        --build-arg "GID=$(id -g)" \
        -t "$IMAGE" "$CTX"
    ;;
build)
    need_tree
    jobs_flags=()
    [ -n "$JOBS" ] && jobs_flags=("-j${JOBS}")
    docker_run ./mach build "${jobs_flags[@]}" "$@"
    ;;
shell)
    need_tree
    docker_run bash "$@"
    ;;
versions)
    docker_run bash -lc 'clang --version | head -1; rustc --version; cargo --version; cbindgen --version; python3 --version; node --version | sed "s/^/node /"; gdb --version | head -1; rustup target list --installed | sed "s/^/rust target /"; aarch64-linux-gnu-gcc --version | head -1'
    ;;
*)
    sed -n '2,25p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 2
    ;;
esac
