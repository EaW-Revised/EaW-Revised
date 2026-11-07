"""Small statistics kit for the async-AI quality comparison (no third-party packages).

Everything here is deterministic: the permutation/split noise floor uses an explicit seeded generator.
"""
from __future__ import annotations

import itertools
import math
import random
from typing import Iterable, Sequence


def percentile(values: Sequence[float], percent: float) -> float:
    """Nearest-rank percentile (the rule the C++ tick profile uses); NaN for no data."""
    if not values:
        return math.nan
    ordered = sorted(values)
    rank = max(1, math.ceil(len(ordered) * percent / 100.0))
    return float(ordered[min(rank, len(ordered)) - 1])


def median(values: Sequence[float]) -> float:
    if not values:
        return math.nan
    ordered = sorted(values)
    middle = len(ordered) // 2
    if len(ordered) % 2:
        return float(ordered[middle])
    return (ordered[middle - 1] + ordered[middle]) / 2.0


def mean(values: Sequence[float]) -> float:
    return sum(values) / len(values) if values else math.nan


def relative_shift(reference: Sequence[float], other: Sequence[float]) -> float:
    """(median(other) - median(reference)) / max(|median(reference)|, 1): a fraction, sign kept.

    The denominator floor of 1 keeps a zero reference median (rare events) from exploding the ratio: a median
    move from 0 to 1 reads as 100 %, a count of 0 against 0 as 0 %.
    """
    if not reference or not other:
        return math.nan
    base = median(reference)
    return (median(other) - base) / max(abs(base), 1.0)


def ks_statistic(first: Sequence[float], second: Sequence[float]) -> float:
    """Two-sample Kolmogorov-Smirnov D (maximum distance between the empirical CDFs)."""
    if not first or not second:
        return math.nan
    a, b = sorted(first), sorted(second)
    i = j = 0
    d = 0.0
    while i < len(a) and j < len(b):
        x = min(a[i], b[j])
        while i < len(a) and a[i] <= x:
            i += 1
        while j < len(b) and b[j] <= x:
            j += 1
        d = max(d, abs(i / len(a) - j / len(b)))
    return d


def ks_pvalue(d: float, n: int, m: int) -> float:
    """Asymptotic two-sided p-value with the small-sample correction (Stephens)."""
    if math.isnan(d) or n < 1 or m < 1:
        return math.nan
    if d <= 0.0:
        return 1.0
    effective = n * m / (n + m)
    lam = (math.sqrt(effective) + 0.12 + 0.11 / math.sqrt(effective)) * d
    if lam < 0.2:
        return 1.0
    total = 0.0
    for k in range(1, 101):
        term = 2.0 * (-1) ** (k - 1) * math.exp(-2.0 * k * k * lam * lam)
        total += term
        if abs(term) < 1e-12:
            break
    return min(1.0, max(0.0, total))


def ks_test(first: Sequence[float], second: Sequence[float]) -> tuple[float, float]:
    d = ks_statistic(first, second)
    return d, ks_pvalue(d, len(first), len(second))


def holm(pvalues: Sequence[float]) -> list[float]:
    """Holm-Bonferroni adjusted p-values in input order; NaN stays NaN and does not count toward m."""
    indexed = [(p, i) for i, p in enumerate(pvalues) if not math.isnan(p)]
    indexed.sort()
    adjusted = [math.nan] * len(pvalues)
    running = 0.0
    m = len(indexed)
    for rank, (p, i) in enumerate(indexed):
        running = max(running, min(1.0, (m - rank) * p))
        adjusted[i] = running
    return adjusted


def wilson(successes: int, total: int, z: float = 1.959964) -> tuple[float, float]:
    """Wilson score interval for a proportion."""
    if total <= 0:
        return (math.nan, math.nan)
    p = successes / total
    denominator = 1 + z * z / total
    centre = (p + z * z / (2 * total)) / denominator
    half = z * math.sqrt(p * (1 - p) / total + z * z / (4 * total * total)) / denominator
    return (max(0.0, centre - half), min(1.0, centre + half))


def poisson_interval(count: int, alpha: float = 0.05) -> tuple[float, float]:
    """Exact (Garwood) interval for a Poisson mean, by bisection on the cumulative distribution."""
    def cdf(k: int, mu: float) -> float:
        term = math.exp(-mu)
        total = term
        for i in range(1, k + 1):
            term *= mu / i
            total += term
        return min(1.0, total)

    def bisect(rising: bool, f) -> float:
        # `f(mu)` is monotone in mu; find the mu where it crosses alpha / 2.
        low, high = 0.0, 10.0 + 4.0 * (count + 1)
        for _ in range(200):
            mid = (low + high) / 2.0
            above = f(mid) > alpha / 2.0
            if above == rising:
                high = mid
            else:
                low = mid
        return (low + high) / 2.0

    # Lower: P(X >= count | mu) rises with mu. Upper: P(X <= count | mu) falls with mu.
    lower = 0.0 if count == 0 else bisect(True, lambda mu: 1.0 - cdf(count - 1, mu))
    upper = bisect(False, lambda mu: cdf(count, mu))
    return (lower, upper)


def balanced_splits(count: int, group: int, limit: int, seed: int) -> list[tuple[tuple[int, ...], tuple[int, ...]]]:
    """Disjoint index pairs (first, second) of size `group` out of `count` items.

    All C(count, group) first groups with their complementary second group when that is at most `limit`
    (second = the next `group` items not in first); otherwise `limit` seeded random draws.
    """
    indices = list(range(count))
    pairs: list[tuple[tuple[int, ...], tuple[int, ...]]] = []
    if group < 1 or 2 * group > count:
        return pairs
    total = math.comb(count, group)
    if total <= limit:
        for first in itertools.combinations(indices, group):
            rest = [i for i in indices if i not in first]
            pairs.append((first, tuple(rest[:group])))
        return pairs
    rng = random.Random(seed)
    for _ in range(limit):
        draw = rng.sample(indices, 2 * group)
        pairs.append((tuple(sorted(draw[:group])), tuple(sorted(draw[group:]))))
    return pairs


def quantile_of(values: Iterable[float], fraction: float) -> float:
    ordered = sorted(v for v in values if not math.isnan(v))
    if not ordered:
        return math.nan
    return float(ordered[min(len(ordered) - 1, max(0, math.ceil(len(ordered) * fraction) - 1))])
