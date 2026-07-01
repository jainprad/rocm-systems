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

import os
import re
import sys
import pytest

# Number of passes expected for the multi-pass tests (one --pmc group / input job
# per pass). This suite exercises SQ_WAVES, GRBM_COUNT and GRBM_GUI_ACTIVE.
EXPECTED_PASS_COUNT = 3

# Internal HIP runtime copy kernels can legitimately report a counter value of 0,
# so the strict "> 0" counter-value check is only applied to real kernel rows.
INTERNAL_KERNEL_RE = re.compile(r"__amd_rocclr_.*")


def _is_real_kernel(kernel_name):
    return not INTERNAL_KERNEL_RE.search(kernel_name)


def _validate_counter_data(counter_data, expected_counter, pass_label):
    """Common counter-value validation for a single pass"""
    assert len(counter_data) > 0, f"No counter data found in {pass_label}"

    for row in counter_data:
        assert (
            row["Counter_Name"] == expected_counter
        ), f"Expected {expected_counter} in {pass_label}, got {row['Counter_Name']}"
        assert int(row["Queue_Id"]) > 0
        assert int(row["Process_Id"]) > 0
        assert len(row["Kernel_Name"]) > 0
        assert len(row["Counter_Value"]) > 0
        # Real kernel dispatches must report a positive counter value; internal
        # runtime copy kernels may legitimately read 0.
        if _is_real_kernel(row["Kernel_Name"]):
            assert (
                float(row["Counter_Value"]) > 0
            ), f"{expected_counter} value is not > 0 for {row['Kernel_Name']} in {pass_label}"
        else:
            assert float(row["Counter_Value"]) >= 0


def test_pass_directories_exist(output_dir):
    """Verify that pass_1, pass_2 and pass_3 directories were created"""
    pass1_dir = os.path.join(output_dir, "pass_1")
    pass2_dir = os.path.join(output_dir, "pass_2")
    pass3_dir = os.path.join(output_dir, "pass_3")

    assert os.path.isdir(pass1_dir), f"Expected pass_1 directory at {pass1_dir}"
    assert os.path.isdir(pass2_dir), f"Expected pass_2 directory at {pass2_dir}"
    assert os.path.isdir(pass3_dir), f"Expected pass_3 directory at {pass3_dir}"


def test_pass_count(output_dir):
    """Verify the pass_* directories are contiguously 1-indexed with no pass_0"""
    pass_dirs = sorted(
        d
        for d in os.listdir(output_dir)
        if d.startswith("pass_") and os.path.isdir(os.path.join(output_dir, d))
    )

    # this repo is 1-indexed; there must never be a pass_0
    assert (
        "pass_0" not in pass_dirs
    ), f"pass_0 should not exist (passes are 1-indexed), got {pass_dirs}"

    # exactly the expected number of passes should be present
    assert (
        len(pass_dirs) == EXPECTED_PASS_COUNT
    ), f"Expected {EXPECTED_PASS_COUNT} pass_* directories, found {len(pass_dirs)}: {pass_dirs}"

    # the passes must be contiguously 1-indexed starting at pass_1
    pass_indices = sorted(int(d.split("_", 1)[1]) for d in pass_dirs)
    assert pass_indices == list(
        range(1, EXPECTED_PASS_COUNT + 1)
    ), f"Pass directories must be contiguously 1-indexed starting at pass_1, got {pass_indices}"


def test_pass1_agent_info(pass1_agent_info):
    """Validate agent info from pass 1"""
    assert len(pass1_agent_info) > 0, "No agent info found in pass 1"

    for row in pass1_agent_info:
        agent_type = row["Agent_Type"]
        assert agent_type in ("CPU", "GPU")
        if agent_type == "CPU":
            assert int(row["Cpu_Cores_Count"]) > 0
            assert int(row["Simd_Count"]) == 0
        else:
            assert int(row["Cpu_Cores_Count"]) == 0
            assert int(row["Simd_Count"]) > 0


def test_pass2_agent_info(pass2_agent_info):
    """Validate agent info from pass 2"""
    assert len(pass2_agent_info) > 0, "No agent info found in pass 2"

    for row in pass2_agent_info:
        agent_type = row["Agent_Type"]
        assert agent_type in ("CPU", "GPU")
        if agent_type == "CPU":
            assert int(row["Cpu_Cores_Count"]) > 0
            assert int(row["Simd_Count"]) == 0
        else:
            assert int(row["Cpu_Cores_Count"]) == 0
            assert int(row["Simd_Count"]) > 0


def test_pass3_agent_info(pass3_agent_info):
    """Validate agent info from pass 3"""
    assert len(pass3_agent_info) > 0, "No agent info found in pass 3"

    for row in pass3_agent_info:
        agent_type = row["Agent_Type"]
        assert agent_type in ("CPU", "GPU")
        if agent_type == "CPU":
            assert int(row["Cpu_Cores_Count"]) > 0
            assert int(row["Simd_Count"]) == 0
        else:
            assert int(row["Cpu_Cores_Count"]) == 0
            assert int(row["Simd_Count"]) > 0


def test_pass1_counters(pass1_counter_data):
    """Validate counters from pass 1 (SQ_WAVES)"""
    _validate_counter_data(pass1_counter_data, "SQ_WAVES", "pass 1")


def test_pass2_counters(pass2_counter_data):
    """Validate counters from pass 2 (GRBM_COUNT)"""
    _validate_counter_data(pass2_counter_data, "GRBM_COUNT", "pass 2")


def test_pass3_counters(pass3_counter_data):
    """Validate counters from pass 3 (GRBM_GUI_ACTIVE)"""
    _validate_counter_data(pass3_counter_data, "GRBM_GUI_ACTIVE", "pass 3")


def test_same_kernel_count_all_passes(
    pass1_counter_data, pass2_counter_data, pass3_counter_data
):
    """Verify that all passes collected data for the same kernel dispatches"""
    # Get unique dispatch IDs from every pass
    pass1_dispatch_ids = set([int(row["Dispatch_Id"]) for row in pass1_counter_data])
    pass2_dispatch_ids = set([int(row["Dispatch_Id"]) for row in pass2_counter_data])
    pass3_dispatch_ids = set([int(row["Dispatch_Id"]) for row in pass3_counter_data])

    # All passes should have collected data for the same dispatches
    assert pass1_dispatch_ids == pass2_dispatch_ids == pass3_dispatch_ids, (
        f"Passes have different dispatch IDs. Pass1: {sorted(pass1_dispatch_ids)}, "
        f"Pass2: {sorted(pass2_dispatch_ids)}, Pass3: {sorted(pass3_dispatch_ids)}"
    )


def test_counter_separation(pass1_counter_data, pass2_counter_data, pass3_counter_data):
    """Verify that counters are properly separated between passes"""
    # Get all counter names from every pass
    pass1_counters = set([row["Counter_Name"] for row in pass1_counter_data])
    pass2_counters = set([row["Counter_Name"] for row in pass2_counter_data])
    pass3_counters = set([row["Counter_Name"] for row in pass3_counter_data])

    # Verify no overlap (each counter should only be in one pass)
    assert (
        len(pass1_counters & pass2_counters) == 0
        and len(pass1_counters & pass3_counters) == 0
        and len(pass2_counters & pass3_counters) == 0
    ), (
        "Counters should not overlap between passes. "
        f"Pass1: {pass1_counters}, Pass2: {pass2_counters}, Pass3: {pass3_counters}"
    )

    # Verify we have the expected counters
    assert pass1_counters == {
        "SQ_WAVES"
    }, f"Expected only SQ_WAVES in pass 1, got {pass1_counters}"
    assert pass2_counters == {
        "GRBM_COUNT"
    }, f"Expected only GRBM_COUNT in pass 2, got {pass2_counters}"
    assert pass3_counters == {
        "GRBM_GUI_ACTIVE"
    }, f"Expected only GRBM_GUI_ACTIVE in pass 3, got {pass3_counters}"


if __name__ == "__main__":
    exit_code = pytest.main(["-x", __file__] + sys.argv[1:])
    sys.exit(exit_code)
