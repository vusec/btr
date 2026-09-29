# Microarchitectural Experiments

This folder contains all code to both run the microarchitectural experiments and
create the graphs.

## Run the experiments

Run the following command to run the experiment on the targeted machine.
Replace `ARCH=` with the current machine, see `config.h` for all pre-configured
machines.

```sh
sudo ARCH=CORTEX_X3 ./run.sh | tee results/log_Cortex_X3.txt
```

## Generate the graphs

Once all log files are collected in the `results` folder, run the command below.
It will generate the aggregated files and next the graphs.

```sh
./gen_graphs.sh
```
