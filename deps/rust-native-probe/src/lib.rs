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

    // --- the parts an editor actually needs -------------------------------
    //
    // Everything above is std::fs, which was never really in doubt once the
    // renames worked. These four are the ones that decide whether a large
    // Rust TUI can run as a native program at all, so they are checked
    // separately rather than assumed from "fs works".

    // 1. env::args()/vars(). On Apple these do NOT read the argv a staticlib
    //    was called with -- they go through _NSGetArgv/_NSGetEnviron, which
    //    answer about the host process. Unrouted, this printed the iSH app's
    //    command line and the Mac's environment.
    println!("env::args(): {:?}", env::args().collect::<Vec<_>>());
    println!("env::var(PATH): {:?}", env::var("PATH"));
    println!("env::var(HOME): {:?}", env::var("HOME"));
    println!("env::vars() count: {}", env::vars().count());
    // home_dir() consults the passwd database only when HOME is unset, and
    // that is the path that used to read the Mac's /etc/passwd through an
    // unrouted getpwuid_r. With HOME set it never gets there, so the check
    // has to take HOME away to mean anything.
    {
        let saved = env::var("HOME").ok();
        env::remove_var("HOME");
        #[allow(deprecated)]
        let home = env::home_dir();
        println!("home_dir() with HOME unset: {:?}  (via getpwuid_r)", home);
        if let Some(h) = saved {
            env::set_var("HOME", h);
        }
    }

    // 2. Threads. A native program is a function call on a guest task's
    //    thread; anything it spawns is a host thread with no task of its own,
    //    so this is the question of whether a worker can do guest work.
    println!("threads:");
    let (tx, rx) = std::sync::mpsc::channel();
    let mut handles = Vec::new();
    for i in 0..4u32 {
        let tx = tx.clone();
        handles.push(std::thread::spawn(move || {
            // Deliberately a guest filesystem read from the spawned thread,
            // not just arithmetic: the interesting failure is a worker with
            // no current task reaching the shim.
            let seen = fs::metadata("/AOK/VERSION").map(|m| m.len()).unwrap_or(0);
            tx.send((i, seen)).unwrap();
            i * i
        }));
    }
    drop(tx);
    let mut got: Vec<(u32, u64)> = rx.iter().collect();
    got.sort();
    println!("  channel results: {:?}", got);
    let squares: Vec<u32> = handles.into_iter().map(|h| h.join().unwrap()).collect();
    println!("  joined: {:?}", squares);

    // 3. Subprocesses. helix spawns language servers and formatters, so this
    //    is not optional for it. fork() is ENOSYS here, and Command has to
    //    land on the spawn path instead.
    println!("subprocess, via std::process::Command:");
    match std::process::Command::new("/bin/echo").arg("hello from a guest binary").output() {
        Ok(out) => println!("  /bin/echo -> status={} stdout={:?}",
                            out.status, String::from_utf8_lossy(&out.stdout).trim()),
        Err(e) => println!("  /bin/echo failed: {}", e),
    }
    match std::process::Command::new("/bin/sh").args(["-c", "echo $$ >/dev/null; id -u"]).output() {
        Ok(out) => println!("  /bin/sh -c 'id -u' -> status={} stdout={:?}",
                            out.status, String::from_utf8_lossy(&out.stdout).trim()),
        Err(e) => println!("  /bin/sh failed: {}", e),
    }

    // 3a. Which shape of Command reaches posix_spawn and which falls to
    //     fork() -- std picks between them from the Command's own settings,
    //     and the difference decides whether a program that spawns anything
    //     can run here at all.
    for (label, mk) in [
        ("status(), inherited stdio",
         (|| std::process::Command::new("/bin/true").status().map(|s| s.to_string()))
            as fn() -> std::io::Result<String>),
        ("status(), stdout to null",
         || std::process::Command::new("/bin/true")
                .stdout(std::process::Stdio::null()).status().map(|s| s.to_string())),
        ("output(), piped",
         || std::process::Command::new("/bin/true").output().map(|o| o.status.to_string())),
        ("spawn()+wait, piped stdout",
         || std::process::Command::new("/bin/true")
                .stdout(std::process::Stdio::piped()).spawn()
                .and_then(|mut c| c.wait()).map(|s| s.to_string())),
    ] {
        match mk() {
            Ok(v) => println!("  {:<28} ok   {}", label, v),
            Err(e) => println!("  {:<28} FAIL {}", label, e),
        }
    }

    // 3b. The same spawn, done by hand. Command failing with ENOSYS says
    //     Rust reached fork(); it does not say whether the shim's spawn path
    //     works, and those are different bugs with different fixes.
    println!("posix_spawnp, called directly:");
    unsafe {
        let mut pid: c_int = -1;
        let path = b"/bin/echo\0";
        let a0 = b"/bin/echo\0";
        let a1 = b"direct spawn\0";
        let argv_v: [*const c_char; 3] = [a0.as_ptr() as _, a1.as_ptr() as _, core::ptr::null()];
        let envp_v: [*const c_char; 1] = [core::ptr::null()];
        let rc = posix_spawnp(&mut pid, path.as_ptr() as _,
                              core::ptr::null_mut(), core::ptr::null_mut(),
                              argv_v.as_ptr(), envp_v.as_ptr());
        println!("  posix_spawnp rc={} pid={}", rc, pid);
        if rc == 0 {
            let mut status: c_int = 0;
            let w = waitpid(pid, &mut status, 0);
            println!("  waitpid -> {} status={}", w, status);
        }
    }

    // 4. The terminal. A TUI needs to know it has one and how big it is; both
    //    answers have to come from the guest's tty, not the app's.
    println!("terminal:");
    unsafe {
        println!("  isatty(0,1,2) = {} {} {}", isatty(0), isatty(1), isatty(2));
        let mut ws = Winsize { row: 0, col: 0, xpixel: 0, ypixel: 0 };
        let rc = ioctl(1, TIOCGWINSZ, &mut ws as *mut Winsize);
        println!("  TIOCGWINSZ rc={} rows={} cols={}", rc, ws.row, ws.col);
    }

    // 5. Writing a file, then reading it back. Proves the write half, which
    //    every check above stops short of.
    let scratch = "/tmp/rust-native-probe.txt";
    match fs::write(scratch, b"round trip\n").and_then(|_| fs::read_to_string(scratch)) {
        Ok(v) => println!("write/read round trip: {:?}", v.trim()),
        Err(e) => println!("write/read round trip failed: {}", e),
    }
    let _ = fs::remove_file(scratch);

    // 6. tokio. The reason this is in a probe at all: on Apple targets its
    //    reactor is kqueue, and the shim routes neither kqueue nor epoll --
    //    so a runtime built here would be putting GUEST descriptor numbers
    //    into a HOST kqueue. Nothing about that errors on the way in, which
    //    is exactly why it is measured.
    //
    //    No unwrap anywhere below: this crate is panic=abort, and a panic in
    //    a native program takes the whole app down rather than the program.
    #[cfg(feature = "tokio-probe")]
    {
    println!("tokio:");
    match tokio::runtime::Builder::new_multi_thread().worker_threads(2).enable_all().build() {
        Err(e) => println!("  runtime build failed: {}", e),
        Ok(rt) => {
            println!("  runtime built");
            let t = std::time::Instant::now();
            rt.block_on(async {
                tokio::time::sleep(std::time::Duration::from_millis(150)).await;
            });
            println!("  time driver: sleep(150ms) returned after {:?}", t.elapsed());

            // The LSP pattern: a child process with a pipe the runtime polls.
            let out = rt.block_on(async {
                tokio::process::Command::new("/bin/echo")
                    .arg("async child")
                    .output()
                    .await
            });
            match out {
                Ok(o) => println!("  process driver: {:?} status={}",
                                  String::from_utf8_lossy(&o.stdout).trim(), o.status),
                Err(e) => println!("  process driver failed: {}", e),
            }
        }
    }
    }
    #[cfg(not(feature = "tokio-probe"))]
    println!("tokio: not built (cargo build --features tokio-probe; see Cargo.toml)");
    0
}

// Declared here rather than pulled from the libc crate on purpose: the rename
// pass rewrites symbols in this archive, and an extra crate is one more place
// for a libc import to enter from. These two are all the terminal questions
// need, and both are routed (nlibc_isatty, nlibc_ioctl).
#[repr(C)]
struct Winsize { row: u16, col: u16, xpixel: u16, ypixel: u16 }

// Darwin's TIOCGWINSZ. The shim translates it to the guest's number.
const TIOCGWINSZ: u64 = 0x40087468;

extern "C" {
    fn isatty(fd: c_int) -> c_int;
    fn ioctl(fd: c_int, request: u64, ...) -> c_int;
    fn posix_spawnp(pid: *mut c_int, file: *const c_char,
                    file_actions: *mut core::ffi::c_void,
                    attrp: *mut core::ffi::c_void,
                    argv: *const *const c_char,
                    envp: *const *const c_char) -> c_int;
    fn waitpid(pid: c_int, status: *mut c_int, options: c_int) -> c_int;
}
