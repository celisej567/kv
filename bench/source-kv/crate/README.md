# Put the crate here

This folder is empty on purpose: `source-kv` is not part of this repository.
To run the comparison, copy the crate here so that `Cargo.toml` and `src/` of `source-kv` sit next to this file:

    git clone https://github.com/LaVashikk/SourceRS
    cp -r SourceRS/crates/source-kv/. bench/source-kv/crate/

(or pass another checkout with `run_sourcers.py --crate=DIR`). Everything else in this folder except this file is
ignored by git. Mind the crate's own license (MIT, see its `LICENSE`).
