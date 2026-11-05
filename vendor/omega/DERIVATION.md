# Origin and port boundary

Original sources come from 3scud3r0/Omega commit
47dd6a20f1699ac6e54ece82397ae64fa78493c4. UPSTREAM.json records their hashes.
The `.omega` compiler and engine modules are preserved without algorithm edits.
`entry.omega` is a local, executable e-graph example used to drive the original
module linker; `egraph_linked.omega` and `modules.json` are its derived output.
The compiler's own principal, not this link entry, is used in the guest.

`tests/bootstrap_omega.py` applies the pinned stage-0 parser/emitter at build
and selects reachable functions from the original compiler and linked engine.
GNU statement expressions emitted by that seed are explicitly marked with
`__extension__`; the C99/pedantic build otherwise treats warnings as errors.

`user/omega/omega_value.h` replaces the hosted value runtime: fixed 8-MiB arena,
checked int64 rationals, bounded ASCII identifier handling and in-memory I/O.
The original compiler principal receives an in-memory source and the --ir flag.
Its checked IR is lowered by the guest scalar bytecode backend. The port is not
an arbitrary-precision replacement for GMP or a full native Omega backend.

`user/omega/driver.c` executes the original e-graph execute() function, including
congruence rebuilding and independent polynomial equivalence. Its PUCT search
is a bounded rational adaptation of omega_synth_mcts/search.py: affine grammar,
2x3 rational tensor priors, integer-floor square-root exploration, finite gas.
It does not port PyTorch, CUDA, Lean admission or the full original grammar.
