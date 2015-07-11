# Lists the tests
#
# Copyright (C) 2015 Andrew Cagney <cagney@gnu.org>
# 
# This program is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the
# Free Software Foundation; either version 2 of the License, or (at your
# option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
# 
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
# or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
# for more details.

import os
import sys
import re
import filecmp
import subprocess
# import difflib: doesn't ignore blank lines
from fab import logutil

# The TestResult objects are almost, but not quite, an enum.  They
# carry around additional result details.
class TestResult:

    def __init__(self, test, errors={}):
        self.errors = errors
        self.test = test
        test.logger.debug("%s: %s", test.name, self)

    def __str__(self):
        if self.errors:
            return "%s %s" % (self.value, self.errors)
        else:
            return self.value

class TestIncomplete(TestResult):
    value = "incomplete"
    passed = None
class TestFailed(TestResult):
    value = "failed"
    passed = False
class TestPassed(TestResult):
    value = "passed"
    passed = True


# Compare two files; how can "diff -N -w -B" be done in python?
def fuzzy_diff(logger, l, r):
    logger.debug("fuzzy compare '%s' '%s'", l, r)
    if filecmp.cmp(l, r):
        # fast path
        return False
    command = "diff -N -w -B '%s' '%s'" % (l, r)
    status, output = subprocess.getstatusoutput(command)
    if status:
        return True
    return False


# Add error WHAT from WHO to the errors dictionary.
def add_error(errors, what, who=None):
    if not what in errors:
        errors[what] = set()
    errors[what].add(who)


def re_check(errors, what, line, who):
    if re.search(what, line):
        add_error(errors, what, who)


class Test:

    def __init__(self, directory, kind, expected_result):
        self.logger = logutil.getLogger(__name__)
        # basics
        self.kind = kind
        self.expected_result = expected_result
        # name must match the directory's basename; since directory
        # could be ".", need to first convert it to an absolute path.
        self.name = os.path.basename(os.path.abspath(directory))
        self.full_name = "test " + self.name
        # avoid "." as a directory, construct the sub-directory paths
        # using the parent's directory (don't abspath or relpath).
        self.directory = os.path.relpath(directory)
        if self.directory == ".":
            self.directory = os.path.join("..", self.name)
        self.output_directory = os.path.join(self.directory, "OUTPUT")
        self.result_file = os.path.join(self.output_directory, "RESULT")
        # will be filled in later
        self.domains = None
        self.initiators = None

    def __str__(self):
        return self.full_name

    def files_with_suffix(self, suffix):
        s = set()
        for file in os.listdir(self.directory):
            host, init, t = file.partition(suffix)
            if init and not t:
                s.add(host)
        return s

    def result(self, baseline=None):

        if not os.path.exists(self.output_directory):
            self.logger.debug("output dir missing: %s", self.output_directory)
            return None

        errors = {}

        # XXX: Look at self.result_file?

        # For every {domain}.console.txt file there should be an empty
        # OUTPUT/{domain}.console.txt file and corresponding empty
        # OUTPUT/{domain}.console.diff file.
        finished = True
        for domain in self.domain_names():

            master_txt = os.path.join(self.directory,
                                      domain + ".console.txt")
            self.logger.debug("console.txt? %s", master_txt)
            if not os.path.exists(master_txt):
                self.logger.debug("%s does not expect results", domain)
                continue

            # Since OUTPUT/{domain}.console.txt is generated from
            # OUTPUT/{domain}.console.verbose.txt after the test has
            # finished it indicates a test's completion.
            console_txt = os.path.join(self.output_directory,
                                       domain + ".console.txt")
            self.logger.debug("console.txt? %s", console_txt)
            if not os.path.exists(console_txt):
                add_error(errors, "missing console.txt", domain)
                finished = False
                continue

            # Generated by sanitize.sh for now.
            console_diff = os.path.join(self.output_directory,
                                        domain + ".console.diff")
            self.logger.debug("console.diff? %s", console_diff)
            diff_ok = True
            if os.path.exists(console_diff):
                if os.stat(console_diff).st_size:
                    add_error(errors, "nonempty diff", domain)
                    diff_ok = False
            else:
                # This just forces us to do the diff ourselves.
                if fuzzy_diff(self.logger, master_txt, console_txt):
                    add_error(errors, "console different", domain)
                    diff_ok = False

            # Perhaps the baseline gives a better diff?
            if not diff_ok and baseline:
                if not self.name in baseline:
                    add_error(errors, "baseline absent", None)
                else:
                    base = baseline[self.name]
                    self.logger.debug("compare baseline? %s", base)
                    baseline_txt = os.path.join(base.output_directory,
                                                domain + ".console.txt")
                    if not os.path.exists(baseline_txt):
                        add_error(errors, "baseline missing", domain)
                    elif fuzzy_diff(self.logger, baseline_txt, console_txt):
                        add_error(errors, "baseline different", domain)

        # Check the console and pluto logs for markers indicating that
        # there was a crash or other unexpected behaviour.

        for domain in self.domain_names():
            console_log = os.path.join(self.output_directory,
                                       domain + ".console.txt")
            self.logger.debug("console.txt? %s", console_log)
            if os.path.exists(console_log):
                console_file = open(console_log, "r")
                for line in console_file:
                    re_check(errors, "^CORE_FOUND", line, domain)
                    re_check(errors, "SEGFAULT", line, domain)
                    re_check(errors, "GPFAULT", line, domain)
                console_file.close()

        for domain in self.domain_names():
            pluto_log = os.path.join(self.output_directory, domain + ".pluto.log")
            self.logger.debug("pluto.log? %s", pluto_log)
            if os.path.exists(pluto_log):
                pluto_file = open(pluto_log, "r")
                for line in pluto_file:
                    re_check(errors, "ASSERTION FAILED", line, domain)
                    re_check(errors, "EXPECTATION FAILED", line, domain)
                pluto_file.close()

        # The final result
        if not finished:
            return TestIncomplete(self, errors=errors)
        elif errors:
            return TestFailed(self, errors=errors)
        else:
            return TestPassed(self)

    def domain_names(self):
        if not self.domains:
            self.domains = self.files_with_suffix("init.sh")
        return self.domains

    def initiator_names(self):
        if not self.initiators:
            self.initiators = self.files_with_suffix("run.sh")
        return self.initiators


class TestIterator:

    def __init__(self, testsuite):
        self.logger = logutil.getLogger(__name__)
        self.testsuite = testsuite
        self.test_list = open(testsuite.testlist, 'r')

    def __next__(self):
        for line in self.test_list:
            line = line.strip()
            self.logger.debug("input: %s", line)
            if not line:
                self.logger.debug("ignore blank line")
                continue
            if line[0] == '#':
                self.logger.debug("ignore comment line")
                continue
            try:
                kind, name, expected_result = line.split()
                test = Test(directory=os.path.join(self.testsuite.directory, name),
                            kind=kind, expected_result=expected_result)
            except ValueError:
                # This is serious
                self.logger.error("****** malformed line: %s", line)
                continue
            self.logger.debug("test directory: %s", test.directory)
            if not os.path.exists(test.directory):
                # This is serious
                self.logger.error("****** invalid test %s: directory not found: %s",
                              test.name, test.directory)
                continue
            return test

        self.test_list.close()
        raise StopIteration


class Testsuite:

    def __init__(self, directory, testlist):
        self.directory = directory
        self.testlist = testlist

    def __iter__(self):
        return TestIterator(self)


def add_arguments(parser):

    group = parser.add_argument_group("Test arguments",
                                      "Options for selecting the tests to run")
    group.add_argument("--test-name", default="",
                       type=re.compile, metavar="REGULAR-EXPRESSION",
                       help=("Select tests with name matching %(metavar)s"
                             " (default: '%(default)s')"))
    group.add_argument("--test-kind", default="kvmplutotest",
                       type=re.compile, metavar="REGULAR-EXPRESSION",
                       help=("Select tests with kind matching %(metavar)s"
                             " (default: '%(default)s')"))
    group.add_argument("--test-expected-result", default="good",
                       type=re.compile, metavar="REGULAR-EXPRESSION",
                       help=("Select tests with expected-result matching %(metavar)s"
                             " (default: '%(default)s')"))
    group.add_argument("--test-exclude", default="",
                       type=re.compile, metavar="REGULAR-EXPRESSION",
                       help=("Exclude tests that match %(metavar)s"
                             " (default: '%(default)s')"))


def log_arguments(logger, args):
    logger.info("Testsuite arguments:")
    logger.info("  test-kind: '%s'" , args.test_kind.pattern)
    logger.info("  test-name: '%s'" , args.test_name.pattern)
    logger.info("  test-result: '%s'" , args.test_expected_result.pattern)
    logger.info("  test-exclude: '%s'" , args.test_exclude.pattern)


TESTLIST = "TESTLIST"


def load(logger, directory):
    """Load the testsuite (TESTLIST) found in DIRECTORY"""

    testlist = os.path.join(directory, TESTLIST)
    if os.path.exists(testlist):
        logger.debug("loading testsuite in '%s'", directory)
        return Testsuite(directory, testlist)

    return None


def load_testsuite_or_tests(logger, directories):

    # Is it a single testsuite directory?
    if len(directories) == 1:
        tests = load(logger, directories[0])
        if tests:
            logger.debug("tests loaded from testsuite '%s'", tests.directory)
            return tests

    # Presumably this is a list of directories, each specifying one
    # test.
    tests = []
    for directory in directories:
        # XXX: Should figure out kind by looking at directory.  Should
        # validate that it is a test directory.
        logger.debug("test loaded from directory '%s'", directory)
        tests.append(Test(directory=directory, kind="kvmplutotest",
                          expected_result="good"))

    return tests


def ignore(test, args):

    """Identify tests that should be ignored due to filters

    The ignore reason is returned.

    This is different to SKIP where a test isn't run because it has
    been run before.

    """

    if args.test_kind.pattern and not args.test_kind.search(test.kind):
        return "kind '%s' does not match '%s'" % (test.kind, args.test_kind.pattern)
    if args.test_name.pattern and not args.test_name.search(test.name):
        return "name '%s' does not match '%s'" % (test.name, args.test_name.pattern)
    if args.test_expected_result.pattern and not args.test_expected_result.search(test.expected_result):
        return "expected test result '%s' does not match '%s'" % (test.expected_result, args.test_expected_result.pattern)

    if args.test_exclude.pattern:
        if args.test_exclude.search(test.kind) or \
           args.test_exclude.search(test.name) or \
           args.test_exclude.search(test.expected_result):
            return "matches exclude regular expression: %s" % args.test_exclude.pattern

    return None
