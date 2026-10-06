"""adX - a music production environment whose project format is a text file.

The Python side owns window layout, tool logic and interaction; every piece of
real work happens in the engine behind ``adx_engine`` (FINAL_PLAN 2.1). ``app/``
is the frontend source root and ``adx`` is the importable package - an import name
of ``app`` would collide with half the ecosystem (phase_0.md 3.6).
"""

from __future__ import annotations

from importlib.metadata import PackageNotFoundError
from importlib.metadata import version as _distribution_version

__all__ = ["__version__", "engine_version", "git_sha"]


def _resolve_version() -> str:
    """Return the installed distribution's version.

    This is the half of the version check that can actually drift: the wheel's
    metadata comes from scikit-build-core reading the ``project(adx VERSION ...)``
    line, while the engine's comes from that same line via ``configure_file``.
    ``test_version_agrees`` compares the two (phase_0.md 3.6).

    The fallback covers running straight out of a source tree with the extension
    on ``sys.path`` but nothing pip-installed; in that case there is no metadata to
    disagree with.
    """
    try:
        return _distribution_version("adx")
    except PackageNotFoundError:
        from adx import engine_bridge

        return engine_bridge.engine_version()


__version__: str = _resolve_version()


def engine_version() -> str:
    """Return the version string reported by the native engine."""
    from adx import engine_bridge

    return engine_bridge.engine_version()


def git_sha() -> str:
    """Return the commit the native engine was configured from, or ``"unknown"``."""
    from adx import engine_bridge

    return engine_bridge.git_sha()
