#!/bin/sh

test_description='Test flux job execution service in simulated mode'

. $(dirname $0)/sharness.sh

test_under_flux 1 job

flux setattr log-stderr-level 1

skip_all_unless_have jq

RPC=${FLUX_BUILD_DIR}/t/request/rpc

job_kvsdir()    { flux job id --to=kvs $1; }
exec_eventlog() { flux kvs get -r $(job_kvsdir $1).guest.exec.eventlog; }
exec_test()     { ${jq} '.attributes.system.exec.test = {}'; }
exec_testattr() {
	${jq} --arg key "$1" --arg value $2 \
	      '.attributes.system.exec.test[$key] = $value'
}

test_expect_success 'job-exec: generate jobspec for simple test job' '
        flux jobspec srun -n1 hostname | exec_test > basic.json
'
test_expect_success 'job-exec: basic job runs in simulated mode' '
	jobid=$(flux job submit basic.json) &&
	flux job wait-event -t 1 ${jobid} start &&
	flux job wait-event -t 1 ${jobid} finish &&
	flux job wait-event -t 1 ${jobid} release &&
	flux job wait-event -t 1 ${jobid} clean
'
test_expect_success 'job-exec: guestns linked into primary' '
	#  guest key is not a link
	test_must_fail \
	  flux kvs readlink $(job_kvsdir ${jobid}).guest 2>readlink.err &&
	grep "Invalid argument" readlink.err &&
	#  gues key is a directory
	test_must_fail \
	  flux kvs get $(job_kvsdir ${jobid}).guest 2>kvsdir.err &&
	grep "Is a directory" kvsdir.err
'
test_expect_success 'job-exec: exec.eventlog exists with expected states' '
	exec_eventlog ${jobid} > eventlog.1.out &&
	head -1 eventlog.1.out | grep "init" &&
	tail -1 eventlog.1.out | grep "done"
'
test_expect_success 'job-exec: canceling job during execution works' '
	jobid=$(flux jobspec srun -t 1 hostname | \
		 exec_test | flux job submit) &&
	flux job wait-event -vt 2.5 ${jobid} start &&
	flux job cancel ${jobid} &&
	flux job wait-event -t 2.5 ${jobid} exception &&
	flux job wait-event -t 2.5 ${jobid} finish | grep status=15 &&
	flux job wait-event -t 2.5 ${jobid} release &&
	flux job wait-event -t 2.5 ${jobid} clean &&
	exec_eventlog $jobid | grep "complete" | grep "\"status\":15"
'
test_expect_success 'job-exec: mock exception during initialization' '
	flux jobspec srun hostname | \
	  exec_testattr mock_exception init > ex1.json &&
	jobid=$(flux job submit ex1.json) &&
	flux job wait-event -t 2.5 ${jobid} exception > exception.1.out &&
	test_debug "flux job eventlog ${jobid}" &&
	grep "type=\"exec\"" exception.1.out &&
	grep "mock initialization exception generated" exception.1.out &&
	flux job wait-event -qt 2.5 ${jobid} clean &&
	flux job eventlog ${jobid} > eventlog.${jobid}.out &&
	test_must_fail grep "finish" eventlog.${jobid}.out
'
test_expect_success 'job-exec: mock exception during run' '
	flux jobspec srun hostname | \
	  exec_testattr mock_exception run > ex2.json &&
	jobid=$(flux job submit ex2.json) &&
	flux job wait-event -t 2.5 ${jobid} exception > exception.2.out &&
	grep "type=\"exec\"" exception.2.out &&
	grep "mock run exception generated" exception.2.out &&
	flux job wait-event -qt 2.5 ${jobid} clean &&
	flux job eventlog ${jobid} > eventlog.${jobid}.out &&
	grep "finish status=15" eventlog.${jobid}.out
'
test_expect_success 'job-exec: simulate epilog/cleanup tasks' '
	flux jobspec srun hostname | \
	  exec_testattr cleanup_duration 0.01s > cleanup.json &&
	jobid=$(flux job submit cleanup.json) &&
	flux job wait-event -vt 2.5 ${jobid} clean &&
	exec_eventlog $jobid > exec.eventlog.$jobid &&
	grep "cleanup\.start" exec.eventlog.$jobid &&
	grep "cleanup\.finish" exec.eventlog.$jobid
'
test_expect_success 'job-exec: R with invalid expiration raises exception' '
	flux module unload job-exec &&
	jobid=$(flux job submit basic.json) &&
	key=$(flux job id --to=kvs $jobid).R &&
	R=$(flux kvs get --wait $key | jq -c ".execution.expiration = -1.") &&
	flux kvs put ${key}=${R} &&
	flux module load job-exec &&
	flux job wait-event -v $jobid exception
'
#
# XXX: Trying to generate an exception during cleanup is racy, however,
#  there is not currently another way to do this until we have a *real*
#  epilog script which could be triggered by events. If this test becomes
#  a problem, we can disable it until that time.
#
test_expect_success 'job-exec: exception during cleanup' '
	flux jobspec srun hostname | \
	  exec_testattr cleanup_duration 1s > cleanup-long.json &&
	jobid=$(flux job submit cleanup-long.json) &&
	flux job wait-event -vt 2.5 ${jobid} finish &&
	flux job cancel ${jobid} &&
	flux job wait-event -t 10 ${jobid} clean &&
	exec_eventlog $jobid > exec.eventlog.$jobid &&
	grep "cleanup\.finish" exec.eventlog.$jobid
'
test_expect_success 'start request with empty payload fails with EPROTO(71)' '
	${RPC} job-exec.start 71 </dev/null
'
test_done
