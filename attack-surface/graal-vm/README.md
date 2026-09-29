# Attack Surface Analysis - GraalVM

This folder contains the code for the attack surface analysis on GraalVM 25.0.2.

## Setup

Download and install GraalVM JDK 25.0.2 from the [Oracle GraalVM downloads page](https://www.oracle.com/java/technologies/downloads/).
Extract the archive to `~/.local/bin/` so the resulting path is `~/.local/bin/graalvm-jdk-25.0.2`.

Set up the environment:

```sh
source env.sh
```

Build the native image:

```sh
mvn clean -DskipTests -Pnative -Pisolated package
```

This produces the `target/embedding` binary used by all experiment scripts.

Build `libfr.so` from the uarch experiments and copy it here. Replace `ARCH` with the target machine (see [uarch-experiments/README.md](../../uarch-experiments/README.md) for valid values):

```sh
make -C ../../uarch-experiments so ARCH=$ARCH
cp ../../uarch-experiments/libfr.so .
```

## Run the experiments

All scripts source `common.sh`, which selects per-microarchitecture parameters.
Pass the target microarchitecture as the first argument:

|   Argument   |                 CPU                 |
|--------------|-------------------------------------|
| `x3`         | Cortex-X3 (Pixel 8)                 |
| `a76`        | Cortex-A76 (Raspberry Pi 5)         |
| `zen4`       | AMD Zen 4  (Ryzen 9 7950X)          |
| `raptorcove` | Intel Raptor Cove (Core i9-14900K)  |
| `lioncove`   | Intel Lion Cove (Core Ultra 9 285K) |

### `reuse_and_rate.sh` — measure the chunk-reuse rate

Runs 100 repetitions and pipes the output through `replace_rate.py` to report the replacement rate.

```sh
./reuse_and_rate.sh <uarch>
# e.g.:
./reuse_and_rate.sh zen4
```

### `btb-control.sh` — sweep BTB control parameters

Sweeps `nr_entry`, `len_bh`, and `nr_bcond_taken` and reports how many iBTB entries stay controlled (`hit0`): the top-5 combinations per mode, and the maximum each mode reaches.

The second argument selects the modes (comma-separated, default `fixed,churn`):

| mode | flags | what it measures |
|---|---|---|
| `fixed` | `--skip-reuse` | the upper bound on controllable entries: train and reload back to back, trainer chunks left mapped |
| `churn` | — | how many of those entries survive the interference the attack carries: the trainer chunks are GC'd and target chunks compiled onto the freed addresses between training and probing |

`NR_REPEATS` (default 10) sets the probe reps per point, `SETTLE` (default 1) the pause between points.

```sh
./btb-control.sh <uarch> [modes]
# e.g.:
./btb-control.sh raptorcove
./btb-control.sh raptorcove fixed
./btb-control.sh raptorcove churn
```
