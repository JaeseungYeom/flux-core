#!/bin/sh
#
test_description='Test flux-shell initrc.lua implementation'

. `dirname $0`/sharness.sh

FLUX_SHELL="${FLUX_BUILD_DIR}/src/shell/flux-shell"

INITRC_TESTDIR="${SHARNESS_TEST_SRCDIR}/shell/initrc"
INITRC_PLUGINPATH="${SHARNESS_TEST_DIRECTORY}/shell/plugins/.libs"

# test initrc files need to be able to find fluxometer.lua:
export LUA_PATH="$LUA_PATH;${SHARNESS_TEST_DIRECTORY}/?.lua"

test_expect_success 'flux-shell: initrc: generate 1-task jobspec and matching R' '
	flux jobspec srun -N1 -n1 echo Hi >j1 &&
	cat >R1 <<-EOT
	{"version": 1, "execution":{ "R_lite":[
		{ "children": { "core": "0,1" }, "rank": "0" }
        ]}}
	EOT
'

test_expect_success 'flux-shell: initrc: loading missing initrc fails' '
	test_expect_code 1 ${FLUX_SHELL} -v -s -r 0 -j j1 -R R1 \
		--initrc=nosuchfile 0
'
test_expect_success 'flux-shell: initrc: specifying initrc of /dev/null works' '
	${FLUX_SHELL} -v -s -r 0 -j j1 -R R1 \
		--initrc=/dev/null 0 > devnull.log 2>&1 &&
	test_debug "cat devnull.log" &&
	grep "Loading /dev/null" devnull.log
'
test_expect_success HAVE_JQ 'flux-shell: initrc: bad initrc in jobspec fails' '
	flux jobspec srun -N1 -n1 echo Hi \
	    | jq ".attributes.system.shell.options.initrc = \"nosuchfile\"" \
	    > j2 &&
	test_expect_code 1 ${FLUX_SHELL} -v -s -r 0 -j j2 -R R1 0
'
test_expect_success HAVE_JQ 'flux-shell: initrc: in jobspec works' '
	name=ok &&
	cat >${name}.lua <<-EOT &&
	    print ("jobspec initrc OK")
	EOT
	flux jobspec srun -N1 -n1 echo Hi \
	    | jq ".attributes.system.shell.options.initrc = \"${name}.lua\"" \
	    > j3 &&
	${FLUX_SHELL} -v -s -r 0 -j j3 -R R1 0 > ${name}.log 2>&1 &&
	test_debug "cat ${name}.log" &&
	grep "jobspec initrc OK" ${name}.log
'
test_expect_success 'flux-shell: initrc: failed initrc causes termination' '
	name=failed &&
	cat >${name}.lua <<-EOT &&
	=
	EOT
	test_expect_code 1 ${FLUX_SHELL} -v -s -r 0 -j j1 -R R1 \
		--initrc=${name}.lua 0 > ${name}.log 2>&1 &&
	test_debug "cat ${name}.log" &&
	grep "unexpected symbol" ${name}.log
'
test_expect_success 'flux-shell: initrc: access task in non-task context fails' '
	name=invalid-access &&
	cat >${name}.lua <<-EOT &&
	    print (task.info)
	EOT
	test_expect_code 1 ${FLUX_SHELL} -v -s -r 0 -j j1 -R R1 \
		--initrc=${name}.lua 0 > ${name}.log 2>&1 &&
	test_debug "cat ${name}.log" &&
	grep "attempt to access task outside of task context" ${name}.log
'
test_expect_success 'flux-shell: initrc: bad plugin.register usage fails' '
	name=bad-register &&
	cat >${name}.lua <<-EOT &&
	    plugin.register {}
	EOT
	test_expect_code 1 ${FLUX_SHELL} -v -s -r 0 -j j1 -R R1 \
		--initrc=${name}.lua 0 > ${name}.log 2>&1 &&
	test_debug "cat ${name}.log" &&
	grep "required handlers table missing" ${name}.log
'
test_expect_success 'flux-shell: initrc: assignment to shell method fails' '
	name=bad-assign &&
	cat >${name}.lua <<-EOT &&
	    shell.getenv = "my data"
	EOT
	test_expect_code 1 ${FLUX_SHELL} -v -s -r 0 -j j1 -R R1 \
		--initrc=${name}.lua 0 > ${name}.log 2>&1 &&
	test_debug "cat ${name}.log" &&
	grep " attempt to set read-only field" ${name}.log
'

test_expect_success 'flux-shell: initrc: return false from plugin aborts shell' '
	name=failed-return &&
	cat >${name}.lua <<-EOT &&
	    plugin.register { 
              name = "$name",
              handlers = {
		{ topic="*", fn = function () return false end }
              }
	    }
	EOT
	test_expect_code 1 ${FLUX_SHELL} -v -s -r 0 -j j1 -R R1 \
		--initrc=${name}.lua 0 > ${name}.log 2>&1 &&
	test_debug "cat ${name}.log" &&
	grep "plugin.*: shell.init failed" ${name}.log
'
test_expect_success HAVE_JQ 'flux-shell: initrc: add test options to jobspec' "
	cat j1 | jq '.attributes.system.shell.options.test = \"test\"' \
		> j.initrc
"

for initrc in ${INITRC_TESTDIR}/tests/*.lua; do
    test_expect_success "flux-shell: initrc: runtest $(basename $initrc)" '
	${FLUX_SHELL} -v -s -r 0 -j j.initrc -R R1 --initrc=${initrc} 0
    '
done

test_expect_success 'flux-shell: initrc: loads single .so successfully' '
	name=conftest-success &&
	cat >${name}.lua <<-EOT &&
        plugin.searchpath = "${INITRC_PLUGINPATH}"
	plugin.load { file = "dummy.so", conf = { result = 0 } }
	EOT
	test_expect_code 0 ${FLUX_SHELL} -v -s -r 0 -j j1 -R R1 \
		--initrc=${name}.lua 0 > ${name}.log 2>&1 &&
	test_debug "cat ${name}.log" &&
	grep "dummy: OK result=0" ${name}.log
'
test_expect_success 'flux-shell: initrc: script fails on plugin.load failure' '
	name=conftest-failure &&
	cat >${name}.lua <<-EOT &&
        plugin.searchpath = "${INITRC_PLUGINPATH}"
	plugin.load { file = "dummy.so", conf = { result = -1 } }
	EOT
	test_expect_code 1 ${FLUX_SHELL} -v -s -r 0 -j j1 -R R1 \
		--initrc=${name}.lua 0 > ${name}.log 2>&1 &&
	test_debug "cat ${name}.log" &&
	grep "dummy: OK result=-1" ${name}.log
'
test_expect_success 'flux-shell: initrc: plugin pattern nomatch not fatal' '
	name=nomatch-noprob &&
	cat >${name}.lua <<-EOT &&
	plugin.load { file = "nofile*.so" }
	EOT
	test_expect_code 0 ${FLUX_SHELL} -v -s -r 0 -j j1 -R R1 \
		--initrc=${name}.lua 0 > ${name}.log 2>&1 &&
	test_debug "cat ${name}.log" &&
	grep "0: Hi" ${name}.log
'
test_expect_success 'flux-shell: initrc: plugin nonpattern nomatch fatal' '
	name=nomatch-prob &&
	cat >${name}.lua <<-EOT &&
	plugin.load { file = "nofile.so" }
	EOT
	test_expect_code 1 ${FLUX_SHELL} -v -s -r 0 -j j1 -R R1 \
		--initrc=${name}.lua 0 > ${name}.log 2>&1 &&
	test_debug "cat ${name}.log" &&
	grep "nofile.so: File not found" ${name}.log
'
test_expect_success 'flux-shell: initrc: plugin.load passes conf to plugin' '
	name=conftest &&
	cat >${name}.lua <<-EOT &&
        plugin.searchpath = "${INITRC_PLUGINPATH}"
	plugin.load { file = "conftest.so",
                      conf = { keys = { "one", "two", "three" },
                               one = "foo",
                               two = "two",
                               three = "bar" }
	            }
	EOT
	${FLUX_SHELL} -v -s -r 0 -j j1 -R R1 \
		--initrc=${name}.lua 0 > ${name}.log 2>&1 &&
	test_debug "cat ${name}.log" &&
	grep "conftest: one=foo" ${name}.log &&
	grep "conftest: two=two" ${name}.log &&
	grep "conftest: three=bar" ${name}.log
'
test_expect_success 'flux-shell: initrc: load invalid args plugins' '
	name=invalid-args &&
	cat >${name}.lua <<-EOT &&
        plugin.searchpath = "${INITRC_PLUGINPATH}"
	plugin.load { file = "invalid-args.so" }
	EOT
	${FLUX_SHELL} -v -s -r 0 -j j1 -R R1 \
		--initrc=${name}.lua 0
'
test_expect_success HAVE_JQ 'flux-shell: initrc: load getopt plugin' '
	name=getopt &&
	cat >${name}.lua <<-EOF &&
	plugin.searchpath = "${INITRC_PLUGINPATH}"
	plugin.load { file = "getopt.so" }
	EOF
	cat j1 | jq ".attributes.system.shell.options.test = 1" >j.${name} &&
	${FLUX_SHELL} -v -s -r 0 -j j.${name} -R R1 --initrc=${name}.lua 0 \
		> ${name}.log 2>&1 &&
	test_debug "cat ${name}.log"
'
test_expect_success 'flux-shell: initrc: shell log functions available' '
	name=shell.log &&
	cat >${name}.lua <<-EOF &&
	shell.log ("test: shell.log")
	shell.debug ("test: debug")

	shell.log_error ("test: error")
	EOF
	${FLUX_SHELL} -v -s -r 0 -j j1 -R R1 --initrc=${name}.lua 0 \
		> ${name}.log 2>&1 &&
	grep "INFO: ${name}.lua:1: test: shell.log" ${name}.log &&
	grep "DEBUG: ${name}.lua:2: test: debug" ${name}.log &&
	grep "ERROR: ${name}.lua:4: test: error" ${name}.log
'
test_expect_success 'flux-shell: initrc: shell.die function works' '
	name=shell.die &&
	cat >${name}.lua <<-EOF &&
        -- shell.die test
	shell.die ("test: shell.die")
	EOF
	test_expect_code 1 \
	    ${FLUX_SHELL} -v -s -r 0 -j j1 -R R1 --initrc=${name}.lua 0 \
		> ${name}.log 2>&1 &&
	grep "FATAL: ${name}.lua:2: test: shell.die" ${name}.log
'

test_done
