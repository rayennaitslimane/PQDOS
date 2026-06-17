#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

cd "$PROJECT_DIR/docs/metrics" || exit 1

python -m venv .venv && . .venv/bin/activate
pip install -r requirements.txt
jupyter notebook