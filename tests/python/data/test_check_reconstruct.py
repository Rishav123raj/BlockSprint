"""V0 checker (research.sim.check_reconstruct): a correct dump passes, any difference fails."""

from conftest import DAY1, SEC, SYMBOL, write_golden_stream

from research.data.normalize import normalize_symbol
from research.data.refbook import RefBook, iter_rows
from research.data.schema import FLAG_LAST_IN_BATCH, partition_dir
from research.sim.check_reconstruct import check


def oracle_dump(book_path, path, top_n=2):
    """What bs_sim reconstruct --every 1 writes, produced here by the oracle itself."""
    lines = ["batch,ts_local,valid,side,level,px,qty"]
    book, batches = RefBook(), 0
    for row in iter_rows(book_path):
        book.apply(row)
        if row.flags & FLAG_LAST_IN_BATCH:
            batches += 1
            if not book.valid:
                lines.append(f"{batches},{row.ts_local},0,,,,")
                continue
            top = book.top(top_n)
            for side, levels in ((0, top["bids"]), (1, top["asks"])):
                lines += [f"{batches},{row.ts_local},1,{side},{i},{px},{qty}" for i, (px, qty) in enumerate(levels)]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def test_v0_checker(raw, tmp_path):
    write_golden_stream(raw, DAY1 + 3600 * SEC)
    raw.close()
    out = tmp_path / "norm"
    normalize_symbol(raw.root, out, SYMBOL)
    dump = tmp_path / "dump.csv"
    oracle_dump(partition_dir(out, SYMBOL, "2026-01-01") / "book.parquet", dump)
    assert check(dump, out, SYMBOL, ["2026-01-01"], 2) == 0

    lines = dump.read_text(encoding="utf-8").splitlines()
    fields = lines[-1].split(",")
    fields[-1] = str(int(fields[-1]) + 1)  # one lot off in the last checkpoint
    bad = tmp_path / "bad.csv"
    bad.write_text("\n".join(lines[:-1] + [",".join(fields)]) + "\n", encoding="utf-8")
    assert check(bad, out, SYMBOL, ["2026-01-01"], 2) == 1
