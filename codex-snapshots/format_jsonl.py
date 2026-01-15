#!/usr/bin/env python3
import json
import sys
from typing import List

SEPARATOR = "\n---\n"

def main() -> int:
    if len(sys.argv) != 2:
        print("usage: format_jsonl.py <session.jsonl>", file=sys.stderr)
        return 2

    input_path = sys.argv[1]
    base = input_path.rsplit(".", 1)[0]
    output_path = f"{base}.txt"

    out = open(output_path, "w", encoding="utf-8")

    with open(input_path, "r", encoding="utf-8") as f:
        first = True
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except json.JSONDecodeError:
                obj = {"_parse_error": True, "raw": line}

            payload = obj.get("payload", {})
            if payload.get("type") != "message":
                continue
            role = payload.get("role")
            if role not in ("user", "assistant"):
                continue
            content = payload.get("content", [])
            parts: List[str] = []
            for item in content:
                text = item.get("text")
                if text:
                    parts.append(text)
            if not parts:
                continue

            if not first:
                out.write(SEPARATOR)
            first = False
            out.write(f"{role.upper()}:\n")
            out.write("\n".join(parts))
            out.write("\n")

    out.close()
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
