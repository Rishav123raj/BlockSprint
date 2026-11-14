"""The sample dataset (P8.4): one packed hour of BTCUSDT and ETHUSDT for the quickstart and examples.

``get`` downloads it from the repository's GitHub release ``sample-data-v1``, checks the archive's SHA-256 against
the one pinned here, unpacks it into ``data/sample/`` and checks every ``.bse`` against its manifest::

    uv run python -m research.data.sample get

``make`` builds the archive from a packed dataset (what was published; for reproducibility)::

    uv run python -m research.data.sample make --dataset data/packed/binance_spot_btc_eth_1110 \\
        --from 2026-11-10T02:00:00Z --to 2026-11-10T03:00:00Z --out dist/sample

The dataset is a normal packed dataset directory (``manifest.json`` plus one ``.bse`` per symbol), so it goes
straight into a run config: ``{"dataset": "data/sample/binance_spot_btc_eth_sample", "symbol": "BTCUSDT", ...}``.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import subprocess
import tarfile
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
NAME = "binance_spot_btc_eth_sample"
ARCHIVE = "blocksprint_sample_btc_eth_2026-11-10_0200.tar.gz"
RELEASE_URL = f"https://github.com/Rishav123raj/BlockSprint/releases/download/sample-data-v1/{ARCHIVE}"
ARCHIVE_SHA256 = "246b1a32db939036f0df97db648bfcc17056985305f87a10a6f316d49ee2fff4"
DEFAULT_DEST = ROOT / "data" / "sample"
BS_SIM = [ROOT / "build" / "Release" / "bs_sim.exe", ROOT / "build" / "bs_sim"]


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def verify(dataset: Path) -> dict[str, Any]:
    """Checks every file in the dataset's manifest against its size and SHA-256; returns the manifest."""
    manifest = json.loads((dataset / "manifest.json").read_text(encoding="utf-8"))
    for f in manifest["files"]:
        p = dataset / f["file"]
        if not p.is_file():
            raise FileNotFoundError(f"{p}: missing")
        if p.stat().st_size != f["bytes"] or sha256_file(p) != f["sha256"]:
            raise ValueError(f"{p}: doesn't match the manifest (size or SHA-256)")
    return manifest


def unpack(archive: Path, dest: Path, expected_sha256: str | None = ARCHIVE_SHA256) -> Path:
    """Checks the archive's SHA-256, then unpacks the dataset directory into dest and verifies it."""
    if expected_sha256 and sha256_file(archive) != expected_sha256:
        raise ValueError(f"{archive}: SHA-256 doesn't match the published sample")
    dest.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive, "r:gz") as tar:
        for m in tar.getmembers():
            parts = Path(m.name).parts
            if not (m.isfile() or m.isdir()) or m.name.startswith("/") or ".." in parts or parts[0] != NAME:
                raise ValueError(f"{archive}: unexpected entry {m.name!r}")
        tar.extractall(dest, filter="data")
    verify(dest / NAME)
    return dest / NAME


def get(dest: Path = DEFAULT_DEST, archive: Path | None = None) -> Path:
    """Downloads (unless archive is given) and unpacks the sample; a verified copy already there is kept."""
    target = dest / NAME
    if (target / "manifest.json").exists():
        try:
            verify(target)
            return target
        except (OSError, ValueError):
            pass
    if archive is None:
        import requests

        archive = dest / ARCHIVE
        dest.mkdir(parents=True, exist_ok=True)
        with requests.get(RELEASE_URL, stream=True, timeout=60) as r:
            r.raise_for_status()
            with open(archive, "wb") as f:
                for chunk in r.iter_content(1 << 20):
                    f.write(chunk)
    out = unpack(archive, dest, ARCHIVE_SHA256)
    if archive.parent == dest:
        archive.unlink()
    return out


def _bs_sim() -> Path:
    for p in BS_SIM:
        if p.exists():
            return p
    raise FileNotFoundError("bs_sim isn't built (cmake --build build --config Release)")


def make(dataset: Path, from_ts: str, to_ts: str, out: Path) -> tuple[Path, str]:
    """Slices every symbol of a packed dataset to [from, to) and writes the archive; returns it and its SHA-256."""
    src = json.loads((dataset / "manifest.json").read_text(encoding="utf-8"))
    work = out / NAME
    work.mkdir(parents=True, exist_ok=True)
    files = []
    for f in src["files"]:
        target = work / f["file"]
        r = subprocess.run([str(_bs_sim()), "slice", str(dataset / f["file"]), "--from", from_ts, "--to", to_ts,
                            "--out", str(target)], check=True, capture_output=True, text=True)
        records = int(r.stdout.split(": ", 1)[1].split()[0])  # "<path>: N records, ..."
        files.append({"symbol": f["symbol"], "file": f["file"], "sha256": sha256_file(target),
                      "bytes": target.stat().st_size, "record_count": records,
                      "sliced_from": {"dataset": src["dataset"], "file_sha256": f["sha256"]}})
    # No creation time: the same input gives the same archive, byte for byte
    manifest = {"dataset": NAME, "format": src["format"], "schema_version": src["schema_version"], "venue": src["venue"],
                "market": src["market"], "symbols": [f["symbol"] for f in files],
                "range": {"from": from_ts, "to": to_ts}, "qc_verdict": src["qc_verdict"], "files": files}
    (work / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    archive = out / ARCHIVE
    # Fixed metadata so the same input gives the same archive
    with open(archive, "wb") as raw:
        import gzip

        with gzip.GzipFile(fileobj=raw, mode="wb", compresslevel=9, mtime=0, filename="") as gz:
            with tarfile.open(fileobj=gz, mode="w", format=tarfile.PAX_FORMAT) as tar:
                for p in [work] + sorted(work.iterdir()):
                    info = tar.gettarinfo(str(p), arcname=str(p.relative_to(out)).replace("\\", "/"))
                    info.mtime, info.uid, info.gid, info.uname, info.gname = 0, 0, 0, "", ""
                    info.mode = 0o755 if p.is_dir() else 0o644
                    if p.is_dir():
                        tar.addfile(info)
                    else:
                        with open(p, "rb") as fh:
                            tar.addfile(info, io.BufferedReader(fh))
    return archive, sha256_file(archive)


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("get", help="download, check and unpack the sample into data/sample/")
    g.add_argument("--dest", default=str(DEFAULT_DEST))
    g.add_argument("--archive", help="a local copy of the archive instead of downloading it")
    m = sub.add_parser("make", help="build the archive from a packed dataset")
    m.add_argument("--dataset", required=True)
    m.add_argument("--from", dest="from_ts", required=True)
    m.add_argument("--to", dest="to_ts", required=True)
    m.add_argument("--out", required=True)
    args = p.parse_args(argv)
    if args.cmd == "get":
        path = get(Path(args.dest), Path(args.archive) if args.archive else None)
        manifest = verify(path)
        print(f"{path}: {', '.join(manifest['symbols'])}, {manifest['range']['from']} to {manifest['range']['to']}")
    else:
        archive, digest = make(Path(args.dataset), args.from_ts, args.to_ts, Path(args.out))
        print(f"{archive}: {archive.stat().st_size / 2**20:.1f} MB, sha256 {digest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
