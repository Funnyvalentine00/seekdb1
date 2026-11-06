# Preserved current-server evidence

Source reference f97bc2823; optimized code6062f81e2. See ../../FINAL_REPORT.md
for interpretation, exact hashes, hardware and limitations.

- E01 groups1/2: maintained cuVS BF versus existing GEMM, five pairs each.
- E02 groups1/2: f97 GEMM versus fused epilogue, five pairs each.
- E02 group3: incomplete1M reference timeout; no valid pair/candidate result.
- acceptance/: final linked helper, CPU/HIP/dynamic checks, lifecycles, final
  unpaired250K and256D control. Failed SONAME coverage rejection is preserved.
- sql-smoke/: existing full12-case GPU integration smoke.
- ddl-native-acceptance/: default SQL restart6/6.
- ddl-native-fused-acceptance_validated/: tile_b128 functional coverage7/7,
  positive fused counters; this parameter is not a new performance claim.
- partial-profile/: partial trace statistics only. Profile wall time is invalid;
  CPU API waits and nested GPU durations must not be added together.

Text evidence is stored without line-ending conversion. manifest.json hashes
the curated files. Complete traces, stdout duplicates, full application logs,
candidate/reference binaries and archive manifests are backed up outside Git
under the local amd-evidence-20261107 directory and remote task root. No datasets,
previous code or remote build artifacts were deleted.
