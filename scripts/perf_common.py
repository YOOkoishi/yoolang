#!/usr/bin/env python3

"""Side-effect-free helpers shared by performance frontends."""

from __future__ import annotations

import math
from pathlib import Path
from statistics import median as _statistics_median
from typing import Iterable


def normalize_output(text: str) -> str:
    """Match the historical SysY performance output normalization."""

    return text.replace("\r\n", "\n").strip()


def read_sysy_expected_output(case: Path) -> tuple[str, int] | None:
    """Read the sibling .out file using compare_perf.py's exit-code convention."""

    candidate = case.with_suffix(".out")
    if not candidate.exists():
        return None

    text = normalize_output(candidate.read_text(errors="replace"))
    if not text:
        return "", 0

    lines = text.splitlines()
    last = lines[-1].strip()
    if last.isdigit() or (last.startswith("-") and last[1:].isdigit()):
        return normalize_output("\n".join(lines[:-1])), int(last)
    return text, 0


def positive_geomean(values: Iterable[float]) -> float | None:
    """Return the geometric mean, ignoring non-positive values as before."""

    positive = [float(value) for value in values if float(value) > 0.0]
    if not positive:
        return None
    return math.exp(sum(math.log(value) for value in positive) / len(positive))


def strict_median(values: Iterable[float]) -> float:
    """Return a median and reject an empty sample set."""

    samples = [float(value) for value in values]
    if not samples:
        raise ValueError("cannot take the median of an empty sample set")
    return float(_statistics_median(samples))


def relative_mad(values: Iterable[float]) -> float:
    """Median absolute deviation divided by the median (lower is steadier)."""

    samples = [float(value) for value in values]
    center = strict_median(samples)
    if center <= 0.0:
        raise ValueError("relative MAD requires a positive median")
    return strict_median(abs(value - center) for value in samples) / center
