"""CLI: run a measurement job (see lkit/jobs.py). Plugins usually wrap this to set their paths."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from lkit.jobs import main  # noqa: E402

if __name__ == "__main__":
    main()
