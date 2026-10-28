#!/usr/bin/env bash
# One-shot build of the Firefox 143 JS shell used by the SpiderMonkey attack
# surface: container image, source tree (clone + release tag + BTR patch), then
# the shell itself. Writes an env file at the end that sets JS_CMD.
#
#   ./setup.sh              x86_64 shell, tree at ./firefox
#   ./setup.sh arm          aarch64 shell, cross-compiled from the host arch
#   ARCH=arm ./setup.sh     same
#
# Knobs (environment):
#   FIREFOX_SRC   source tree          (default ./firefox; cloned when absent)
#   FIREFOX_REPO  clone URL            (default mozilla-firefox/firefox)
#   FIREFOX_TAG   release tag          (default DEVEDITION_143_0b9_BUILD1)
#   ARCH          x86 or arm           (default x86)
#   JOBS          parallel compile jobs(default: let mach decide)
#   IMAGE         image tag            (default spidermonkey:ff143)
#   REBUILD_IMAGE 1 to rebuild the image even when it already exists
#
# The build itself is docker-build.sh; this script only prepares its inputs and
# then hands over.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ARCH="${1:-${ARCH:-x86}}"
FIREFOX_SRC="${FIREFOX_SRC:-$PWD/firefox}"
FIREFOX_REPO="${FIREFOX_REPO:-https://github.com/mozilla-firefox/firefox.git}"
FIREFOX_TAG="${FIREFOX_TAG:-DEVEDITION_143_0b9_BUILD1}"
JOBS="${JOBS:-}"
IMAGE="${IMAGE:-spidermonkey:ff143}"
OBJDIR="${OBJDIR:-}"
PATCH="$HERE/jsshell/firefox143.patch"

export ARCH FIREFOX_SRC JOBS IMAGE OBJDIR

# docker -v needs absolute paths; the launcher mounts these as-is.
case "$FIREFOX_SRC" in /*) ;; *) FIREFOX_SRC="$PWD/$FIREFOX_SRC" ;; esac
case "$OBJDIR" in "" | /*) ;; *) OBJDIR="$PWD/$OBJDIR" ;; esac
export FIREFOX_SRC OBJDIR

case "$ARCH" in
    x86|arm) ;;
    *) echo "ARCH must be x86 or arm, got: $ARCH" >&2; exit 2 ;;
esac

# Source tree: clone the release tag when it is absent, then make sure the BTR
# patch is in. A tree that is neither patched nor patchable is an error, not
# something to build around.
if [ ! -e "$FIREFOX_SRC" ]; then
    echo "==> cloning $FIREFOX_REPO ($FIREFOX_TAG) into $FIREFOX_SRC"
    git clone --depth 1 --branch "$FIREFOX_TAG" "$FIREFOX_REPO" "$FIREFOX_SRC"
elif [ ! -f "$FIREFOX_SRC/mach" ]; then
    echo "FIREFOX_SRC=$FIREFOX_SRC is not a Firefox checkout (no ./mach)." >&2
    echo "Remove it, or point FIREFOX_SRC at a $FIREFOX_TAG tree." >&2
    exit 2
fi

# A tree at some other revision still builds, but not the tree the artifact
# numbers came from, so say so.
head_rev="$(git -C "$FIREFOX_SRC" rev-parse HEAD 2>/dev/null || true)"
tag_rev="$(git -C "$FIREFOX_SRC" rev-parse -q --verify "$FIREFOX_TAG^{commit}" 2>/dev/null || true)"
if [ -n "$tag_rev" ] && [ "$head_rev" != "$tag_rev" ]; then
    echo "==> warning: $FIREFOX_SRC is at ${head_rev:-an unknown revision}, not $FIREFOX_TAG" >&2
fi

if git -C "$FIREFOX_SRC" apply --reverse --check "$PATCH" 2>/dev/null; then
    echo "==> BTR patch already applied"
elif git -C "$FIREFOX_SRC" apply --check "$PATCH" 2>/dev/null; then
    echo "==> applying $(basename "$PATCH")"
    git -C "$FIREFOX_SRC" apply "$PATCH"
else
    echo "==> cannot apply $PATCH to $FIREFOX_SRC" >&2
    echo "    the tree must be at $FIREFOX_TAG and carry no conflicting edits" >&2
    exit 2
fi

# Image: an existing tag is reused only when it carries the toolchain this build
# needs, since an image built before the cross toolchain landed would fail at
# configure instead of here.
image_ok() {
    docker image inspect "$IMAGE" >/dev/null 2>&1 || return 1
    local probe='command -v clang >/dev/null && command -v rustc >/dev/null && command -v cbindgen >/dev/null'
    if [ "$ARCH" = arm ]; then
        probe="$probe && command -v aarch64-linux-gnu-gcc >/dev/null && rustup target list --installed | grep -qx aarch64-unknown-linux-gnu"
    fi
    docker run --rm "$IMAGE" bash -lc "$probe"
}

if [ "${REBUILD_IMAGE:-0}" = 1 ]; then
    echo "==> rebuilding image $IMAGE"
    "$HERE/docker-build.sh" image
elif image_ok; then
    echo "==> image $IMAGE present"
else
    echo "==> building image $IMAGE"
    "$HERE/docker-build.sh" image
fi

echo "==> building the $ARCH js shell"
"$HERE/docker-build.sh" build

JS_CMD="${OBJDIR:-$HERE/docker/build/obj-$ARCH}/dist/bin/js"
[ -x "$JS_CMD" ] || {
    echo "build finished without $JS_CMD" >&2
    exit 1
}
ENVFILE="$HERE/docker/build/jsenv-$ARCH.sh"
printf 'export JS_CMD=%s\n' "$JS_CMD" > "$ENVFILE"

echo
echo "==> shell: $JS_CMD"
echo "    source $ENVFILE"
