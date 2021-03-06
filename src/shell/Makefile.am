AM_CFLAGS = \
	$(WARNING_CFLAGS) \
	$(CODE_COVERAGE_CFLAGS)
AM_LDFLAGS = \
	$(CODE_COVERAGE_LIBS)

AM_CPPFLAGS = \
	-I$(top_srcdir) \
	-I$(top_srcdir)/src/include \
	-I$(top_builddir)/src/common/libflux \
	$(ZMQ_CFLAGS) $(VALGRIND_CFLAGS) \
	$(LUA_INCLUDE) \
	$(HWLOC_CFLAGS)

shellrcdir = \
	$(fluxrcdir)/shell
shellluadir = \
	$(shellrcdir)/lua.d

dist_shellrc_SCRIPTS = \
	initrc.lua

dist_shelllua_SCRIPTS = \
	lua.d/mvapich.lua \
	lua.d/intel_mpi.lua \
	lua.d/openmpi.lua \
	lua.d/spectrum.lua

noinst_LTLIBRARIES = \
	libshell.la \
	libmpir.la

libshell_la_SOURCES = \
	plugstack.c \
	plugstack.h \
	jobspec.c \
	jobspec.h \
	eventlogger.c \
	eventlogger.h \
	rcalc.c \
	rcalc.h

libmpir_la_SOURCES = \
	mpir/rangelist.c \
	mpir/rangelist.h \
	mpir/nodelist.c \
	mpir/nodelist.h \
	mpir/proctable.c \
	mpir/proctable.h


fluxinclude_HEADERS = \
	shell.h

fluxlibexec_PROGRAMS = flux-shell

flux_shell_SOURCES = \
	shell.c \
	shell.h \
	internal.h \
	rc.c \
	rc.h \
	builtins.c \
	builtins.h \
	info.c \
	info.h \
	task.c \
	task.h \
	log.c \
	log.h \
	events.c \
	events.h \
	pmi.c \
	input.c \
	output.c \
	svc.c \
	svc.h \
	kill.c \
	signals.c \
	affinity.c \
	gpubind.c \
	evlog.c \
	pty.c \
	batch.c \
	mpir/mpir.c \
	mpir/ptrace.c

flux_shell_LDADD = \
	$(builddir)/libshell.la \
	$(builddir)/libmpir.la \
	$(top_builddir)/src/bindings/lua/libfluxlua.la \
	$(top_builddir)/src/common/libflux-core.la \
	$(top_builddir)/src/common/libpmi/libpmi_server.la \
	$(top_builddir)/src/common/libflux-internal.la \
	$(top_builddir)/src/common/libflux-optparse.la \
	$(top_builddir)/src/common/libterminus/libterminus.la \
	$(top_builddir)/src/common/libutil/libutil.la \
	$(LUA_LIB) \
	$(HWLOC_LIBS)

flux_shell_LDFLAGS = \
	-export-dynamic \
	-Wl,--version-script=$(srcdir)/flux-shell.map

EXTRA_DIST = \
	flux-shell.map

TESTS = \
	test_jobspec.t \
	test_plugstack.t \
	mpir/test_rangelist.t \
	mpir/test_nodelist.t \
	mpir/test_proctable.t

test_ldadd = \
	$(top_builddir)/src/common/libflux-core.la \
	$(top_builddir)/src/common/libpmi/libpmi_server.la \
	$(top_builddir)/src/common/libflux-internal.la \
	$(top_builddir)/src/common/libtap/libtap.la

test_ldflags = \
	-no-install

test_cppflags = \
        -I$(top_srcdir)/src/common/libtap \
        $(AM_CPPFLAGS)


check_PROGRAMS = $(TESTS)

check_LTLIBRARIES = \
	test/a/plugin.la \
	test/b/plugin.la \
	test/c/plugin.la

TEST_EXTENSIONS = .t
T_LOG_DRIVER = env AM_TAP_AWK='$(AWK)' $(SHELL) \
       $(top_srcdir)/config/tap-driver.sh

test_jobspec_t_SOURCES = test/jobspec.c
test_jobspec_t_CPPFLAGS = $(test_cppflags)
test_jobspec_t_LDADD = \
	$(builddir)/libshell.la \
	$(test_ldadd)
test_jobspec_t_LDFLAGS = \
	$(test_ldflags)

test_plugstack_t_SOURCES = plugstack.c test/plugstack.c
test_plugstack_t_CPPFLAGS = $(test_cppflags) -DPLUGSTACK_STANDALONE
test_plugstack_t_LDADD = \
	$(builddir)/libshell.la \
	$(test_ldadd)
test_plugstack_t_LDFLAGS = \
	$(test_ldflags)

test_a_plugin_la_SOURCES = test/plugin_test.c
test_a_plugin_la_CPPFLAGS = $(test_cppflags) -DTEST_PLUGIN_RESULT=\"A\"
test_a_plugin_la_LDFLAGS = -module -rpath /nowhere $(test_ldflags)

test_b_plugin_la_SOURCES = test/plugin_test.c
test_b_plugin_la_CPPFLAGS = $(test_cppflags) -DTEST_PLUGIN_RESULT=\"B\"
test_b_plugin_la_LDFLAGS = -module -rpath /nowhere $(test_ldflags)

test_c_plugin_la_SOURCES = test/plugin_test.c
test_c_plugin_la_CPPFLAGS = $(test_cppflags) -DTEST_PLUGIN_RESULT=\"C\"
test_c_plugin_la_LDFLAGS = -module -rpath /nowhere $(test_ldflags)

mpir_test_rangelist_t_SOURCES = mpir/test/rangelist.c
mpir_test_rangelist_t_CPPFLAGS = $(test_cppflags)
mpir_test_rangelist_t_LDADD = \
	$(builddir)/libmpir.la \
	$(test_ldadd)
mpir_test_rangelist_t_LDFLAGS = \
	$(test_ldflags)

mpir_test_nodelist_t_SOURCES = mpir/test/nodelist.c
mpir_test_nodelist_t_CPPFLAGS = $(test_cppflags)
mpir_test_nodelist_t_LDADD = \
	$(builddir)/libmpir.la \
	$(test_ldadd)
mpir_test_nodelist_t_LDFLAGS = \
	$(test_ldflags)

mpir_test_proctable_t_SOURCES = mpir/test/proctable.c
mpir_test_proctable_t_CPPFLAGS = $(test_cppflags)
mpir_test_proctable_t_LDADD = \
	$(builddir)/libmpir.la \
	$(test_ldadd)
mpir_test_proctable_t_LDFLAGS = \
	$(test_ldflags)
