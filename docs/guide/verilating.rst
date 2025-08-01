.. Copyright 2003-2025 by Wilson Snyder.
.. SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

**********
Verilating
**********

Verilator may be used in five major ways:

* With the :vlopt:`--binary` option, Verilator will translate the design
  into an executable, via generating C++ and compiling it.  See
  :ref:`Binary, C++ and SystemC Generation`.

* With the :vlopt:`--cc` or :vlopt:`--sc` options, Verilator will translate
  the design into C++ or SystemC code, respectively.  See :ref:`Binary, C++
  and SystemC Generation`.

* With the :vlopt:`--lint-only` option, Verilator will lint the design to
  check for warnings but will not typically create any output files.

* With the :vlopt:`--xml-only` option, Verilator will create XML output
  that may be used to feed into other user-designed tools.  See
  :file:`docs/xml.rst` in the distribution.

* With the :vlopt:`-E` option, Verilator will preprocess the code according
  to IEEE preprocessing rules and write the output to standard out. This
  is useful to feed other tools and to debug how "\`define" statements are
  expanded.


.. _Binary, C++ and SystemC Generation:

Binary, C++ and SystemC Generation
==================================

Verilator will translate a SystemVerilog design into C++ with the
:vlopt:`--cc` option, or into SystemC with the :vlopt:`--sc` option.  It
will translate into C++ and compile it into an executable binary with the
:vlopt:`--binary` option.

When using these options:

#. Verilator reads the input Verilog code and determines all "top modules", that
   is, modules or programs that are not used as instances under other cells.
   If :vlopt:`--top-module` is used, then that determines the top module, and
   all other top modules are removed; otherwise a :vlopt:`MULTITOP` warning
   is given.

#. Verilator writes the C++/SystemC code to output files into the
   :vlopt:`--Mdir` option-specified directory, or defaults to "obj_dir".
   The prefix is set with :vlopt:`--prefix`, or defaults to the name of the
   top module.

#. If :vlopt:`--binary` or :vlopt:`--main` is used, Verilator creates a C++
   top wrapper to read command line arguments, create the model, and
   execute the model.

#. If :vlopt:`--binary` or :vlopt:`--exe` is used, Verilator creates
   makefiles to generate a simulation executable, otherwise, it creates
   makefiles to generate an archive (.a) containing the objects.

#. If :vlopt:`--binary` or :vlopt:`--build` is used, it calls :ref:`GNU
   Make` or :ref:`CMake` to build the model.

Once a model is built, the next step is typically for the user to run it,
see :ref:`Simulating`.


.. _Finding and Binding Modules:

Finding and Binding Modules
===========================

Verilator provides several mechanisms to find the source code containing a
module, primitive, interface, or program ("module" in this section) and
bind them to an instantiation.  These capabilities are similar to the
"Precompiling in a single-pass" use model described in IEEE 1800-2023
33.5.1, although `config` is not yet supported.

Verilator first reads all files provided on the command line and
:vlopt:`-f` files, and parses all modules within.  Each module is assigned
to the most recent library specified with :vlopt:`-work`, thus `-work liba
a.v -work libb b.v` will assign modules in `a.v` to `liba` and modules in
`b.v` to `libb`.

If a module is not defined from a file on the command-line, Verilator
attempts to find a filename constructed from the module name using
:vlopt:`-y` and `+libext`.

Binding begins with the :vlopt:`--top` module, if provided. If not provided
Verilator attempts to figure out the top module itself, and if multiple
tops result a :option:`MULTITOP` warning is issued which may be suppressed
(see details in :option:`MULTITOP`).

Verilator will attempt to bind lower unresolved instances first in the same
library name as the parent's instantiation library, and if not found search
globally across all libraries in the order modules were declared.  This
allows otherwise conflicting duplicate module names between libraries to
coexist uniquely within each library name.  When IEEE `config use` is
supported, more complicated selections will be able to be specified.


.. _Hierarchical Verilation:

Hierarchical Verilation
=======================

Large designs may take long (e.g., 10+ minutes) and huge memory (e.g., 100+
GB) to Verilate.  In hierarchical mode, the user manually selects some
large lower-level hierarchy blocks to separate from the larger design. For
example, a core may be the hierarchy block separated out of a multi-core
SoC design.

Verilator is run in hierarchical mode on the whole SoC.  Verilator will
make two models, one for the CPU hierarchy block and one for the SoC.  The
Verilated code for the SoC will automatically call the CPU Verilated model.

The current hierarchical Verilation is based on :vlopt:`--lib-create`. Each
hierarchy block is Verilated into a library. User modules of the hierarchy
blocks will see a tiny wrapper generated by :vlopt:`--lib-create`.


Usage
-----

Users need to mark one or more moderate-size modules as hierarchy block(s).
There are two ways to mark a module:

* Write :option:`/*verilator&32;hier_block*/` metacomment in HDL code.

* Add a :option:`hier_block` line in the :ref:`Verilator Control Files`.

Then pass the :vlopt:`--hierarchical` option to Verilator.

The compilation is the same as when not using hierarchical mode.

.. code-block:: bash

    make -C obj_dir -f Vtop_module_name.mk


Limitations
-----------

Hierarchy blocks have some limitations, including:

* The hierarchy block cannot be accessed using dot (.) from the upper
  module(s) or other hierarchy blocks.

* Modport cannot be used at the hierarchical block boundary.

* The simulation speed is likely not as fast as flat Verilation, in which
  all modules are globally scheduled.

* Generated clocks may not work correctly if generated in the hierarchical
  model and passed into another hierarchical model or the top module.

* Delays are not allowed in hierarchy blocks.

But, the following usage is supported:

* Nested hierarchy blocks. A hierarchy block may instantiate other
  hierarchy blocks.

* Parameterized hierarchy block. Parameters of a hierarchy block can be
  overridden using :code:`#(.param_name(value))` construct.


.. _Overlapping Verilation and Compilation:

Overlapping Verilation and Compilation
--------------------------------------

Verilator needs to run 2 + *N* times in hierarchical Verilation, where *N*
is the number of hierarchy blocks. One of the two is for the top module,
which refers to the wrappers of all other hierarchy blocks.  The second of the
two is the initial run that searches modules marked with
:option:`/*verilator&32;hier_block*/` metacomment and creates a plan and
write in :file:`{prefix}_hier.mk`.  This initial run internally invokes
other *N* + 1 runs, so you don't have to care about these *N* + 1 times of
run. The additional *N* is the Verilator run for each hierarchical block.

If ::vlopt:`-j {jobs} <-j>` option is specified, Verilation for hierarchy
blocks runs in parallel.

If :vlopt:`--build` option is specified, C++ compilation also runs as soon
as a hierarchy block is Verilated. C++ compilation and Verilation for other
hierarchy blocks run simultaneously.


Cross Compilation
=================

Verilator supports cross-compiling Verilated code.  This is generally used
to run Verilator on a Linux system and produce C++ code that is then compiled
on Windows.

Cross-compilation involves up to three different OSes.  The build system is
where you configure and compile Verilator, the host system is where you run
Verilator, and the target system is where you compile the Verilated code
and run the simulation.

Verilator requires the build and host system types to be the
same, though the target system type may be different.  To support this,
:command:`./configure` and make Verilator on the build system.  Then, run
Verilator on the host system.  Finally, the output of Verilator may be
compiled on the different target system.

To support this, none of the files that Verilator produces will reference
any configure-generated build-system-specific files, such as
:file:`config.h` (which is renamed in Verilator to :file:`config_package.h`
to reduce confusion.)  The disadvantage of this approach is that
:file:`include/verilatedos.h` must self-detect the requirements of the
target system, rather than using configure.

The target system may also require edits to the Makefiles, the simple
Makefiles produced by Verilator presume the target system is the same type
as the build system.


.. _Multithreading:

Multithreading
==============

Verilator supports multithreaded simulation models.

With :vlopt:`--threads 1 <--threads>`, the generated model is
single-threaded; however, the support libraries are multithread safe. This
allows different instantiations of the model(s) to potentially each be run
under a different thread. All threading is the responsibility of the user's
C++ testbench.

With :vlopt:`--threads {N} <--threads>`, where N is at least 2, the
generated model will be designed to run in parallel on N threads. The
thread calling eval() provides one of those threads, and the generated
model will create and manage the other N-1 threads. It's the client's
responsibility not to oversubscribe the available CPU cores. Under CPU
oversubscription, the Verilated model should not livelock nor deadlock;
however, you can expect performance to be far worse than it would be with
the proper ratio of threads and CPU cores.

The thread used for constructing a model must be the same thread that calls
:code:`eval()` into the model; this is called the "eval thread". The thread
used to perform certain global operations, such as saving and tracing, must
be done by a "main thread". In most cases, the eval thread and main thread
are the same thread (i.e. the user's top C++ testbench runs on a single
thread), but this is not required.

When making frequent use of DPI imported functions in a multithreaded
model, it may be beneficial to performance to adjust the
:vlopt:`--instr-count-dpi` option based on some experimentation. This
influences the partitioning of the model by adjusting the assumed execution
time of DPI imports.

When using :vlopt:`--trace-vcd` to perform VCD tracing, the VCD trace
construction is parallelized using the same number of threads as specified
with :vlopt:`--threads`, and is executed on the same thread pool as the model.

The :vlopt:`--trace-threads` options can be used with :vlopt:`--trace-fst`
to offload FST tracing using multiple threads. If :vlopt:`--trace-threads` is
given without :vlopt:`--threads`, then :vlopt:`--trace-threads` will imply
:vlopt:`--threads 1 <--threads>`, i.e., the support libraries will be
thread safe.

With :vlopt:`--trace-threads 0 <--trace-threads>`, trace dumps are produced
on the main thread. This again gives the highest single-thread performance.

With :vlopt:`--trace-threads {N} <--trace-threads>`, where N is at least 1,
up to N additional threads will be created and managed by the trace files
(e.g., VerilatedFstC), to offload construction of the trace dump. The main
thread will be released to proceed with execution as soon as possible, though
some main thread blocking is still necessary while capturing the
trace. FST tracing can utilize up to 2 offload threads, so there is no use
of setting :vlopt:`--trace-threads` higher than 2 at the moment.

When running a multithreaded model, the default Linux task scheduler often
works against the model by assuming short-lived threads and thus it often
schedules threads using multiple hyperthreads within the same physical
core. If there is no affinity already set, on Linux only, Verilator
attempts to set thread-to-processor affinity in a reasonable way.

For best performance, use the :command:`numactl` program to (when the
threading count fits) select unique physical cores on the same socket. The
same applies for :vlopt:`--trace-threads` as well.

As an example, if a model was Verilated with
:vlopt:`--threads 4 <--threads>`, we consult:

.. code-block:: bash

    egrep 'processor|physical id|core id' /proc/cpuinfo

To select cores 0, 1, 2, and 3 that are all located on the same socket (0)
but have different physical cores.  (Also useful is
:command:`numactl --hardware`, or :command:`lscpu`, but those don't show
hyperthreading cores.)  Then we execute:

.. code-block:: bash

    numactl -m 0 -C 0,1,2,3 -- verilated_executable_name

This will limit memory to socket 0, and threads to cores 0, 1, 2, 3,
(presumably on socket 0), optimizing performance.  Of course, this must be
adjusted if you want another simulator to use, e.g., socket 1, or if you
Verilated with a different number of threads.  To see what CPUs are
actually used, use :vlopt:`--prof-exec`.


Multithreaded Verilog and Library Support
-----------------------------------------

$display/$stop/$finish are delayed until the end of an eval() call
to maintain ordering between threads. This may result in additional tasks
completing after the $stop or $finish.

If using :vlopt:`--coverage`, the coverage routines are fully thread-safe.

If using the DPI, Verilator assumes pure DPI imports are thread-safe,
balancing performance versus safety. See :vlopt:`--threads-dpi`.

If using :vlopt:`--savable`, the save/restore classes are not multithreaded
and must be called only by the eval thread.

If using :vlopt:`--sc`, the SystemC kernel is not thread-safe; therefore,
the eval thread and main thread must be the same.

If using :vlopt:`--trace-vcd` or other trace options, the tracing classes
must be constructed and called from the main thread.

If using :vlopt:`--vpi`, since SystemVerilog VPI was not architected by
IEEE to be multithreaded, Verilator requires all VPI calls are only made
from the main thread.


.. _GNU Make:

GNU Make
========

Verilator defaults to creating GNU Make makefiles for the model.  Verilator
will call make automatically when the :vlopt:`--build` option is used.

If calling Verilator from a makefile, the :vlopt:`--MMD` option will create
a dependency file, allowing Make to only run Verilator if input Verilog
files change.

.. _CMake:

CMake
=====

Verilator can be run using CMake, which takes care of both running
Verilator and compiling the output. There is a CMake example in the
:file:`examples/` directory. The following is a minimal CMakeLists.txt that
would build the code listed in :ref:`Example C++ Execution`

.. code-block:: CMake

     project(cmake_example)
     find_package(verilator HINTS $ENV{VERILATOR_ROOT})
     add_executable(Vour sim_main.cpp)
     verilate(Vour SOURCES our.v)

:code:`find_package` will automatically find an installed copy of
Verilator, or use a local build if VERILATOR_ROOT is set.

Using CMake >= 3.12 and the Ninja generator is recommended, though other
combinations should work. To build with CMake, change to the folder
containing CMakeLists.txt and run:

.. code-block:: bash

     mkdir build
     cd build
     cmake -GNinja ..
     ninja

Or to build with your system default generator:

.. code-block:: bash

     mkdir build
     cd build
     cmake ..
     cmake --build .

If you're building the example, you should have an executable to run:

.. code-block:: bash

     ./Vour

The package sets the CMake variables verilator_FOUND, VERILATOR_ROOT,
and VERILATOR_BIN to the appropriate values and creates a verilate()
function. verilate() will automatically create custom commands to run
Verilator and add the generated C++ sources to the target specified.

Verilate in CMake
-----------------

.. code-block:: CMake

     verilate(target SOURCES source ... [TOP_MODULE top] [PREFIX name]
              [COVERAGE] [SYSTEMC]
              [TRACE_FST] [TRACE_SAIF] [TRACE_VCD] [TRACE_THREADS num]
              [INCLUDE_DIRS dir ...] [OPT_SLOW ...] [OPT_FAST ...]
              [OPT_GLOBAL ..] [DIRECTORY dir] [THREADS num]
              [VERILATOR_ARGS ...])

Lowercase and ... should be replaced with arguments; the uppercase parts
delimit the arguments and can be passed in any order or left out entirely
if optional.

verilate(target ...) can be called multiple times to add other Verilog
modules to an executable or library target.

When generating Verilated SystemC sources, you should list the
SystemC include directories and link to the SystemC libraries.

.. describe:: target

   Name of a target created by add_executable or add_library.

.. describe:: COVERAGE

   Optional. Enables coverage if present, equivalent to "VERILATOR_ARGS
   --coverage".

.. describe:: DIRECTORY

   Optional. Set the verilator output directory. It is preferable to use
   the default, which will avoid collisions with other files.

.. describe:: INCLUDE_DIRS

   Optional. Sets directories that Verilator searches (same as -y).

.. describe:: OPT_SLOW

   Optional. Set compiler options for the slow path. You may want to reduce
   the optimization level to improve compile times with large designs.

.. describe:: OPT_FAST

   Optional. Set compiler options for the fast path.

.. describe:: OPT_GLOBAL

   Optional. Set compiler options for the common runtime library used by
   Verilated models.

.. describe:: PREFIX

   Optional. Sets the Verilator output prefix. Defaults to the name of the
   first source file with a "V" prepended. It must be unique in each call
   to verilate(), so this is necessary if you build a module multiple times
   with different parameters. It must be a valid C++ identifier, i.e., it
   contains no white space and only characters A-Z, a-z, 0-9 or _.

.. describe:: SOURCES

   List of Verilog files to Verilate. You must provide at least one file.

.. describe:: SYSTEMC

   Optional. Enables SystemC mode, defaults to C++ if not specified.

   When using Accellera's SystemC with CMake support, a CMake target is
   available that simplifies the SystemC steps. This will only work if
   CMake can find the SystemC installation, and this can be configured by
   setting the CMAKE_PREFIX_PATH variable during CMake configuration.

   Don't forget to set the same C++ standard for the Verilated sources as
   the SystemC library. This can be specified using the SYSTEMC_CXX_FLAGS
   environment variable.

.. describe:: THREADS

   Optional. Enable a multithreaded model; see :vlopt:`--threads`.

.. describe:: TOP_MODULE

   Optional. Sets the name of the top module. Defaults to the name of the
   first file in the SOURCES array.

.. describe:: TRACE

   Deprecated. Same as TRACE_VCD, which should be used instead.

.. describe:: TRACE_FST

   Optional. Enables FST tracing if present, equivalent to "VERILATOR_ARGS
   --trace-fst".

.. describe:: TRACE_SAIF

   Optional. Enables SAIF tracing if present, equivalent to "VERILATOR_ARGS
   --trace-saif".

.. describe:: TRACE_THREADS

   Optional. Enable multithreaded FST trace; see :vlopt:`--trace-threads`.

.. describe:: TRACE_VCD

   Optional. Enables VCD tracing if present, equivalent to "VERILATOR_ARGS
   --trace-vcd".

.. describe:: VERILATOR_ARGS

   Optional. Extra arguments to Verilator. Do not specify :vlopt:`--Mdir`
   or :vlopt:`--prefix` here; use DIRECTORY or PREFIX.


SystemC Link in CMake
---------------------

Verilator's CMake support provides a convenience function to automatically
find and link to the SystemC library.  It can be used as:

.. code-block:: CMake

     verilator_link_systemc(target)

where target is the name of your target.

The search paths can be configured by setting some variables:

.. describe:: SYSTEMC_INCLUDE

   Sets the direct path to the SystemC includes.

.. describe:: SYSTEMC_LIBDIR

   Sets the direct path to the SystemC libraries.

.. describe:: SYSTEMC_ROOT

   Sets the installation prefix of an installed SystemC library.

.. describe:: SYSTEMC

   Sets the installation prefix of an installed SystemC library. (Same as
   SYSTEMC_ROOT).


.. _Verilation Summary Report:

Verilation Summary Report
=========================

When Verilator generates code, unless :vlopt:`--quiet-stats` is used, it
will print a report to stdout summarizing the build. For example:

.. code-block::

    - V e r i l a t i o n   R e p o r t: Verilator ....
    - Verilator: Built from 354 MB sources in 247 modules,
        into 74 MB in 89 C++ files needing 0.192 MB
    - Verilator: Walltime 26.580 s (elab=2.096, cvt=18.268,
        bld=2.100); cpu 26.548 s on 1 threads; alloced 2894.672 MB

The information in this report is:

.. describe:: "Verilator ..."

   Program version.

.. describe:: "234 MB sources"

   Characters of post-preprocessed text in all input
   Verilog and Verilator Control files in megabytes.

.. describe:: "247 modules"

   Number of interfaces/modules/classes/packages in design before
   elaboration.

.. describe:: "into 74 MB"

   Characters of output C++ code, including comments in megabytes.

.. describe:: "89 C++ files"

   Number of .cpp files created.

.. describe:: "needing 192MB"

   Verilation-time minimum-bound estimate of memory needed to run model in
   megabytes. (Expect to need significantly more.)

.. describe:: "Walltime 26.580 s"

   Real elapsed wall time for Verilation and build.

.. describe:: "elab=2.096"

   Wall time to read in files and complete elaboration.

.. describe:: "cvt=18.268"

   Wall time for Verilator to process and write output.

.. describe:: "bld=2.1"

   Wall time to compile gcc/clang (if using :vlopt:`--build`).

.. describe:: "cpu 22.548 s"

   CPU time used, total across all CPU threads.

.. describe:: "4 threads"

   Number of simultaneous threads used.

.. describe:: "alloced 123 MB"

   Total memory used during build by Verilator executable (excludes
   :vlopt:`--build` compiler's usage) in megabytes.
