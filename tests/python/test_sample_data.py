"""The sample dataset tool (P8.4): building the archive, and checking it before and after unpacking."""

import io
import json
import shutil
import tarfile
from pathlib import Path

import pytest

from research.data import sample
from research.data.sample import NAME, get, make, sha256_file, unpack, verify

FIXTURE = Path(__file__).resolve().parents[2] / "tests" / "sim" / "data" / "btcusdt_2026-11-05_0200_1m.bse"


def _dataset(root: Path) -> Path:
    d = root / NAME
    d.mkdir(parents=True)
    shutil.copy(FIXTURE, d / "BTCUSDT.bse")
    (d / "manifest.json").write_text(json.dumps({"files": [
        {"symbol": "BTCUSDT", "file": "BTCUSDT.bse", "sha256": sha256_file(d / "BTCUSDT.bse"),
         "bytes": (d / "BTCUSDT.bse").stat().st_size}]}))
    return d


def _archive(root: Path, src: Path, extra: tuple[str, bytes] | None = None) -> Path:
    out = root / "a.tar.gz"
    with tarfile.open(out, "w:gz") as tar:
        tar.add(src, arcname=NAME)
        if extra:
            info = tarfile.TarInfo(extra[0])
            info.size = len(extra[1])
            tar.addfile(info, io.BytesIO(extra[1]))
    return out


def test_unpack_checks_the_archive_and_the_files(tmp_path):
    archive = _archive(tmp_path, _dataset(tmp_path / "src"))
    with pytest.raises(ValueError, match="SHA-256"):
        unpack(archive, tmp_path / "out1", expected_sha256="0" * 64)
    path = unpack(archive, tmp_path / "out2", expected_sha256=sha256_file(archive))
    assert verify(path)["files"][0]["symbol"] == "BTCUSDT"
    with open(path / "BTCUSDT.bse", "r+b") as f:  # a changed byte
        f.seek(1000)
        b = f.read(1)
        f.seek(1000)
        f.write(bytes([b[0] ^ 1]))
    with pytest.raises(ValueError, match="manifest"):
        verify(path)


def test_unpack_refuses_entries_outside_the_dataset(tmp_path):
    src = _dataset(tmp_path / "src")
    for name in ("../evil.txt", "other/x.txt"):
        archive = _archive(tmp_path, src, (name, b"x"))
        with pytest.raises(ValueError, match="unexpected entry"):
            unpack(archive, tmp_path / "out", expected_sha256=None)
    assert not (tmp_path / "evil.txt").exists()


def test_get_keeps_a_verified_copy_and_uses_a_local_archive(tmp_path, monkeypatch):
    archive = _archive(tmp_path, _dataset(tmp_path / "src"))
    monkeypatch.setattr(sample, "ARCHIVE_SHA256", sha256_file(archive))
    dest = tmp_path / "data"
    path = get(dest, archive)
    assert verify(path) and archive.exists()   # a given archive is not deleted
    assert get(dest) == path                   # already there and verified: no download


@pytest.mark.skipif(not any(p.exists() for p in sample.BS_SIM), reason="bs_sim isn't built")
def test_make_is_reproducible_and_unpacks(tmp_path):
    src = tmp_path / "packed"
    src.mkdir()
    shutil.copy(FIXTURE, src / "BTCUSDT.bse")
    (src / "manifest.json").write_text(json.dumps({
        "dataset": "fixture", "format": {"name": "bse", "version": 1, "record_size": 40}, "schema_version": 1,
        "venue": "binance", "market": "spot", "qc_verdict": "pass",
        "files": [{"symbol": "BTCUSDT", "file": "BTCUSDT.bse", "sha256": sha256_file(src / "BTCUSDT.bse")}]}))
    a1, h1 = make(src, "2026-11-05T02:00:10Z", "2026-11-05T02:00:40Z", tmp_path / "o1")
    a2, h2 = make(src, "2026-11-05T02:00:10Z", "2026-11-05T02:00:40Z", tmp_path / "o2")
    assert h1 == h2 == sha256_file(a1)
    m = verify(unpack(a1, tmp_path / "out", expected_sha256=h1))
    assert m["files"][0]["record_count"] > 0 and m["range"]["from"] == "2026-11-05T02:00:10Z"
