# CMAKE generated file: DO NOT EDIT!
# Generated by "Unix Makefiles" Generator, CMake Version 2.8

#=============================================================================
# Special targets provided by cmake.

# Disable implicit rules so canonical targets will work.
.SUFFIXES:

# Remove some rules from gmake that .SUFFIXES does not remove.
SUFFIXES =

.SUFFIXES: .hpux_make_needs_suffix_list

# Suppress display of executed commands.
$(VERBOSE).SILENT:

# A target that is always out of date.
cmake_force:
.PHONY : cmake_force

#=============================================================================
# Set environment variables for the build.

# The shell in which to execute make rules.
SHELL = /bin/sh

# The CMake executable.
CMAKE_COMMAND = /usr/bin/cmake

# The command to remove a file.
RM = /usr/bin/cmake -E remove -f

# Escaping for special characters.
EQUALS = =

# The program to use to edit the cache.
CMAKE_EDIT_COMMAND = /usr/bin/ccmake

# The top-level source directory on which CMake was run.
CMAKE_SOURCE_DIR = /home/andrew/Dropbox/School/rcos/dr-vis/dynamorio

# The top-level build directory on which CMake was run.
CMAKE_BINARY_DIR = /home/andrew/Dropbox/School/rcos/dr-vis/dynamorio/build

# Utility rule file for api_headers.

# Include the progress variables for this target.
include CMakeFiles/api_headers.dir/progress.make

CMakeFiles/api_headers: include/dr_app.h
CMakeFiles/api_headers: include/dr_api.h

include/dr_app.h: ../core/lib/genapi.pl
include/dr_app.h: ../core/perscache.h
include/dr_app.h: ../core/rct.h
include/dr_app.h: ../core/heap.h
include/dr_app.h: ../core/vmareas.h
include/dr_app.h: ../core/hotpatch.h
include/dr_app.h: ../core/hashtablex.h
include/dr_app.h: ../core/instrlist.h
include/dr_app.h: ../core/fcache.h
include/dr_app.h: ../core/perfctr.h
include/dr_app.h: ../core/dispatch.h
include/dr_app.h: ../core/optionsx.h
include/dr_app.h: ../core/config.h
include/dr_app.h: ../core/iox.h
include/dr_app.h: ../core/stats.h
include/dr_app.h: ../core/link.h
include/dr_app.h: ../core/native_exec.h
include/dr_app.h: ../core/emit.h
include/dr_app.h: ../core/hashtable.h
include/dr_app.h: ../core/options.h
include/dr_app.h: ../core/moduledb.h
include/dr_app.h: ../core/os_shared.h
include/dr_app.h: ../core/fragment.h
include/dr_app.h: ../core/module_shared.h
include/dr_app.h: ../core/monitor.h
include/dr_app.h: ../core/globals.h
include/dr_app.h: ../core/nudge.h
include/dr_app.h: ../core/utils.h
include/dr_app.h: ../core/synch.h
include/dr_app.h: ../core/x86/sideline.h
include/dr_app.h: ../core/x86/decode_fast.h
include/dr_app.h: ../core/x86/instr_inline.h
include/dr_app.h: ../core/x86/proc.h
include/dr_app.h: ../core/x86/steal_reg.h
include/dr_app.h: ../core/x86/arch.h
include/dr_app.h: ../core/x86/arch_exports.h
include/dr_app.h: ../core/x86/disassemble.h
include/dr_app.h: ../core/x86/instrument.h
include/dr_app.h: ../core/x86/instr.h
include/dr_app.h: ../core/x86/instr_create.h
include/dr_app.h: ../core/x86/loadtoconst.h
include/dr_app.h: ../core/x86/decode.h
include/dr_app.h: ../core/lib/globals_shared.h
include/dr_app.h: ../core/lib/kstatsx.h
include/dr_app.h: ../core/lib/dr_api.h
include/dr_app.h: ../core/lib/mcxtx.h
include/dr_app.h: ../core/lib/dr_config.h
include/dr_app.h: ../core/lib/dr_inject.h
include/dr_app.h: ../core/lib/dr_app.h
include/dr_app.h: ../core/lib/statsx.h
include/dr_app.h: ../core/lib/dr_stats.h
include/dr_app.h: ../core/lib/hotpatch_interface.h
include/dr_app.h: ../core/win32/syscallx.h
include/dr_app.h: ../core/win32/gbop.h
include/dr_app.h: ../core/win32/inject_shared.h
include/dr_app.h: ../core/win32/diagnost.h
include/dr_app.h: ../core/win32/ntdll.h
include/dr_app.h: ../core/win32/aslr.h
include/dr_app.h: ../core/win32/module.h
include/dr_app.h: ../core/win32/drmarker.h
include/dr_app.h: ../core/win32/os_exports.h
include/dr_app.h: ../core/win32/os_private.h
include/dr_app.h: ../core/win32/ntdll_types.h
include/dr_app.h: ../core/unix/module.h
include/dr_app.h: ../core/unix/os_exports.h
include/dr_app.h: ../core/unix/os_private.h
include/dr_app.h: ../core/unix/symtab.h
include/dr_app.h: ../libutil/parser.h
include/dr_app.h: ../libutil/share.h
include/dr_app.h: ../libutil/elm.h
include/dr_app.h: ../libutil/processes.h
include/dr_app.h: ../libutil/mfapi.h
include/dr_app.h: ../libutil/config.h
include/dr_app.h: ../libutil/our_tchar.h
include/dr_app.h: ../libutil/services.h
include/dr_app.h: ../libutil/policy.h
include/dr_app.h: ../libutil/tests.h
include/dr_app.h: ../libutil/utils.h
include/dr_app.h: ../core/hotpatch.c
include/dr_app.h: ../ext/drx/drx.h
include/dr_app.h: ../ext/drcontainers/drtable.h
include/dr_app.h: ../ext/drcontainers/containers_private.h
include/dr_app.h: ../ext/drcontainers/hashtable.h
include/dr_app.h: ../ext/drcontainers/drvector.h
include/dr_app.h: ../ext/drutil/drutil.h
include/dr_app.h: ../ext/drmgr/drmgr.h
include/dr_app.h: ../ext/drwrap/drwrap.h
include/dr_app.h: ../ext/drsyms/drsyms_private.h
include/dr_app.h: ../ext/drsyms/dbghelp_imports.h
include/dr_app.h: ../ext/drsyms/demangle.h
include/dr_app.h: ../ext/drsyms/drsyms.h
include/dr_app.h: ../ext/drsyms/drsyms_obj.h
include/dr_app.h: ../ext/drgui/drgui_main_window.h
include/dr_app.h: ../ext/drgui/drgui_tool_interface.h
include/dr_app.h: ../ext/drgui/drgui_options_window.h
include/dr_app.h: ../ext/drgui/drgui_options_interface.h
	$(CMAKE_COMMAND) -E cmake_progress_report /home/andrew/Dropbox/School/rcos/dr-vis/dynamorio/build/CMakeFiles $(CMAKE_PROGRESS_1)
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --blue --bold "Generating include/dr_app.h"
	cd /home/andrew/Dropbox/School/rcos/dr-vis/dynamorio/core && /usr/bin/perl /home/andrew/Dropbox/School/rcos/dr-vis/dynamorio/core/lib/genapi.pl -core /home/andrew/Dropbox/School/rcos/dr-vis/dynamorio/core -header /home/andrew/Dropbox/School/rcos/dr-vis/dynamorio/build/include "-DINTERNAL -DDEBUG -DDRSTATS_DEMO -DX64 -DLINUX -DUNIX -DCLIENT_INTERFACE -DAPP_EXPORTS -DKSTATS -DHAVE_FVISIBILITY -DDR_DO_NOT_DEFINE_uint -DDR_DO_NOT_DEFINE_ushort -DDR__Bool_EXISTS -DX86 -DASSEMBLE_WITH_GAS -DHAVE_PROC_MAPS -DHAVE_TLS -DHAVE_SIGALTSTACK -DUSE_VISIBILITY_ATTRIBUTES -DDYNAMORIO_IR_EXPORTS -DCUSTOM_TRACES -DCLIENT_SIDELINE -DDR_APP_EXPORTS -DDEBUG_MEMORY -DSTACK_GUARD_PAGE -DHEAP_ACCOUNTING -DDEADLOCK_AVOIDANCE -DSHARING_STUDY -DHASHTABLE_STATISTICS -DAPI_EXPORT_ONLY"

api_headers: CMakeFiles/api_headers
api_headers: include/dr_app.h
api_headers: CMakeFiles/api_headers.dir/build.make
.PHONY : api_headers

# Rule to build all files generated by this target.
CMakeFiles/api_headers.dir/build: api_headers
.PHONY : CMakeFiles/api_headers.dir/build

CMakeFiles/api_headers.dir/clean:
	$(CMAKE_COMMAND) -P CMakeFiles/api_headers.dir/cmake_clean.cmake
.PHONY : CMakeFiles/api_headers.dir/clean

CMakeFiles/api_headers.dir/depend:
	cd /home/andrew/Dropbox/School/rcos/dr-vis/dynamorio/build && $(CMAKE_COMMAND) -E cmake_depends "Unix Makefiles" /home/andrew/Dropbox/School/rcos/dr-vis/dynamorio /home/andrew/Dropbox/School/rcos/dr-vis/dynamorio /home/andrew/Dropbox/School/rcos/dr-vis/dynamorio/build /home/andrew/Dropbox/School/rcos/dr-vis/dynamorio/build /home/andrew/Dropbox/School/rcos/dr-vis/dynamorio/build/CMakeFiles/api_headers.dir/DependInfo.cmake --color=$(COLOR)
.PHONY : CMakeFiles/api_headers.dir/depend

