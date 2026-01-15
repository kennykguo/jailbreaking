# Codex snapshots

Store full chat logs here so they travel with the repo across OSes.

Two files are kept for each session:
1) The raw JSONL export copied from `~/.codex/sessions/...`.
2) A plain-text transcript extracted from that JSONL (user/assistant only).

How to update the latest transcript:
- Copy the newest JSONL into this folder (same filename).
- Run the formatter on that JSONL. It writes a `.txt` file with the same base name.

Example:
```
cp ~/.codex/sessions/2026/01/14/rollout-*.jsonl codex-snapshots/
./codex-snapshots/format_jsonl.py codex-snapshots/rollout-*.jsonl
```

Notes:
- The formatter accepts a single JSONL filename and always outputs `<input>.txt`.
- If the transcript looks stale, the session log may not have flushed yet; rerun later.
