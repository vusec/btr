#!/usr/bin/env python3
from enum import Enum
import sys
import re
import argparse
import pandas as pd


class loopType(Enum):
    LOOP_NONE = 0
    # LOOP_SLEEP = 1
    LOOP_BUSY = 2
    LOOP_BUSY_FORK = 3

class trainType(Enum):
    TRAIN_SINGLE_TARGET = 0
    TRAIN_MOCK_TARGET = 1
    TRAIN_ALTERNATE_TARGET = 2


def main(log_file, output_file):

    with open(log_file) as f:

        items = []

        for line in f:
            if "loop_var" not in line:
                continue
            if "his_size" not in line:
                continue

            dict_line = line.split("}")[0] + "}"

            try:
                items.append(eval(dict_line))
            except Exception as e:
                print("Error parsing line:", dict_line)
                print(e)
                continue

    df = pd.DataFrame(items)

    # Calculate average for each group
    avg_hits = df.groupby(['m', 'loop_var', 'loop_type', 'train_type', 'overwrite', 'n_his', 'his_takes', 'his_size'])['hits'].mean()
    df_avg = avg_hits.reset_index()

    max_hits = df_avg.sort_values("hits").groupby(['m', 'n_his', 'loop_var', 'loop_type', 'train_type', 'overwrite']).last().reset_index()
    df_max = max_hits.reset_index(drop=True)

    df_max.to_csv(output_file, index=False)

    # No loop
    max_idx = df_max[df_max['loop_type'] == loopType.LOOP_NONE.value]['hits'].idxmax()

    print("Max hits without loop:")
    print(f"- Configuration: {df_max.iloc[max_idx][['n_his', 'loop_var', 'loop_type', 'train_type']].to_dict()}")
    print(f"- Max hits: {int(df_max.iloc[max_idx]['hits'])}")


    print("\nMax hits with busy empty loop:")
    df_busy_empty = df_max[ (df_max['loop_type'] == loopType.LOOP_BUSY.value) & (df_max['overwrite'] == 0) ]
    print(f"- 10**6 cpu cycles: {int(df_busy_empty[df_busy_empty['loop_var'] == 1000000]['hits'].max())}")
    print(f"- 10**7 cpu cycles: {int(df_busy_empty[df_busy_empty['loop_var'] == 10000000]['hits'].max())}")
    print(f"- 10**8 cpu cycles: {int(df_busy_empty[df_busy_empty['loop_var'] == 100000000]['hits'].max())}")
    print(f"- 10**9 cpu cycles: {int(df_busy_empty[df_busy_empty['loop_var'] == 1000000000]['hits'].max())}")


    print("\nMax hits with busy empty loop, rewritten target:")
    df_busy_empty = df_max[ (df_max['loop_type'] == loopType.LOOP_BUSY.value) & (df_max['overwrite'] == 1) ]
    print(f"- 10**6 cpu cycles: {int(df_busy_empty[df_busy_empty['loop_var'] == 1000000]['hits'].max())}")
    print(f"- 10**7 cpu cycles: {int(df_busy_empty[df_busy_empty['loop_var'] == 10000000]['hits'].max())}")
    print(f"- 10**8 cpu cycles: {int(df_busy_empty[df_busy_empty['loop_var'] == 100000000]['hits'].max())}")
    print(f"- 10**9 cpu cycles: {int(df_busy_empty[df_busy_empty['loop_var'] == 1000000000]['hits'].max())}")

    print("\nMax hits with system activity (fork + execve):")
    df_busy_fork = df_max[ (df_max['loop_type'] == loopType.LOOP_BUSY_FORK.value) & (df_max['overwrite'] == 0) ]
    print(f"- 1 fork + execve: {int(df_busy_fork[df_busy_fork['loop_var'] == 1]['hits'].max())}")
    print(f"- 10 fork + execve: {int(df_busy_fork[df_busy_fork['loop_var'] == 10]['hits'].max())}")
    print(f"- 100 fork + execve: {int(df_busy_fork[df_busy_fork['loop_var'] == 100]['hits'].max())}")
    print(f"- 1000 fork + execve: {int(df_busy_fork[df_busy_fork['loop_var'] == 1000]['hits'].max())}")


if __name__ == '__main__':

    arg_parser = argparse.ArgumentParser(description='History collisions log file')
    arg_parser.add_argument('input')
    arg_parser.add_argument('output')

    args = arg_parser.parse_args()

    main(args.input, args.output)
