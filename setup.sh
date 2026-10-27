#!/usr/bin/env bash
set -euo pipefail

STAMP=".venv/.setup-hash"

CURRENT_HASH=$(shasum -a 256 requirements-dev.txt .pre-commit-config.yaml | shasum -a 256 | cut -d' ' -f1)

# Skip if already configured
if [[ -x ".venv/bin/python" && -x ".venv/bin/pre-commit" && -f "$STAMP" ]] &&
   [[ "$(cat "$STAMP")" == "$CURRENT_HASH" ]]; then
    echo "Environment already configured. Skipping."
    exit 0
fi

# Create environment if missing
if [[ ! -x ".venv/bin/python" ]]; then
    python3 -m venv .venv
fi

.venv/bin/python -m pip install -r requirements-dev.txt
.venv/bin/pre-commit install
.venv/bin/pre-commit run --all-files

echo "$CURRENT_HASH" > "$STAMP"
