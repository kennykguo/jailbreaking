# Codex snapshots

Store full chat logs here so they travel with the repo across OSes.

Two files are kept for each session:
1) The raw JSONL export copied from `~/.codex/sessions/...`.
2) A plain-text transcript extracted from that JSONL (user/assistant only).

How to update the latest transcript (always create a new `.txt` file):
- Copy the newest JSONL into this folder and give it a new timestamped filename
  (do not overwrite the previous JSONL).
- Run the formatter on that JSONL. It writes a `.txt` file with the same base name.

Example:
```
STAMP=$(date -u +"%Y%m%dT%H%M%SZ")
cp ~/.codex/sessions/2026/01/14/rollout-*.jsonl \
  codex-snapshots/rollout-2026-01-14T21-16-22-${STAMP}.jsonl
./codex-snapshots/format_jsonl.py \
  codex-snapshots/rollout-2026-01-14T21-16-22-${STAMP}.jsonl
```

Notes:
- The formatter accepts a single JSONL filename and always outputs `<input>.txt`.
- The file you read as the transcript is the `.txt` file, not the `.jsonl`.
- If the transcript looks stale, the session log may not have flushed yet; rerun later.
- Naming convention: `rollout-YYYY-MM-DDTHH-MM-SS-<session_id>-YYYYMMDDTHHMMSSZ.jsonl`
  where the trailing UTC stamp makes every refresh unique and consistent.
