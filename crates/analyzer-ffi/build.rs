//! Generates the C header the platform user interfaces compile against.
//!
//! Written into `include/` at build time rather than committed, so the header
//! and the Rust surface cannot drift. A stale checked-in header that still
//! compiles but no longer matches the ABI is a bad failure: the linker is happy
//! and the fields are wrong.

use std::path::PathBuf;

fn main() {
    println!("cargo:rerun-if-changed=src/lib.rs");
    println!("cargo:rerun-if-changed=cbindgen.toml");

    let crate_dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    let include = crate_dir.join("include");
    if let Err(error) = std::fs::create_dir_all(&include) {
        println!("cargo:warning=cannot create {}: {error}", include.display());
        return;
    }

    match cbindgen::generate(&crate_dir) {
        Ok(bindings) => {
            bindings.write_to_file(include.join("analyzer.h"));
        }
        // A header failure must not block `cargo test`, which does not need one,
        // but it must be loud enough to notice before the Xcode build fails.
        Err(error) => println!("cargo:warning=cbindgen failed: {error}"),
    }
}
