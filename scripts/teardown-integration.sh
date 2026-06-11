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

# Single confirmation prompt
read -r -p "Reset storage node data? (y/N): " answer
if [[ "$answer" == "y" || "$answer" == "Y" ]]; then
  echo "Resetting storage node data..."
  rm -rf /tmp/node1-data /tmp/node2-data /tmp/node3-data
else
  echo "Keeping existing storage node data."
fi

# Single confirmation prompt
read -r -p "Reset /tmp/pqdos_test_kek.json? (y/N): " answer
if [[ "$answer" == "y" || "$answer" == "Y" ]]; then
  echo "Resetting /tmp/pqdos_test_kek.json..."
  rm -f /tmp/pqdos_test_kek.json
else
  echo "Keeping existing /tmp/pqdos_test_kek.json."
fi

echo "Teardown complete."
