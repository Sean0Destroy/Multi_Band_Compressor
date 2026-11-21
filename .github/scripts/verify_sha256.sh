#!/usr/bin/env bash
# Usage: verify_sha256.sh <file> <expected-sha256>
# Tools downloaded in CI must match the digest pinned in the workflow.
set -euo pipefail
file=$1; expected=$2
if command -v sha256sum >/dev/null 2>&1; then
  actual=$(sha256sum "$file" | cut -d' ' -f1)
else
  actual=$(shasum -a 256 "$file" | cut -d' ' -f1)
fi
if [ "$actual" != "$expected" ]; then
  echo "SHA-256 mismatch for $file: got $actual, expected $expected" >&2
  exit 1
fi
echo "SHA-256 ok: $file"
