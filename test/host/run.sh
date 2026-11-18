#!/bin/sh
# Builds and runs the host tests (no device needed). Used by both the build and the release workflow, so a test
# that needs extra sources is listed once. Run from the repository root.
set -eu
CXX="${CXX:-g++} -std=c++17 -O2 -Wall -Wextra -Werror -I include"
OUT=${1:-build/host-tests}
mkdir -p "$OUT"

$CXX test/host/flash_log_test.cpp -o "$OUT/flash_log_test"
"$OUT/flash_log_test"
$CXX test/host/provisioning_code_test.cpp -o "$OUT/provisioning_code_test"
"$OUT/provisioning_code_test"
$CXX test/host/text_parse_test.cpp src/util/semantic_version.cpp -o "$OUT/text_parse_test"
"$OUT/text_parse_test"
$CXX test/host/upload_policy_test.cpp -o "$OUT/upload_policy_test"
"$OUT/upload_policy_test"
$CXX test/host/battery_monitor_test.cpp -o "$OUT/battery_monitor_test"
"$OUT/battery_monitor_test"
