#!/usr/bin/env python

###############################################################
# Copyright 2014 Lawrence Livermore National Security, LLC
# (c.f. AUTHORS, NOTICE.LLNS, COPYING)
#
# This file is part of the Flux resource manager framework.
# For details, see https://github.com/flux-framework.
#
# SPDX-License-Identifier: LGPL-3.0
###############################################################

import os
import errno
import sys
import json
import yaml

import unittest

import flux
from flux import job
from flux.job import ffi


def __flux_size():
    return 1


def yaml_to_json(s):
    obj = yaml.load(s)
    return json.dumps(obj, separators=(",", ":"))


class TestJob(unittest.TestCase):
    @classmethod
    def setUpClass(self):
        self.fh = flux.Flux()

        jobspec_dir = os.path.abspath(
            os.path.join(os.environ["FLUX_SOURCE_DIR"], "t", "jobspec")
        )

        # get a valid jobspec
        basic_jobspec_fname = os.path.join(jobspec_dir, "valid", "basic_v1.yaml")
        with open(basic_jobspec_fname, "rb") as infile:
            basic_yaml = infile.read()
        self.jobspec = yaml_to_json(basic_yaml)

    def test_00_null_submit(self):
        with self.assertRaises(EnvironmentError) as error:
            job.submit(ffi.NULL, self.jobspec)
        self.assertEqual(error.exception.errno, errno.EINVAL)

        with self.assertRaises(EnvironmentError) as error:
            job.submit_get_id(ffi.NULL)
        self.assertEqual(error.exception.errno, errno.EINVAL)

        with self.assertRaises(EnvironmentError) as error:
            job.submit(self.fh, ffi.NULL)
        self.assertEqual(error.exception.errno, errno.EINVAL)

    def test_01_nonstring_submit(self):
        with self.assertRaises(TypeError):
            job.submit(self.fh, 0)

    def test_02_sync_submit(self):
        jobid = job.submit(self.fh, self.jobspec)
        self.assertGreater(jobid, 0)


if __name__ == "__main__":
    from subflux import rerun_under_flux

    if rerun_under_flux(size=__flux_size(), personality="job"):
        from pycotap import TAPTestRunner

        unittest.main(testRunner=TAPTestRunner())
