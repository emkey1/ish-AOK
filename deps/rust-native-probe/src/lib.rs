//! The smallest Rust program that proves the interposition works end to end.
//!
//! It exists to answer one question that nothing else in the tree can: does a
//! foreign toolchain's standard library, which AOK does not compile and whose
//! libc calls no #define reaches, actually land on the guest's filesystem?
//!
//! The whole mechanism is in the build, not here: tools/gen-nlibc-renames.py
//! rewrites this archive's libc imports onto nlibc_* before it is linked. So
//! this file is deliberately ordinary Rust -- std::fs and std::env, nothing
//! clever -- because the point is that ordinary Rust needs no adaptation.
use std::env;
use std::ffi::{c_char, c_int, CStr};
use std::fs;

fn show(path: &str) {
    match fs::metadata(path) {
        Ok(m) => println!("  {} -> {} bytes, dir={}", path, m.len(), m.is_dir()),
        Err(e) => println!("  {} -> {}", path, e),
    }
}

/// argv/envp arrive as C arrays: this is called as a native program
/// (kernel/native.h), on a guest task's own thread.
#[no_mangle]
pub extern "C" fn rust_native_probe_main(
    argc: c_int,
    argv: *const *const c_char,
    _envp: *const *const c_char,
) -> c_int {
    let mut args: Vec<String> = Vec::new();
    for i in 0..argc.max(0) as usize {
        let p = unsafe { *argv.add(i) };
        if p.is_null() {
            break;
        }
        args.push(unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned());
    }

    println!("rust-native-probe: Rust {} running as a native program", env!("CARGO_PKG_VERSION"));
    println!("argv: {:?}", args);

    // Everything below goes through Rust's std, which knows nothing about AOK.
    // If the renames did their job these are the GUEST's files.
    println!("guest filesystem, via std::fs:");
    show("/etc");
    show("/etc/hostname");
    show("/AOK/VERSION");

    match fs::read_to_string("/AOK/VERSION") {
        Ok(v) => println!("  /AOK/VERSION contents: {}", v.trim()),
        Err(e) => println!("  /AOK/VERSION unreadable: {}", e),
    }

    println!("directory listing of /AOK, via std::fs::read_dir:");
    match fs::read_dir("/AOK") {
        Ok(entries) => {
            let mut names: Vec<String> =
                entries.filter_map(|e| e.ok()).map(|e| e.file_name().to_string_lossy().into_owned()).collect();
            names.sort();
            println!("  {}", names.join(" "));
        }
        Err(e) => println!("  read_dir failed: {}", e),
    }

    // The sysctlbyname split: this asks hw.ncpu, which nlibc_sysctlbyname
    // answers from AOK's policy rather than the host's core count.
    match std::thread::available_parallelism() {
        Ok(n) => println!("available_parallelism: {} (AOK's count, not the host's)", n),
        Err(e) => println!("available_parallelism: {}", e),
    }
    println!("cwd: {:?}", env::current_dir());
    0
}
