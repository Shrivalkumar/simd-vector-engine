#!/usr/bin/env bash
set -euo pipefail

VDB_REPOSITORY_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
VDB_SANITIZER_IMAGE="vectordb-sanitizer-base:local"

docker build \
  --target build \
  --tag "${VDB_SANITIZER_IMAGE}" \
  --file "${VDB_REPOSITORY_ROOT}/deploy/docker/Dockerfile" \
  "${VDB_REPOSITORY_ROOT}"

docker run --rm \
  --volume "${VDB_REPOSITORY_ROOT}:/work:ro" \
  "${VDB_SANITIZER_IMAGE}" \
  bash -lc 'cmake -S /work -B /asan -DCMAKE_BUILD_TYPE=Debug -DVDB_BUILD_TESTS=ON -DVDB_BUILD_TOOLS=OFF -DVDB_BUILD_GRPC=OFF -DVDB_ENABLE_ASAN=ON && cmake --build /asan --target vectordb-tests -j$(nproc) && ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ctest --test-dir /asan -R vectordb.unit --output-on-failure'

docker run --rm \
  --volume "${VDB_REPOSITORY_ROOT}:/work:ro" \
  "${VDB_SANITIZER_IMAGE}" \
  bash -lc 'cmake -S /work -B /tsan -DCMAKE_BUILD_TYPE=Debug -DVDB_BUILD_TESTS=ON -DVDB_BUILD_TOOLS=OFF -DVDB_BUILD_GRPC=OFF -DVDB_ENABLE_TSAN=ON && cmake --build /tsan --target vectordb-tests -j$(nproc) && TSAN_OPTIONS=halt_on_error=1:history_size=7 ctest --test-dir /tsan -R vectordb.unit --output-on-failure'
