# Regression report

| Check | State | Limits |
| --- | --- | --- |
| CPU ASan/UBSan harness | PASS8/8 | CPU doubles, no GPU timing |
| Production HIP bridge | PASS | Real ROCm7.2.1, gfx1100; warnings retained |
| Dynamic symbol resolution | PASS | After private-prefix dependency install |
| Real kernel fixture | PASS |8192 distances, short-tile copy/merge |
| Full GEMM helper | PASS20 combinations |128D,256D,dim130,tails,tiles smaller than K; linked fused/unfused WMMA and BLAS counters verified |
| FP32 bridge lifecycle | PASS | Small fixture, Recall@10=1.0 |
| FP16 bridge reload | PASS after private patch | Original failed snapshot loads unchanged |
| Wrong snapshot shape | PASS | Wrong n/dim rejected; float and half |
| E02 paired GPU promotion | PASS250K/d128 | Two5-pair groups,18.27%/18.11%, Recall and sampled peak unchanged |
| Final250K/@10/@100/resources | PASS | Default-on final bridge; unpaired acceptance |
| 256D AUTO control | PASS10K | R@10=1/@100=1; no comparative speedup claim |
| 1M paired group | INCOMPLETE | Reference first build240s timeout; no candidate result |
| SQL route/fallback/freshness/batch/no-GPU | PASS12/12 | Existing full smoke; R10=1/R100=.9994 on10K |
| SQL default restart persistence | PASS6/6 | Native adaptation of existing Docker-only smoke; actual final bridge maps |
| SQL fused-path restart persistence | PASS7/7 | tile_b128 coverage only, fused_tiles312; initial console-log oracle failure preserved |
| Concurrent builds | PENDING | Bridge still has global affinity/VRAM-watch state |
| HIP OOM/launch/sync failures on GPU | PENDING | CPU allocation/copy/select failure injection passes |
| No-rocWMMA/other-architecture runtime | PENDING | CPU recording-compiler check only |
| Contest | NOT RUN | No score claimed |

Preserve the failed validation logs; a fixed test oracle is not a production
performance improvement. Every retained candidate must rerun relevant checks.
