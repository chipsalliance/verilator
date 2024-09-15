#!/usr/bin/env python3
# DESCRIPTION: Verilator: Verilog Test driver/expect definition
#
# Copyright 2024 by Wilson Snyder. This program is free software; you
# can redistribute it and/or modify it under the terms of either the GNU
# Lesser General Public License Version 3 or the Perl Artistic License
# Version 2.0.
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import vltest_bootstrap

test.scenarios('vlt')
test.top_filename = "t/t_opt_localize_max_size.v"

test.compile(verilator_flags2=["--stats --localize-max-size 1"])

test.execute()

# Value must differ from that in t_opt_localize_max_size.py
test.file_grep(test.stats, r'Optimizations, Vars localized\s+(\d+)', 4)

test.passes()