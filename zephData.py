#!/usr/bin/env python3
#
# This file is part of the ZEPHIRuS project.
#
# Copyright (c) 2026 Jason Toney
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <https://www.gnu.org/licenses/>.

import sys
from datetime import timedelta
import pandas as pd
from tabulate import tabulate

BINS = [1, 2, 3, 4]
ARRAY = {"A": BINS[0], "B": BINS[1], "C": BINS[2], "D": BINS[3]}


def analyze_csv(filename, array=ARRAY):
    df = pd.read_csv(filename)
    wind = "WindSpeed(m/s)"
    length = "Length(s)"
    results = []
    samplers = list(array.keys())
    values = list(array.values())
    for i, (sampler, lower) in enumerate(array.items()):
        if i < len(values) - 1:
            upper = values[i + 1]
            mask = (df[wind] >= lower) & (df[wind] < upper)
            label = f"{lower} - {upper - 0.01:.2f}"
        else:
            mask = df[wind] >= lower
            label = f"{lower}+"
        samples = mask.sum()
        seconds = df.loc[mask, length].sum()
        results.append(
            {
                "Sampler": sampler,
                "Wind Speed (m/s)": label,
                "Samples": samples,
                "Seconds": seconds,
                "Time": str(timedelta(seconds=int(seconds))),
            }
        )
    total_seconds = df[length].sum()
    return results, total_seconds


def print_results(results, total_seconds):
    table = [
        [r["Sampler"], r["Wind Speed (m/s)"], r["Samples"], r["Time"]] for r in results
    ]
    print(
        tabulate(
            table,
            headers=["Sampler", "Wind Speed (m/s)", "Samples", "Time"],
            tablefmt="simple",
        )
    )
    print()
    print(f"Total runtime: {timedelta(seconds=int(total_seconds))}")


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} input.csv")
        sys.exit(1)
    results, total_seconds = analyze_csv(sys.argv[1])
    print_results(results, total_seconds)


if __name__ == "__main__":
    main()
