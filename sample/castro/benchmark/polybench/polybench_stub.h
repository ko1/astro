// Castro-friendly stand-in for utilities/polybench.h.
//
// PolyBench's official polybench.h declares timing helpers that pull in
// libc's <time.h>, <sys/time.h>, etc. — castro's C subset can't parse
// the resulting libc headers (typedefs, __builtin_va_list, …) cleanly,
// so the upstream header is replaced wholesale.
//
// We keep just the structural macros each kernel uses (POLYBENCH_2D
// etc. for declarations, POLYBENCH_ARRAY to forward an arg into a
// callee).  The timing / printing entry points are stubbed out — the
// kernel-specific main() in each adapted file calls the kernel inside
// a repeat loop and returns a checksum, which is what we measure.

#ifndef POLYBENCH_STUB_H
#define POLYBENCH_STUB_H

// Default scalar type — pick double; kernels that want int / float
// override DATA_TYPE before including this stub.
#ifndef DATA_TYPE
#define DATA_TYPE double
#endif

// Array declaration helpers.  We declare arrays as static-storage
// globals instead of dynamic allocation, so:
//   DATA_TYPE POLYBENCH_2D(A,N,M,n,m);     in upstream
// becomes:
//   DATA_TYPE A[N][M];                     after the macro expands
//
// Function args use the same syntax (pointer-to-array) — works because
// both forms are valid C array decls.

#define POLYBENCH_1D(var, dim1, dim1_unused) var[dim1]
#define POLYBENCH_2D(var, dim1, dim2, du1, du2) var[dim1][dim2]
#define POLYBENCH_3D(var, dim1, dim2, dim3, du1, du2, du3) var[dim1][dim2][dim3]

// Forward an array argument unchanged.  The original macro returns
// `(*x)` to dereference an alloc'd pointer; with our static-global
// model the array name itself decays to the right thing.
#define POLYBENCH_ARRAY(x) x

// Loop bound: just use the literal upper bound.
#define POLYBENCH_LOOP_BOUND(N, var) (N)

// Timing / instrumentation — all no-ops.  The main() in each adapted
// kernel measures via wall-clock outside the binary.
#define polybench_start_instruments         do { } while (0)
#define polybench_stop_instruments          do { } while (0)
#define polybench_print_instruments         do { } while (0)
#define polybench_prepare_instruments       do { } while (0)
#define polybench_flush_cache               do { } while (0)
#define polybench_papi_init_eventlist       do { } while (0)
#define polybench_papi_close                do { } while (0)
#define polybench_papi_print                do { } while (0)
#define polybench_papi_start_counter(i)     do { (void)(i); } while (0)
#define polybench_papi_stop_counter(i)      do { (void)(i); } while (0)

// Memory: skip alloc — the adapted kernel uses static globals.
#define polybench_alloc_data(n, sz)         ((void *)0)
#define polybench_free_data(p)              do { (void)(p); } while (0)
#define POLYBENCH_FREE_ARRAY(x)             do { (void)(x); } while (0)

#endif  /* POLYBENCH_STUB_H */
