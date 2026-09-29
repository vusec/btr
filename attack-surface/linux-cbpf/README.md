# Attack Surface Analysis - Linux cBPF

This folder contains the code for the attack surface analysis on cBPF.

## Linux kernel

The experiments require the Ubuntu kernel 6.14.0-27-generic (Ubuntu 24.04), the
same kernel as used for the end-to-end exploit. It can be installed and scheduled
to boot on the next reboot with:

```sh
../../e2e-exploit/install_kernel.sh
```

See [e2e-exploit/README.md](../../e2e-exploit/README.md) for alternative
installation methods. Please reboot into this kernel before continuing.

## Setup

Please install the kernel module (warning: only install on test environments).
The kernel module contains helper functions and inserts 2 mock gadgets.

```sh
sudo ./setup.sh
```

## Run the experiments

The testbed supports 3 modes to run:

- `test_reuse_rate`: test the chunk-reuse rate
- `test_leakage_rate`: test the leakage rate
- `leak_dummy`: leak a dummy secret (alphabet)

```sh
Usage:
./run.sh {test_leakage_rate, test_reuse_rate, leak_dummy} [options]
  -c                 Bypass constant-blind
```

Provide the machine to test with the `ARCH=` variable. Supported values are
`LION_COVE`, `RAPTOR_COVE`.
For example, to test the chunk reuse rate on Lion Cove (e.g., Intel Ultra 9 285K):

```sh
ARCH=LION_COVE ./run.sh test_reuse_rate
```

## Results

The results are printed to the terminal. The relevant numbers are:

- `Iteration Avg time: ... us`, the average time one leak iteration takes in
  microseconds.
- `Reuse: ... / 1000`, the chunk reuse numbers (e.g., a reuse of 375 / 1000 on
  average is a reuse rate of 37.5%).

## Run with constant-blinding enabled

To perform the tests with BPF constant-blinding enabled, first enable `bpf_jit_harden`:

```sh
echo "2" | sudo tee /proc/sys/net/core/bpf_jit_harden
```

Next run the commands with the `-c` option:

```sh
ARCH=LION_COVE ./run.sh -c test_reuse_rate
```
