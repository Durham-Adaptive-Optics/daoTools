# Primary daoBase API migration regression tests

The migration uses daoBase's primary SHM API (introduced in daoBase commit
`8f64d74`). Python's public `shm` interface is unchanged.

The production changes are restricted to these substitutions:

| Previous call | Primary call |
| --- | --- |
| `daoShmImageCreate(...)` | `daoShmCreate(...)` |
| `daoShmShm2Img(...)` | `daoShmOpen(...)` |
| `daoShmImage2Shm(data, count, image)` | `daoShmSetData(image, data, count)` |
| `daoShmImagePart2ShmFinalize(...)` | `daoShmSetDataPartFinalize(...)` |
| `daoShmCombineShm2Shm(...)` | `daoShmCombine(...)` |
| `daoShmWaitForSemaphore(...)` | `daoShmWaitSem(...)` |
| `daoShmWaitForSemaphoreTimeout(...)` | `daoShmWaitSemTimeout(...)` |

Only the full-frame write changes argument order. Return-code checks, timing,
algorithms, frame-ID propagation, and publication points are preserved.

## Running the tests

Requirements: pytest, NumPy, the current daoBase Python bindings and library,
and original/migrated daoTools builds. Build **both versions against the same
new daoBase**, with the same compiler and optimization settings. The baseline
for this migration is daoTools commit `6f15a56`.

Use separate source copies and a temporary installation prefix for daoBase
when validating a working installation. Do not replace live installed
libraries to run these tests. The migrated daoTools build requires the new
daoBase headers **and** library; the previous installed library lacks the new
symbols.

For example, after building the two copies:

```bash
export DAO_TOOLS_BASELINE_SOURCE=/path/to/original/daoTools
export DAO_TOOLS_BASELINE_LIB=/path/to/original/build/src/libdaoTools.so
export DAO_TOOLS_LIB=/path/to/migrated/build/src/libdaoTools.so

# Point at matching, current daoBase bindings and libraries.
export PYTHONPATH=/path/to/daoBase/src/python:/path/to/daoBase/installed/python
export LD_LIBRARY_PATH=/path/to/daoBase/build/src:/path/to/daoBase/installed/lib

python3 -B -m pytest -q tests/test_primary_api.py tests/test_api_migration_scope.py
```

On macOS, use the corresponding `.dylib` paths and configure `DAOROOT` to
the isolated daoBase installation used by its Python binding. The validation
reported below was run on Linux, not macOS.

Both daoTools library paths must refer to distinct builds. Executable tests
expect the associated executables under `../apps` relative to each library's
`src` directory. Missing optional test configuration is reported as a skip;
a migration validation run must finish with **no skipped tests**.

The tests create private temporary SHM files. Executable tests use short
filenames for compatibility with existing CLI buffers, wait at most five
seconds for expected output, and terminate their own subprocesses on exit.
They never launch an installed tool by searching `PATH`.

## What is checked

- Numerical create/open/write/read-back behavior for all 12 scalar/complex
  types, including integer limits and signed/fractional values.
- Frame counter increments, direct-buffer finalization, both semaphore wait
  interfaces, and timeout return values.
- Old/new generic channel combination against NumPy, for all ten real types,
  1/4/6/8 channels, single-slot and three-slot SHM, including FIFO wrap-around.
- Every changed library routine: float/double calibration, pyramid calibration,
  extraction, all three explicit normalization finalizers, both flux-based
  normalization routines, DM combination, and copying to an output offset.
- Empty masks, zero flux, rejected mask types, frame IDs, publication counts,
  and copy-without-finalize behavior.
- Real original/new executables for scalar/modal gain, matrix multiplication,
  calibration, extraction, and mean/sum downsampling on distinct input frames.
- An independent inverse transformation checks every token in all 49 changed
  production files against the baseline. It allows only the documented API
  renames, write-argument reordering, comments, and whitespace.

Before/after numeric arrays must match exactly. Independent NumPy references
use a small floating-point tolerance where appropriate; integer outputs are
exact. The DM-specific combiner deliberately checks existing behavior against
the baseline, rather than redefining its clipping/piston algorithm.

## Validation of this migration

On Linux with GCC 13.3 and daoBase `8f64d74`:

- Original and migrated full builds passed (101 Waf tasks each), including
  daoDAQ and the GPU target.
- 209 numerical/integration cases and 49 source-equivalence cases passed.
- All 46 changed sources included in the normal build produced identical
  machine-instruction bytes to an independent reference build made by applying
  the seven API mappings to the original via C preprocessor aliases.
  Build-path strings and their relocation offsets differ between source copies.
- The excluded `daoModesCutoffFull.c` and `daoRandWriter.c` passed syntax
  checks before and after.
- The excluded `daoDMSend.c` fails before and after because `inShmName`,
  `inShm`, and `waitCounter` are undeclared. This unrelated defect is unchanged.

The C normalization routines cache the mask address and update counter in
static variables. During testing, closing and recreating masks at a reused
address/counter exposed stale cached indices in the **baseline**. Each
numerical case loads fresh library copies to isolate this state, as separate
CLI processes do. The existing cache-lifetime issue is not repaired by this
API-only migration.

This is regression evidence for the interface transition, not a claim that
every existing algorithm is correct. GPU execution, hardware/network delivery,
macOS execution, and every executable's full operating modes have not been
tested. The exact source checks cover the migration in those callers.
