#!/usr/bin/env python3
# DESCRIPTION: Verilator: Verilog Test driver/expect definition
#
# Copyright 2024 by Wilson Snyder. This program is free software; you
# can redistribute it and/or modify it under the terms of either the GNU
# Lesser General Public License Version 3 or the Perl Artistic License
# Version 2.0.
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import vltest_bootstrap

test.scenarios('vlt_all')
test.top_filename = "t/t_unopt_array.v"

test.compile(v_flags2=["--trace-vcd --output-split 1 --output-split-cfuncs 1 -Wno-UNOPTFLAT"])

test.execute()

test.passes()
