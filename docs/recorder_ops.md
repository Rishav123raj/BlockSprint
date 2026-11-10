# Market-Data Recorder: Operations Guide

The recorder ([research/data/recorder.py](../research/data/recorder.py)) captures Binance spot **diff depth** (`<sym>@depth@100ms`), **every trade** (`<sym>@trade`) and **REST depth snapshots** into a raw archive. Every day it isn't running is a day of order-book history that can't be backtested later, so it should run continuously.

## Run it

```powershell
uv sync
uv run python -m research.data.recorder --symbols BTCUSDT ETHUSDT --out data/raw
```

| Option | Default | Meaning |
| :--- | :--- | :--- |
| `--symbols` | (required) | Binance symbols, checked against `exchangeInfo` at startup |
| `--out` | `data/raw` | Archive root (git-ignored) |
| `--snapshot-interval` | `600` | Seconds between REST depth snapshots (one is also taken on every new connection) |
| `--rollover-hours` | `23` | Replace the WebSocket connection before Binance's 24 h disconnect |
| `--overlap-seconds` | `30` | Old and new connections both record during a rollover, so nothing is missed |
| `--duration` | none | Stop after N seconds (testing) |
| `--log-file` | `logs/recorder.log` | Log file (also printed to the console) |

Stop with `Ctrl+C`: open files are finalized and connections are closed cleanly.

## Check the data

```powershell
uv run python -m research.data.inspect_raw --root data/raw --symbols BTCUSDT ETHUSDT
```

Per symbol and connection it reports depth-sequence gaps (`U != previous u + 1`), trade-ID gaps, whether connection handovers were seamless, whether every snapshot can be synced to the stream (Binance rule `U <= lastUpdateId + 1 <= u`), and receive latency. It exits non-zero if anything is wrong. Gaps aren't fatal: normalization (P2.7) splits the data into segments at gaps and re-syncs from the next snapshot.

Live health: `data/raw/_status.json` is rewritten every 60 s with frame and snapshot counts and the last frame time per symbol. If `last_frame_ns` stops advancing, the recorder is stuck or the machine is offline.

## Archive layout

```
data/raw/binance/spot/<SYMBOL>/<YYYY-MM-DD>/<HH>-<open_ns>.jsonl.zst   # finalized hourly file
data/raw/binance/spot/<SYMBOL>/<YYYY-MM-DD>/<HH>-<open_ns>.jsonl.zst.part  # file being written
data/raw/binance/spot/_meta/...    # exchangeInfo (daily) and NTP clock samples (hourly)
```

Files are flushed as independent zstd frames every 5 s, so a `.part` file left by a crash or power cut stays readable up to its last flush. The envelope format is documented in [research/data/rawio.py](../research/data/rawio.py).

## Disk usage (measured)

Measured on 2026-11-02 over 88 s: about **3.5 kB/s per symbol** compressed (9.6× zstd ratio). That's **about 0.3 GB per symbol per day** at that activity level, with snapshots adding about 5 MB/day. Volatile days can be several times larger, so budget **1 GB per symbol per day** to be safe.

## Where to run it

**Latency finding from the first runs (2026-11-02):** from the development PC, receive time minus Binance event time has a floor of about 62 ms, but **bursts of 1–2.5 s backlog** occur every minute or so. A bare probe that only timestamps frames shows the same bursts at the same moments, so they come from the network path, not the recorder. Two consequences:

1. The archive is **complete** regardless (no sequence gaps), so these recordings are fine for book reconstruction and research.
2. `ts_local` latencies recorded here describe *this* network path. For realistic latency models (P3.8), record from a small Linux VPS **near Binance's matching engine (AWS Tokyo, `ap-northeast-1`)**.

### Option A: this Windows PC (installed 2026-11-02)

A scheduled task named **`BlockSprint Recorder`** runs [scripts/run_recorder.ps1](../scripts/run_recorder.ps1) hidden at every logon. That script restarts the recorder 10 s after it exits for any reason, and logs each restart to `logs/recorder_supervisor.log`. Task Scheduler also restarts the script itself if it dies. Killing the recorder process was tested: it was streaming again 13 s later.

It runs while you're logged in (no admin rights or stored password needed). Nothing runs while the PC sleeps or is shut down; on 2026-11-02 a sleep at 23:41 lost about 24 hours of data.

**Keep-awake (since 2026-11-03):** while it runs, the recorder holds a Windows "system required" request (`SetThreadExecutionState`, see [research/data/keep_awake.py](../research/data/keep_awake.py)), so Windows won't *idle*-sleep or idle-hibernate. The display can still turn off. The request is released automatically when the recorder exits or crashes. Pass `--no-keep-awake` to turn it off. Check it from an Administrator prompt with `powercfg /requests`: `python.exe` appears under `SYSTEM`.

Keep-awake **cannot** stop sleep that you start, so for unbroken recording:
- Don't choose *Start → Sleep*, and don't shut down.
- Set *Control Panel → Power Options → Choose what closing the lid does → When I close the lid (Plugged in)* to **Do nothing**.
- Keep the laptop plugged in.
- Windows Update can still restart the PC. Set *Settings → Windows Update → Active hours* to cover the recording, or pause updates for a day. The scheduled task resumes recording at the next login, and the gap shows up in `inspect_raw`.

Manage it from PowerShell:

```powershell
Get-ScheduledTask "BlockSprint Recorder" | Select-Object State          # Running / Ready
Get-Content logs\recorder.log -Tail 5                                    # recorder log
Get-Content logs\recorder_supervisor.log -Tail 5                         # restarts
Stop-ScheduledTask "BlockSprint Recorder"                                # stop now (see note)
Start-ScheduledTask "BlockSprint Recorder"                               # start now
Disable-ScheduledTask "BlockSprint Recorder"                             # don't start at logon
Unregister-ScheduledTask "BlockSprint Recorder" -Confirm:$false          # remove completely
```

`Stop-ScheduledTask` ends the script abruptly. The current hour's file stays a `.part` file, which is still readable up to its last 5 s flush. For a clean stop of a manual run, use `Ctrl+C`.

To record other symbols, edit the `-Symbols` default in `scripts/run_recorder.ps1`, then stop and start the task.

### Option B: Linux VPS in Tokyo (recommended for latency studies)

`/etc/systemd/system/blocksprint-recorder.service`:

```ini
[Unit]
Description=BlockSprint market-data recorder
After=network-online.target
Wants=network-online.target

[Service]
WorkingDirectory=/opt/blocksprint
ExecStart=/usr/local/bin/uv run --no-sync python -m research.data.recorder --symbols BTCUSDT ETHUSDT --out /data/raw
Restart=always
RestartSec=10
User=blocksprint

[Install]
WantedBy=multi-user.target
```

Then `systemctl enable --now blocksprint-recorder` and keep the clock synced with `chrony`. Copy archives to your research machine with `rsync`, skipping `*.part`.

## Rate limits

A `limit=5000` depth snapshot costs 250 request weight; Binance allows 6,000 per minute per IP. The defaults (one snapshot per symbol every 10 min, spaced 1 s apart) use well under 1% of that. On HTTP 429 or 418 the recorder backs off for the `Retry-After` period.

## Next steps for the data

Normalizing, checking, packing and cataloguing the recorded data: [data_pipeline.md](data_pipeline.md).
