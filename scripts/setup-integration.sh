#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "Starting postgres-test..."
docker compose -f "$PROJECT_DIR/docker-compose.yml" up -d postgres-test

echo "Waiting for postgres to be healthy..."
until docker compose -f "$PROJECT_DIR/docker-compose.yml" exec -T postgres-test pg_isready -U test_user -d pqdos_test > /dev/null 2>&1; do
  sleep 1
done
echo "Postgres is ready."

echo "Starting storage nodes..."

# Single confirmation prompt
read -r -p "Reset storage node data? (y/N): " answer
if [[ "$answer" == "y" || "$answer" == "Y" ]]; then
  echo "Resetting storage node data..."
  rm -rf /tmp/node1-data /tmp/node2-data /tmp/node3-data
else
  echo "Keeping existing storage node data."
fi

mkdir -p /tmp/node1-data /tmp/node2-data /tmp/node3-data

"$PROJECT_DIR/build/Release/storage_node" 0.0.0.0 9001 /tmp/node1-data &
"$PROJECT_DIR/build/Release/storage_node" 0.0.0.0 9002 /tmp/node2-data &
"$PROJECT_DIR/build/Release/storage_node" 0.0.0.0 9003 /tmp/node3-data &

sleep 1

echo "Starting storage client..."
"$PROJECT_DIR/build/Release/storage_client" \
  0.0.0.0 \
  8080 \
  "host=localhost port=5433 dbname=pqdos_test user=test_user password=test_password" &

echo "Waiting for storage client to be ready..."
for i in $(seq 1 30); do
  if curl -sf http://localhost:8080/objects > /dev/null 2>&1; then
    echo "Storage client is ready on 0.0.0.0:8080"
    exit 0
  fi
  sleep 1
done

echo "ERROR: Storage client did not become ready in time."
exit 1
