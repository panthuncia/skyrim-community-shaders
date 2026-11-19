"""cs-test's entry point: python tools/cs-test/cs_test.py <command> (README.md beside this file)."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from cstest.cli import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
