#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "Stopping storage client..."
pkill storage_client || true

echo "Stopping storage nodes..."
pkill storage_node || true

echo "Stopping postgres-test..."
docker compose -f "$PROJECT_DIR/docker-compose.yml" down

echo "Teardown complete."
