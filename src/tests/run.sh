#!/bin/sh
# Host-side config parser tests. No vitasdk required.
set -e
cd "$(dirname "$0")/../.."
out=$(mktemp -t pfe_test_config)
trap 'rm -f "$out"' EXIT
cc -Wall -Wextra -Werror -O1 -o "$out" src/config.c src/tests/test_config.c
"$out"
