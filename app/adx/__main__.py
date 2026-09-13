"""``python -m adx`` - prints the app and engine versions and exits 0."""

from __future__ import annotations

import sys

from adx.cli import main

if __name__ == "__main__":
    sys.exit(main())
