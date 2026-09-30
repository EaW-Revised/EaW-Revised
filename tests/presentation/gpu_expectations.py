"""Per-GPU expectations for graphical tests (docs/worker-offload.md#per-gpu-expectations).

Captures from different GPUs and drivers differ in low bits, so a graphical
test must not pin a raw capture hash that was recorded on one machine. In
order of preference a test:

1. compares captures made in the same run (repeatability, on/off controls);
2. pins CPU-side facts (scene, stream and mask hashes, counts);
3. compares decoded pixels against a reference with a declared tolerance
   (``assert_images_close``);
4. keys an exact expectation by GPU profile (``expected_for_gpu``), with one
   entry per profile it has been recorded on.

The profile comes from EAWR_GPU_PROFILE: tools/rig/Invoke-RigSuite.ps1 sets it
from the GPU host's entry in config/gpu-hosts.json (``nvidia-gtx970`` on the
GTX 970 rig, ``nvidia-rtx4070-laptop`` on the RTX 4070 laptop, ``amd-rx7900xtx`` on the
RX 7900 XTX workstation); a run started by hand sets it itself. A test whose
profile has no entry is skipped with the reason, not failed.
"""

from __future__ import annotations

import os
import pathlib
import unittest
from typing import Mapping, TypeVar

PROFILE_ENV = "EAWR_GPU_PROFILE"
KNOWN_PROFILES = ("amd-rx7900xtx", "nvidia-gtx970", "nvidia-rtx4070-laptop", "llvmpipe")
T = TypeVar("T")


def gpu_profile() -> str:
    return os.environ.get(PROFILE_ENV, "").strip()


def expected_for_gpu(test: unittest.TestCase, table: Mapping[str, T], what: str) -> T:
    """The expectation recorded for this run's GPU profile, or skip the test."""
    unknown = set(table) - set(KNOWN_PROFILES)
    if unknown:
        raise ValueError(f"unknown GPU profile(s) in the {what} table: {sorted(unknown)}")
    profile = gpu_profile()
    if not profile:
        test.skipTest(f"set {PROFILE_ENV} to compare the {what} (one of {', '.join(KNOWN_PROFILES)})")
    if profile not in table:
        test.skipTest(f"no {what} recorded for GPU profile {profile}")
    return table[profile]


def image_difference(first: pathlib.Path, second: pathlib.Path) -> dict:
    """Decoded-pixel difference of two same-sized PNGs: per pixel, the largest channel delta."""
    from PIL import Image, ImageChops
    with Image.open(first) as a, Image.open(second) as b:
        if a.size != b.size:
            raise ValueError(f"image sizes differ: {a.size} and {b.size}")
        channels = ImageChops.difference(a.convert("RGBA"), b.convert("RGBA")).split()
    per_pixel = channels[0]
    for channel in channels[1:]:
        per_pixel = ImageChops.lighter(per_pixel, channel)
    histogram = per_pixel.histogram()
    pixels = per_pixel.size[0] * per_pixel.size[1]
    differing = pixels - histogram[0]
    largest = max((level for level, count in enumerate(histogram) if count), default=0)
    total = sum(level * count for level, count in enumerate(histogram))
    return {"pixels": pixels, "differing_pixels": differing, "differing_fraction": differing / pixels,
            "max_channel_delta": largest, "mean_delta_of_differing": total / differing if differing else 0.0}


def assert_images_close(test: unittest.TestCase, actual: pathlib.Path, reference: pathlib.Path, *,
                        max_channel_delta: int, max_differing_fraction: float) -> dict:
    """Fail when a pixel moves by more than max_channel_delta levels in any channel, or when more
    than max_differing_fraction of the pixels differ at all."""
    difference = image_difference(actual, reference)
    test.assertLessEqual(difference["max_channel_delta"], max_channel_delta,
                         f"{actual.name} vs {reference.name}: {difference}")
    test.assertLessEqual(difference["differing_fraction"], max_differing_fraction,
                         f"{actual.name} vs {reference.name}: {difference}")
    return difference
