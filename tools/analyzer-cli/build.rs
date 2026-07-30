//! Embeds an `Info.plist` into the executable on macOS.
//!
//! Without one, `AudioDeviceCreateIOProcID` on an input device blocks forever:
//! TCC needs a bundle identifier and a usage description to attribute a
//! microphone prompt to, and an unbundled binary provides neither, so the
//! request never resolves. Embedding the plist in a `__TEXT,__info_plist`
//! section gives a plain command-line binary the same identity a bundle would.

fn main() {
    println!("cargo:rerun-if-changed=Info.plist");
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("macos") {
        let plist = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("Info.plist");
        println!(
            "cargo:rustc-link-arg=-Wl,-sectcreate,__TEXT,__info_plist,{}",
            plist.display()
        );
    }
}
