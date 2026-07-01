#!/usr/bin/env python3

# MIT License
#
# Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All rights reserved.
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.

import sys
import pytest
import numpy as np
import pandas as pd
import re


def unique(lst):
    return list(set(lst))


def validate_csv(df, kernel_list, counter_name):

    assert not df.empty
    df_Agent_id = df["Agent_Id"].str.split(" ").str[-1]
    assert (df_Agent_id.astype(int).values >= 0).all()
    assert (df["Queue_Id"].astype(int).values > 0).all()
    assert (df["Process_Id"].astype(int).values > 0).all()
    assert len(df["Kernel_Name"]) > 0

    counter_collection_pmc1_kernel_list = [
        x
        for x in sorted(df["Kernel_Name"].unique().tolist())
        if not re.search(r"__amd_rocclr_.*", x)
    ]

    assert kernel_list == counter_collection_pmc1_kernel_list

    kernel_count = dict([[itr, 0] for itr in kernel_list])
    assert len(kernel_count) == len(kernel_list)
    for itr in df["Kernel_Name"]:
        if re.search(r"__amd_rocclr_.*", itr):
            continue
        kernel_count[itr] += 1
    kn_cnt = [itr for _, itr in kernel_count.items()]
    assert min(kn_cnt) == max(kn_cnt) and len(unique(kn_cnt)) == 1

    assert len(df["Counter_Value"]) > 0
    assert df["Counter_Name"].str.contains(counter_name).all()
    assert (df["Counter_Value"].astype(int).values > 0).all()


def validate_csv_iteration_range(df, kernel_list, counter_name, iteration_range):

    # Reuse the standard kernel-name / counter checks and the per-kernel
    # equal-count invariant.
    validate_csv(df, kernel_list, counter_name)

    # Additionally assert that the number of captured dispatches per kernel
    # equals the size of the requested --kernel-iteration-range, i.e. that the
    # iteration-range filter selected exactly the requested launch indices and
    # not every launch of each kernel.
    expected_count = len(iteration_range)
    assert expected_count > 0

    kernel_count = dict([[itr, 0] for itr in kernel_list])
    for itr in df["Kernel_Name"]:
        if re.search(r"__amd_rocclr_.*", itr):
            continue
        kernel_count[itr] += 1

    for kernel_name, count in kernel_count.items():
        assert (
            count == expected_count
        ), f"{kernel_name} captured {count} dispatches, expected {expected_count}"


def validate_json_iteration_range(json_data, kernel_list, iteration_range):

    # Prove that the EXACT requested launch indices were captured (not merely
    # the right count). --kernel-trace is enabled for this test, so
    # buffer_records.kernel_dispatch contains every launch of every kernel
    # (the dispatch trace is not affected by --kernel-iteration-range), whereas
    # callback_records.counter_collection only contains the SELECTED dispatches.
    # We recover each kernel's per-name launch ordinal from the full trace and
    # assert the selected ordinals equal the requested range set.
    data = json_data["rocprofiler-sdk-tool"]
    counter_collection_data = data["callback_records"]["counter_collection"]
    kernel_dispatch_data = data["buffer_records"]["kernel_dispatch"]

    def get_kernel_name(kernel_id):
        return data["kernel_symbols"][kernel_id]["formatted_kernel_name"]

    # Walk the full (unfiltered) dispatch trace in order and assign each
    # dispatch_id its 1-based ordinal within its own kernel name.
    per_kernel_seen = dict([[itr, 0] for itr in kernel_list])
    ordinal_by_dispatch_id = {}
    kernel_by_dispatch_id = {}
    for dispatch in kernel_dispatch_data:
        dispatch_info = dispatch["dispatch_info"]
        kernel_name = get_kernel_name(dispatch_info["kernel_id"])
        if kernel_name not in per_kernel_seen:
            continue
        per_kernel_seen[kernel_name] += 1
        dispatch_id = dispatch_info["dispatch_id"]
        ordinal_by_dispatch_id[dispatch_id] = per_kernel_seen[kernel_name]
        kernel_by_dispatch_id[dispatch_id] = kernel_name

    # The dispatch trace must contain strictly MORE launches per kernel than the
    # size of the requested range; otherwise we could not distinguish the
    # original launch ordinals from a trace that had itself been filtered down
    # to the range. vector-ops launches each kernel once per stream (3 streams
    # in this test), so with a 2-element range every targeted kernel is launched
    # 3 > 2 times. This assertion is what makes the "exact iteration" proof
    # sound: it confirms --kernel-iteration-range filters only the
    # counter_collection callback records, not the kernel_dispatch trace, which
    # is the same convention the sibling range_filtering test relies on.
    expected_ordinals = set(iteration_range)
    for kernel_name in kernel_list:
        assert per_kernel_seen[kernel_name] > len(expected_ordinals), (
            f"{kernel_name} launched {per_kernel_seen[kernel_name]} times; "
            f"expected more than the {len(expected_ordinals)} requested "
            "iterations so original launch ordinals can be recovered"
        )

    # Collect the per-kernel ordinals that were actually counter-collected.
    captured_ordinals = dict([[itr, set()] for itr in kernel_list])
    for counter in counter_collection_data:
        dispatch_info = counter["dispatch_data"]["dispatch_info"]
        dispatch_id = dispatch_info["dispatch_id"]
        kernel_name = kernel_by_dispatch_id.get(dispatch_id)
        if kernel_name is None:
            continue
        captured_ordinals[kernel_name].add(ordinal_by_dispatch_id[dispatch_id])

    for kernel_name in kernel_list:
        assert captured_ordinals[kernel_name] == expected_ordinals, (
            f"{kernel_name} captured iterations {sorted(captured_ordinals[kernel_name])}, "
            f"expected {sorted(expected_ordinals)}"
        )


def validate_json(json_data, counter_name, check_dispatch):

    data = json_data["rocprofiler-sdk-tool"]
    counter_collection_data = data["callback_records"]["counter_collection"]
    dispatch_ids = []
    # at present, AQLProfile has bugs when reporting the counters for below architectures
    skip_gfx = ("gfx1101", "gfx1102", "gfx1150", "gfx1151", "gfx1152", "gfx1153")

    def get_kernel_name(kernel_id):
        return data["kernel_symbols"][kernel_id]["formatted_kernel_name"]

    def get_agent(agent_id):
        for agent in data["agents"]:
            if agent["id"]["handle"] == agent_id["handle"]:
                return agent
        return None

    def get_counter(counter_id):
        for counter in data["counters"]:
            if counter["id"]["handle"] == counter_id["handle"]:
                return counter
        return None

    for counter in counter_collection_data:
        dispatch_data = counter["dispatch_data"]["dispatch_info"]

        assert dispatch_data["dispatch_id"] > 0
        assert dispatch_data["agent_id"]["handle"] > 0
        assert dispatch_data["queue_id"]["handle"] > 0

        agent = get_agent(dispatch_data["agent_id"])
        kernel_name = get_kernel_name(dispatch_data["kernel_id"])

        assert agent is not None
        assert len(kernel_name) > 0

        dispatch_ids.append(dispatch_data["dispatch_id"])
        if not re.search(r"__amd_rocclr_.*", kernel_name):
            values = []
            for record in counter["records"]:
                counter = get_counter(record["counter_id"])
                assert counter is not None, f"record:\n\t{record}"
                assert (
                    counter["name"] == counter_name
                ), f"record:\n\t{record}\ncounter:\n\t{counter}"
                if agent["name"] not in skip_gfx:
                    values.append(record["value"])

            # Check aggregate sum
            if agent["name"] not in skip_gfx:
                assert sum(values) > 0, f"{counter_name} value is not > 0"

    if check_dispatch:
        di_uniq = list(set(sorted(dispatch_ids)))
        # make sure the dispatch ids are unique and ordered
        di_expect = [idx + 1 for idx in range(len(dispatch_ids))]
        assert di_expect == di_uniq


def test_validate_counter_collection_csv_pass1(input_csv_pass1: pd.DataFrame):
    kernel_list = sorted(["addition_kernel", "subtract_kernel", "divide_kernel"])
    validate_csv(input_csv_pass1, kernel_list, "SQ_WAVES")


def test_validate_counter_collection_csv_pmc1(input_csv_pmc1: pd.DataFrame):
    kernel_list = sorted(["addition_kernel", "subtract_kernel", "divide_kernel"])
    validate_csv(input_csv_pmc1, kernel_list, "SQ_WAVES")


def test_validate_counter_collection_csv_iteration_range(
    input_csv_iteration_range: pd.DataFrame, iteration_range
):
    kernel_list = sorted(
        ["addition_kernel", "subtract_kernel", "multiply_kernel", "divide_kernel"]
    )
    validate_csv_iteration_range(
        input_csv_iteration_range, kernel_list, "SQ_WAVES", iteration_range
    )


def test_validate_counter_collection_json_iteration_range(
    input_json_iteration_range, iteration_range
):
    kernel_list = sorted(
        ["addition_kernel", "subtract_kernel", "multiply_kernel", "divide_kernel"]
    )
    validate_json_iteration_range(
        input_json_iteration_range, kernel_list, iteration_range
    )


def test_validate_counter_collection_csv_pass2(input_csv_pass2: pd.DataFrame):
    kernel_list = sorted(
        ["addition_kernel", "subtract_kernel", "multiply_kernel", "divide_kernel"]
    )
    validate_csv(input_csv_pass2, kernel_list, "GRBM_COUNT")


def test_validate_counter_collection_csv_pass3(input_csv_pass3: pd.DataFrame):
    kernel_list = sorted(
        ["addition_kernel", "subtract_kernel", "multiply_kernel", "divide_kernel"]
    )
    validate_csv(input_csv_pass3, kernel_list, "GRBM_GUI_ACTIVE")


def test_validate_counter_collection_csv_pass4(input_csv_pass4: pd.DataFrame):
    kernel_list = sorted(["divide_kernel"])
    validate_csv(input_csv_pass4, kernel_list, "SQ_WAVES")


def test_validate_counter_collection_json_pass1(input_json_pass1):
    validate_json(input_json_pass1, "SQ_WAVES", False)


def test_validate_counter_collection_json_pass2(input_json_pass2):
    validate_json(input_json_pass2, "GRBM_COUNT", False)


def test_validate_counter_collection_json_pass3(input_json_pass3):
    validate_json(input_json_pass3, "GRBM_GUI_ACTIVE", True)


def test_validate_counter_collection_json_pass4(input_json_pass4):
    validate_json(input_json_pass4, "SQ_WAVES", False)


if __name__ == "__main__":
    exit_code = pytest.main(["-x", __file__] + sys.argv[1:])
    sys.exit(exit_code)
