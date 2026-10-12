# Submodule patches

The parse-side speedups (findings.md S3, S4 and the first part of M7; progress.md Steps 3a, 3b,
3d, 5, 5b, 5c) live in the `srcDispatch` and `srcDispatch/srcSAX` submodules, which point at
`srcML/srcDispatch` and `srcML/srcSAX`. They are not committed in the submodules, so they are kept
here as patches to be reviewed with the srcSlice changes:

- `srcDispatch-perf.patch`: against srcDispatch `09a669a` (44 files: `src/dispatcher/`, every
  policy class, and the new `src/dispatcher/Any.hpp`)
- `srcSAX-perf.patch`: against srcSAX `93cbbee` (5 files: the SAX2 handler, the controller, the
  C++ callback adapter)

Apply with

```bash
git -C srcDispatch apply ../patches/srcDispatch-perf.patch
git -C srcDispatch/srcSAX apply ../../patches/srcSAX-perf.patch
```

They also carry two crash fixes made while setting up the benchmarks (srcDispatch's `copyAs` did
`any_cast` on a `DeltaElement<std::any>` wrapper; srcSAX dereferenced `xmlCtxtGetLastError()`
without a null check) and the `ControlPolicy` reset fix of progress.md Step 3b.
