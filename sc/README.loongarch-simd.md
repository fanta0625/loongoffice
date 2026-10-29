# Calc LSX array summation

The LSX implementation uses four independent two-double accumulators with
Neumaier compensation. Scalar code handles the remaining elements. Non-finite
vector results are recomputed through the existing scalar path to retain Calc's
handling of encoded errors and empty cells.

## Compiler baseline

The implementation is selected when the compiler defines `__loongarch_sx`.
Product compilation must use a consistent target baseline for the implementation
and its callers. This selection does not detect CPU capabilities at runtime;
an LSX-targeted product requires an LSX-capable CPU and operating system.

## Unit tests

Run the tests for the configured compiler baseline:

```sh
make CppunitTest_sc_arraysum
```

On LoongArch with CPU and operating-system support for LSX, explicitly test the
LSX implementation:

```sh
make SC_ARRAYSUM_TEST_LSX=TRUE CppunitTest_sc_arraysum_lsx
```

The explicit target adds `-mlsx` to its own translation unit. Separate test
objects keep these flags independent of the default test and product objects.
The tests include implementation sources to avoid sharing production gbuild
objects (fdo#47246) or exporting internal functions solely for testing. Product
build integration and path selection require separate product-level checks.

Coverage includes lengths and tails, alignment offsets, compensated
cancellation, exact allocations, encoded errors, infinity, and the LSX
consumed-index contract. With `DISABLE_NAN_TESTS=TRUE`, assertions requiring NaN
payload preservation are skipped in both targets; finite arithmetic, infinity,
and index checks remain enabled. Use an ASan build to detect out-of-bounds loads
in the exact-allocation tests.

To check the configuration that disables NaN-payload assertions:

```sh
make DISABLE_NAN_TESTS=TRUE CppunitTest_sc_arraysum
make DISABLE_NAN_TESTS=TRUE SC_ARRAYSUM_TEST_LSX=TRUE CppunitTest_sc_arraysum_lsx
```

## LoongOffice 26.2.6 validation snapshot

This change is scoped to Calc array summation on the LoongOffice baseline
`af1911d32d3775e6a8becb3b0a9b7b1b52d9c894`. LASX, runtime CPU dispatch, and
optimizations to other modules are outside this change. Generic compilation
selects the existing generic path; an LSX-targeted binary does not automatically
fall back on machines without LSX support.

The following results were reported from physical LoongArch testing of the
implementation before commit consolidation; they were not rerun on the local
Windows host:

- Default and explicit LSX CppUnit targets each passed five tests.
- Function-level comparisons used identical `-mlsx` compiler options for both
  versions, with exact checks across sizes and offsets. The one-million-element
  case measured 1.962 ms for the baseline and 0.740 ms for LSX (about 2.65x).
- A focused product experiment relinked `libsclo.so` from the same product tree.
  Both versions recompiled `interpr6.cxx` with `-O2 -mlsx`; the candidate also
  compiled the LSX implementation with those options. All 120 checks passed.
  One-million-row `calculateAll()` measured 17.578 ms versus 8.641 ms (about
  2.03x). This was not a clean full-tree LSX build or a GUI performance result.

The later changes to honor `DISABLE_NAN_TESTS` and documentation do not alter
the production implementation. Physical-machine verification of both test
targets with and without that setting is still pending in the available record.
The additional business-like XLSX suite has baseline measurements only; no
candidate speedup is claimed for those files. A current upstream master port,
its build/regression checks, and upstream submission remain follow-up work.
