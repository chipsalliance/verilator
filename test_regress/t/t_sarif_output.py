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
test.top_filename = "t/t_sarif.v"

test.lint(
    verilator_flags2=['-Wno-fatal', '--diagnostics-sarif-output', test.obj_dir + "/my.sarif"])

test.file_grep(test.obj_dir + "/my.sarif", "t_sarif.v")

test.passes()
