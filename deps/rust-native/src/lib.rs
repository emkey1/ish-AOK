//! Native Rust programs for iSH-AOK.
//!
//! One crate rather than one per program, because each staticlib brings its
//! own std and two of them in a link is thousands of duplicate symbols. See
//! Cargo.toml.
//!
//! The whole mechanism is in the build, not here: tools/build-rust-native.sh
//! rewrites this archive's libc imports onto nlibc_* before it is linked, so
//! these modules are deliberately ordinary Rust -- the point is that ordinary
//! Rust needs no adaptation.
pub mod probe;

#[cfg(feature = "helix")]
pub mod helix;
