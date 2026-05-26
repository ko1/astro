# Known intermittent fuzz failures (aot-stress mode)

These programs caused SEGV in `aot-stress` mode during sustained fuzz runs
(15+ seeds × 600 iter × 4 modes), but do NOT reproduce in isolation:
- Plain mode: OK
- AOT alone: OK (program too short to bake SDs in some cases)
- aot-stress in fresh run: OK
- BARUBY_GC_STRESS=1 standalone: OK

Hypothesis: the bug triggers under combined heap/GC pressure when:
- code_store/ has accumulated many SDs (50+ programs baked)
- fuzz parent process is iter ≥75
- Ruby interpreter is concurrently running pg-compile via Open3

Need ulimit -c unlimited + ASAN build to capture a core dump and root-cause.
