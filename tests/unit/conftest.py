import pathlib
import sys

# Make the external component importable as a top-level `vitohome` package.
ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "components"))
