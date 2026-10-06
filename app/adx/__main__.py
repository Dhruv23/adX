"""``python -m adx`` - the application; ``python -m adx <command>`` - the CLI (cli.py)."""

from __future__ import annotations

import sys

from adx.cli import main

if __name__ == "__main__":
    sys.exit(main())
