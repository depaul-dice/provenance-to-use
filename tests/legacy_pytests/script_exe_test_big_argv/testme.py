#!/usr/bin/env python2

import sys
sys.path.insert(0, '..')
from cde_test_common import *

def checker_func():
  assert os.path.isfile(CDE_ROOT_DIR + '/bin/sh')

generic_test_runner(["./script_exe_test_big_argv.sh", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine"], checker_func)
