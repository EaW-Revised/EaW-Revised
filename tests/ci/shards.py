"""Split one test file across several CTest entries that run side by side.

A test module sets `load_tests = shards.load_tests`. With EAWR_TEST_SHARD=i/n in the environment
only every n-th test from the i-th (in the loader's order) runs, so the n entries together run
every test once; without it the module runs everything, as before. tests/ci/CMakeLists.txt
registers a sharded file once per shard (eawr_add_sharded_test)."""

import os
import unittest


def _flatten(suite):
    for item in suite:
        if isinstance(item, unittest.TestSuite):
            yield from _flatten(item)
        else:
            yield item


def load_tests(loader, tests, pattern):
    shard = os.environ.get("EAWR_TEST_SHARD", "")
    if not shard:
        return tests
    index, _, count = shard.partition("/")
    index, count = int(index), int(count)
    if not 0 <= index < count:
        raise ValueError(f"EAWR_TEST_SHARD={shard}: expected i/n with 0 <= i < n")
    return unittest.TestSuite(test for position, test in enumerate(_flatten(tests)) if position % count == index)
