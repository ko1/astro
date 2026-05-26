# baruby_precise — backend matrix (median of 3 runs)

Build: `make GC=<backend> ASTRO_DEBUG=0`, mode `plain`.
Generated: 2026-05-26T05:29:36Z

## Elapsed (seconds, median)

| Bench | mark | mark_gen | mark_gen_inc | copy |
|---|---:|---:|---:|---:|
| ackermann | 7.62 | **7.53** | — | — |
| binary_trees | **0.74** | 1.30 | — | — |
| chain_add | **1.36** | — | — | — |
| fannkuch | 0.79 | **0.78** | — | — |
| fib | 7.37 | **7.19** | — | — |
| fib_pair | **0.97** | 1.06 | — | — |
| gc_combined | **0.90** | — | — | — |
| hash_chain | **1.21** | — | — | — |
| json_parse | **0.98** | — | — | — |
| list_alloc | **0.81** | — | — | — |

## Peak RSS (MiB, from /usr/bin/time -M)

| Bench | mark | mark_gen | mark_gen_inc | copy |
|---|---:|---:|---:|---:|
| ackermann | **7.9** | 8.2 | — | — |
| binary_trees | **258.8** | 269.6 | — | — |
| chain_add | **2.2** | — | — | — |
| fannkuch | **30.9** | 32.9 | — | — |
| fib | **2.2** | 2.4 | — | — |
| fib_pair | **23.8** | 29.1 | — | — |
| gc_combined | **26.9** | — | — | — |
| hash_chain | **20.4** | — | — | — |
| json_parse | **23.9** | — | — | — |
| list_alloc | **26.2** | — | — | — |
