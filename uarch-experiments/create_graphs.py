#!/usr/bin/env python3
import sys
import re
import os
import argparse
import pandas as pd
import matplotlib.pyplot as plt
from matplotlib.ticker import ScalarFormatter
from matplotlib import ticker as mticker
import numpy as np
from matplotlib.patches import Patch

# IBM:
line_colors = ['#ffb000', '#fe6100', '#dc267f', '#785ef0', '#648fff']
# Tol color:
tol_bright = ["#4477AA", "#EE6677", "#228833", "#CCBB44", "#66CCEE", "#AA3377", "#BBBBBB"]
tol_vibrant = ["#0077BB", "#33BBEE", "#009988", "#EE7733", "#CC3311", "#EE3377", "#BBBBBB"]
tol_muted = ["#332288", "#88CCEE", "#44AA99", "#117733", "#999933", "#DDCC77", "#CC6677", "#882255", "#AA4499"]

line_colors = tol_vibrant


LOOP_NONE = 0
LOOP_SLEEP = 1
LOOP_BUSY = 2
LOOP_BUSY_FORK = 3

TRAIN_SINGLE_TARGET = 0
TRAIN_MOCK_TARGET = 1
TRAIN_ALTERNATE_TARGET = 2

train_patterns = ['', '///', '\\\\']
train_labels = ['Single Target', 'Mock Target', 'Alternate Target']

machines = ['Cortex-A76', 'Cortex-X3', 'i9-14900K', '9-285K', '7950X']

machines_l = {'Cortex-A76':'Cortex-A76', 'Cortex-X3':'Cortex-X3', 'i9-14900K':'Raptor Cove', '9-285K' : 'Lion Cove', '7950X' : 'Zen 4'}


def format_sci(val):
        if val >= 100:
            # Formats as 1x10^6
            s = "{:.0e}".format(val).replace("e+0", "e").replace("e+", "e")
            base, exp = s.split('e')
            return f"${base} \\times 10^{{{exp}}}$" if base != "1" else f"$10^{{{exp}}}$"
        return str(val)

def create_mega_graph(df, output_folder):
    plt.rcParams.update({'figure.dpi': '300'})

    df_single = df[(df['loop_type'] == LOOP_NONE) & (df['train_type'] == TRAIN_SINGLE_TARGET)][['m', 'n_his', 'hits']]
    df_mock = df[(df['loop_type'] == LOOP_NONE) & (df['train_type'] == TRAIN_MOCK_TARGET)][['m', 'n_his', 'hits']]
    df_alternate = df[(df['loop_type'] == LOOP_NONE) & (df['train_type'] == TRAIN_ALTERNATE_TARGET)][['m', 'n_his', 'hits']]

    datasets = [df_single, df_mock, df_alternate]

    n_his_values = sorted(df['n_his'].unique())

    num_train_types = len(datasets)
    num_machines = len(machines)
    total_bars_per_group = num_machines * num_train_types

    total_group_width = 0.9
    machine_gap = 0.02  # Gap between different machines

    # Setup bar_width
    total_gap_space = machine_gap * (num_machines - 1)
    bar_width = (total_group_width - total_gap_space) / (num_machines * num_train_types)

    fig, ax = plt.subplots(figsize=(14, 2.1))
    x = np.arange(len(n_his_values))

    for h_idx, n_his in enumerate(n_his_values):
        for d_idx, dataset in enumerate(datasets):
            for m_idx, machine in enumerate(machines):

                # Calculate bar position
                offset = (m_idx * num_train_types * bar_width) + (m_idx * machine_gap) + (d_idx * bar_width)

                # Extract hit value for this specific machine and history count
                val_row = dataset[(dataset['n_his'] == n_his) & (dataset['m'] == machine)]
                val = val_row['hits'].values[0] if not val_row.empty else 0

                ax.bar(
                    x[h_idx] + offset,
                    val,
                    width=bar_width,
                    color=line_colors[m_idx % len(line_colors)],
                    hatch=train_patterns[d_idx],
                    edgecolor='white',
                    linewidth=0.1
                )

    # Machine legend
    machine_patches = [Patch(facecolor=line_colors[i], label=machines_l[machines[i]]) for i in range(len(machines))]
    leg1 = ax.legend(
        handles=machine_patches,
        loc='upper left',
        fontsize=9,
        frameon=False,
        bbox_to_anchor=(0.2, 1.2),
        ncol=5,
        columnspacing=4
    )
    ax.add_artist(leg1)

    # Training Type Legend
    train_patches = [
        Patch(facecolor="black", edgecolor='white', hatch=train_patterns[i], label=train_labels[i])
        for i in range(len(train_labels))
    ]

    ax.legend(
        handles=train_patches,
        loc='upper left',
        bbox_to_anchor=(0, 1),
        ncol=1,
        fontsize=9,
        title_fontsize=9,
        frameon=False,
        title='Training Type',
    )

    # Formatting
    ax.set_xticks(x + (total_group_width / 2) - (bar_width / 2))
    ax.set_xticklabels(n_his_values)

    ax.set_ylabel("# Hits")
    ax.set_xlabel("Inserted histories")
    ax.set_yscale("log", base=2)

    # Y-axis limits and ticks
    x1, x2, y1, y2 = plt.axis()
    ax.set_ylim([8, 9000])
    ax.set_yticks([8, 32, 128, 512, 2048, 8192])
    ax.yaxis.set_major_formatter(ScalarFormatter())

    # Minor ticks for log scale
    powers = 2 ** np.arange(4, 14)
    ax.yaxis.set_minor_locator(mticker.FixedLocator(powers))
    ax.yaxis.set_minor_formatter(mticker.NullFormatter())

    plt.tick_params(axis='x', which='both', bottom=True, top=False)
    plt.grid(axis='y', which='major', linewidth=0.1, alpha=0.2)
    plt.grid(axis='y', which='minor', linewidth=0.1, alpha=0.2)

    fig.tight_layout()
    fig.savefig(os.path.join(output_folder, "mega_history_graph.pdf"))


def create_graph_machine_panels(dataset, name, output_folder, xaxis_title, location="left"):
    plt.rcParams.update({
        'figure.dpi': '300'
    })

    dataset = dataset.sort_index()

    available_machines = [m for m in machines if m in dataset.columns]
    categories = dataset.index.tolist()

    num_machines = len(available_machines)
    num_categories = len(categories)

    if location == 'left':
        fig, ax = plt.subplots(figsize=(5 * 1.04, 1.7))
    else:
        fig, ax = plt.subplots(figsize=(5, 1.7))

    # Bar settings
    bar_width = 0.7
    machine_group_gap = 0.2  # Gap between machine clusters

    all_x_pos = []
    all_x_labels = []

    for m_idx, machine in enumerate(available_machines):
        # Calculate where this machine's cluster starts
        # Cluster offset = machine_index * (number of bars + the gap)
        base_x = m_idx * (num_categories + machine_group_gap)

        machine_hits = dataset[machine].values
        machine_color = line_colors[m_idx % len(line_colors)]

        if (location == "left" and m_idx == 0) or (location == "center" and m_idx in [1,2,3]) or (location == "right" and m_idx == 4):
            label = machines_l[machine]
        else:
            label = None

        for c_idx, cat in enumerate(categories):
            if c_idx > 0: label =  None

            pos = base_x + c_idx
            ax.bar(
                pos,
                machine_hits[c_idx],
                width=bar_width,
                color=machine_color,
                label=label,
            )

            all_x_pos.append(pos)
            all_x_labels.append(format_sci(cat))


    # Primary X-axis: The Categories (1, 10, 100)
    ax.set_xticks(all_x_pos)
    ax.set_xticklabels(all_x_labels, fontsize=7)

    if location == 'left':
        ax.set_ylabel("# Hits")

    # Move the xaxis title down slightly to avoid clashing with machine names
    ax.set_xlabel(xaxis_title)

    # Log scale for Hits
    ax.set_yscale("log", base=2)
    ax.set_ylim([8, 9000])
    ax.set_yticks([8, 32, 128, 512, 2048, 8192])

    ax.yaxis.set_major_formatter(ScalarFormatter())

    # Minor ticks
    powers = 2 ** np.arange(3, 14)
    ax.yaxis.set_minor_locator(mticker.FixedLocator(powers))
    ax.yaxis.set_minor_formatter(mticker.NullFormatter())


    plt.tick_params(axis='x', which='both', bottom=True, top=False)
    plt.grid(axis='y', which='major', linewidth=0.1, alpha=0.2)
    plt.grid(axis='y', which='minor', linewidth=0.1, alpha=0.2)


    fig.tight_layout()

    if location == 'left':
        ax.legend(loc = "upper right", bbox_to_anchor=(1, 1.4), ncol=4, frameon=False)
    elif location == 'center':
        ax.legend(loc = "upper left", bbox_to_anchor=(-0.07, 1.4), ncol=4, frameon=False, columnspacing=3.5)
    elif location == 'right':
        ax.legend(loc = "upper left", bbox_to_anchor=(-0.07, 1.4), ncol=4, frameon=False)

    fig.savefig(os.path.join(output_folder, f"{name}.pdf"),  bbox_inches='tight')


def main(agg_file, output_folder):

    if not os.path.exists(output_folder):
        os.makedirs(output_folder)

    df = pd.read_csv(agg_file)

    create_mega_graph(df, output_folder)

    # ----------------------------------------------------------------------------------
    # busy loop

    df_busy =  df[((df['loop_type'] == LOOP_BUSY) | (df['loop_type'] == LOOP_NONE)) & (df['overwrite'] == 0) & (df['loop_var'] != 0)]
    max_hits = df_busy.groupby(['m', 'loop_var'])['hits'].max().reset_index()
    df_busy = max_hits.pivot(index='loop_var', columns='m', values='hits')

    create_graph_machine_panels(df_busy, "busy_loop", output_folder, 'Number of CPU cycles', location='left')
    # ----------------------------------------------------------------------------------
    # busy fork loop

    df_busy =  df[(df['loop_type'] == LOOP_BUSY_FORK) | (df['loop_type'] == LOOP_NONE) & (df['overwrite'] == 0) & (df['loop_var'] != 0)]
    max_hits = df_busy.groupby(['m', 'loop_var'])['hits'].max().reset_index()
    df_busy = max_hits.pivot(index='loop_var', columns='m', values='hits')

    create_graph_machine_panels(df_busy, "busy_loop_fork", output_folder, 'Number of fork + execve executions', location='right')

    # ----------------------------------------------------------------------------------
    # busy loop + overwrite

    df_busy =  df[((df['loop_type'] == LOOP_BUSY) | (df['loop_type'] == LOOP_NONE)) & (df['overwrite'] == 1) & (df['loop_var'] != 0)]
    max_hits = df_busy.groupby(['m', 'loop_var'])['hits'].max().reset_index()
    df_busy = max_hits.pivot(index='loop_var', columns='m', values='hits')

    create_graph_machine_panels(df_busy, "busy_loop_overwrite", output_folder, 'Number of CPU cycles', location='center')


if __name__ == '__main__':

    arg_parser = argparse.ArgumentParser(description='Create History collisions graphs')
    arg_parser.add_argument('input')
    arg_parser.add_argument('output_folder')

    args = arg_parser.parse_args()

    main(args.input, args.output_folder)
