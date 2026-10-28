# spidermonkey-docker

Containerized build of the **Firefox 143 JS shell** used by the BTR artifact's
SpiderMonkey attack-surface experiments.

## Why

Building the FF143 (`DEVEDITION_143_0b9_BUILD1`) tree on a current host fails in
two independent ways, both because the host toolchain is newer than the tree:

1. **`mach bootstrap` can't fetch a clang toolchain.** Bootstrap downloads a
   prebuilt `toolchain-linux64-clang` keyed on the current mozilla-central
   revision; a pinned release tag has no matching artifact, so it dies with
   *"Could not find artifacts for a toolchain build named `toolchain-linux64-clang`"*.
2. **rustc >= 1.98 breaks configure.** rust-lang/rust#157650 added
   OpenEmbedded `oe`-vendor Linux targets that collide with the `unknown`-vendor
   targets in FF143's `detect_rustc_target()`, so configure dies with
   *"Don't know how to translate x86_64-pc-linux-gnu for rustc"*
   (Mozilla [bug 2053518](https://bugzilla.mozilla.org/show_bug.cgi?id=2053518)).

The image carries its own clang and a pinned `rustc` (< 1.98) plus `cbindgen`,
so the build uses the image toolchain and never runs `mach bootstrap`. The
mozconfigs here drop `--enable-bootstrap` and set `CC=clang`/`CXX=clang++`.

The toolchain is **clang 19 (from apt.llvm.org) + rust 1.89**, matching the
19.1.7 the tree expects. The wasm constant-pool layout and the BTR grid/pool
parameters are deterministic and compiler-independent; the page-reuse behavior
depends on an exec-mem pool randomization source mod (disabled in the tuned
build), not on the compiler, so a self-contained distro clang is used.

## Layout

The launcher `docker-build.sh` sits one level up, in `attack-surface/spidermonkey/`;
this `docker/` directory holds the image definition it uses.

```
../docker-build.sh         driver script (run this)
docker/Dockerfile       ubuntu:24.04 + clang + pinned rust + cbindgen
docker/mozconfig/jsshell-x86   FF143 js-shell config, x86_64
docker/mozconfig/jsshell-arm   FF143 js-shell config, aarch64 (cross from any host arch)
docker/build/           objdirs (gitignored), one per arch
```

## Use

Commands below are run from `attack-surface/spidermonkey/` (where `docker-build.sh`
lives):

```sh
# one-time: build the image
./docker-build.sh image

# get the source and apply the BTR patch (once)
git clone https://github.com/mozilla-firefox/firefox.git
git -C firefox checkout DEVEDITION_143_0b9_BUILD1
git -C firefox apply jsshell/firefox143.patch

# build the js shell (tree at ./firefox by default, or pass FIREFOX_SRC=...)
./docker-build.sh build

./docker-build.sh versions            # report toolchain versions
./docker-build.sh shell               # interactive, inside the source tree
```

`docker-build.sh` defaults `FIREFOX_SRC` to `./firefox`, so a checkout in the
current directory needs no path; otherwise set `FIREFOX_SRC=/path/to/firefox`.

The objdir is kept out of the source tree, under `docker/build/obj-$ARCH/`; the
built binary lands at `docker/build/obj-$ARCH/dist/bin/js`. Point the
attack-surface scripts at it:

```sh
export JS_CMD=$PWD/docker/build/obj-x86/dist/bin/js
```

## Knobs

Environment variables read by `docker-build.sh`:

| var | default | meaning |
|---|---|---|
| `FIREFOX_SRC` | `./firefox` | source tree |
| `ARCH` | `x86` | `x86` or `arm`; picks `mozconfig/jsshell-$ARCH` (`arm` cross-compiles aarch64) |
| `OBJDIR` | `docker/build/obj-$ARCH` | build output, mounted outside the source tree |
| `MOZBUILD` | `$FIREFOX_SRC/.mozbuild-docker` | mach state + container `HOME` |
| `JOBS` | *(mach decides)* | parallel compile jobs |
| `MOUNTS` | *(none)* | extra host paths to mount at the same path |
| `IMAGE` | `spidermonkey:ff143` | image tag |

The pinned rust version is a build arg: `./docker-build.sh image` passes the current
UID/GID; to change rust, build directly, e.g.
`docker build --build-arg RUST_VERSION=1.95.0 -t spidermonkey:ff143 docker`.

## aarch64

The aarch64 experiments run on the arm boards (rpi5, Pixel 8, …), but the shell
is cross-compiled on the host:

```sh
ARCH=arm ./docker-build.sh build          # objdir docker/build/obj-arm
cp docker/build/obj-arm/dist/bin/js <board>:/path/to/js
```

The image carries an aarch64 cross toolchain: `g++-aarch64-linux-gnu` supplies the
sysroot at `/usr/aarch64-linux-gnu`, clang gets `--target=aarch64-linux-gnu` from
`mozconfig/jsshell-arm`, lld links the aarch64 objects, rust has the
`aarch64-unknown-linux-gnu` std installed, and zlib comes from the bundled
`modules/zlib` (`--without-system-zlib`, since the sysroot has no libz and
pkg-config would answer for the host's). On an arm64 host the same mozconfig
builds natively, so `ARCH=arm` works either way.

The sysroot is Ubuntu 24.04, and the linked binary needs `GLIBC_2.38` and
`GLIBCXX_3.4.32` (libstdc++ from GCC 13) at runtime: the board must provide those
or the shell won't start. For an older board, build on the board itself instead
of cross-compiling.

Rebuild the image after pulling this cross-toolchain change
(`./docker-build.sh image`), otherwise `ARCH=arm` fails at configure with a
missing target compiler.
