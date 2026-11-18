"""Where the CME schema files live.

The SBE templates (templates_FixBinary.xml) and the channel map (config.xml) are CME files and
are not stored in this repository. They are looked up in this order:
  1. $TICKSCOPE_SCHEMA_DIR
  2. <repo>/schema/                      (standalone clone)
  3. <repo>/../1_cme_data/schema/        (the original course workspace layout)
See schema/README.md for where to download them.
"""
import os
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]


def schema_dir() -> Path:
    cands = []
    if os.environ.get("TICKSCOPE_SCHEMA_DIR"):
        cands.append(Path(os.environ["TICKSCOPE_SCHEMA_DIR"]))
    cands += [REPO / "schema", REPO.parent / "1_cme_data" / "schema"]
    for c in cands:
        if (c / "templates_FixBinary.xml").exists():
            return c
    raise FileNotFoundError("CME schema not found; see schema/README.md (looked in: "
                            + ", ".join(str(c) for c in cands) + ")")


def schema_file() -> Path:
    return schema_dir() / "templates_FixBinary.xml"


def config_file() -> Path:
    return schema_dir() / "config.xml"
