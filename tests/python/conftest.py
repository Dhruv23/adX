"""Shared fixtures for the Python suite."""

from __future__ import annotations

import pathlib

import pytest

_REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]


@pytest.fixture(scope="session")
def repo_root() -> pathlib.Path:
    """The repository root.

    Phase 2 onward needs it to reach ``docs/examples/*.adx``, which are permanent
    parser regression fixtures (FINAL_PLAN 6).
    """
    return _REPO_ROOT
