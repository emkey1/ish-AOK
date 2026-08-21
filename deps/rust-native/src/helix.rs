//! helix as an AOK native program.
//!
//! This is helix-term/src/main.rs with two changes and no others: the tokio
//! runtime is built by hand instead of by #[tokio::main], because a native
//! program is a function call rather than a process with a main; and the exit
//! code is RETURNED rather than passed to std::process::exit, because
//! kernel/native.c turns the return value into the task's wait status and an
//! exit() here would be a second, redundant way to say the same thing.
use anyhow::{Context, Error, Result};
use helix_loader::VERSION_AND_GIT_HASH;
use helix_term::application::Application;
use helix_term::args::Args;
use helix_term::config::{Config, ConfigLoadError};
use std::ffi::{c_char, c_int};

fn setup_logging(verbosity: u64) -> Result<()> {
    let level = match verbosity {
        0 => log::LevelFilter::Warn,
        1 => log::LevelFilter::Info,
        2 => log::LevelFilter::Debug,
        _3_or_more => log::LevelFilter::Trace,
    };
    helix_term::logging::init_file(level, &helix_loader::log_file())?;
    Ok(())
}

async fn run() -> Result<i32> {
    let args = Args::parse_args().context("could not parse arguments")?;

    helix_loader::initialize_config_file(args.config_file.clone());
    helix_loader::initialize_log_file(args.log_file.clone());

    if args.display_help {
        print!("helix {}\n\nUSAGE:\n    hx [FLAGS] [files]...\n", VERSION_AND_GIT_HASH);
        return Ok(0);
    }
    if args.display_version {
        println!("helix {}", VERSION_AND_GIT_HASH);
        return Ok(0);
    }
    if args.health {
        if let Err(err) = helix_term::health::print_health(args.health_arg) {
            if err.kind() != std::io::ErrorKind::BrokenPipe {
                return Err(err.into());
            }
        }
        return Ok(0);
    }
    // Grammar fetching and building are deliberately NOT wired up: both shell
    // out to git and a C compiler to produce loadable objects, and a native
    // program has neither a toolchain to build them nor a dlopen that can see
    // the guest's filesystem. Saying so beats a command that appears to work.
    if args.fetch_grammars || args.build_grammars {
        eprintln!("hx: grammar fetch/build is not available in the native build");
        return Ok(1);
    }

    setup_logging(args.verbosity).context("failed to initialize logging")?;

    if let Some(path) = &args.working_directory {
        helix_stdx::env::set_current_working_dir(path)?;
    } else if let Some((path, _)) = args.files.first().filter(|p| p.0.is_dir()) {
        helix_stdx::env::set_current_working_dir(path)?;
    } else if let Err(err) = std::env::current_dir() {
        eprintln!("Couldn't determine the current working directory: {err}");
        return Ok(1);
    }

    let config = match Config::load_default() {
        Ok(config) => config,
        Err(ConfigLoadError::Error(err)) if err.kind() == std::io::ErrorKind::NotFound => {
            Config::default()
        }
        Err(ConfigLoadError::Error(err)) => return Err(Error::new(err)),
        Err(ConfigLoadError::BadConfig(err)) => {
            eprintln!("Bad config: {}", err);
            Config::default()
        }
    };

    let workspace_trust =
        helix_loader::workspace_trust::WorkspaceTrust::new((&config.editor.workspace_trust).into());
    let lang_loader = helix_core::config::user_lang_loader(&workspace_trust).unwrap_or_else(|err| {
        eprintln!("{}", err);
        helix_core::config::default_lang_loader()
    });

    let mut app = Application::new(args, config, lang_loader, workspace_trust)
        .context("unable to start Helix")?;
    let mut events = app.event_stream();
    app.run(&mut events).await
}

/// argv/argc arrive as C arrays, but nothing here reads them: helix parses its
/// own arguments through std::env::args, which on Apple reads _NSGetArgv --
/// routed to the guest's argv by the shim (kernel/native_libc.c).
#[no_mangle]
pub extern "C" fn helix_native_main(
    _argc: c_int,
    _argv: *const *const c_char,
    _envp: *const *const c_char,
) -> c_int {
    let rt = match tokio::runtime::Builder::new_multi_thread().enable_all().build() {
        Ok(rt) => rt,
        Err(e) => {
            eprintln!("hx: could not start the async runtime: {e}");
            return 1;
        }
    };
    match rt.block_on(run()) {
        Ok(code) => code,
        Err(e) => {
            eprintln!("hx: {e:?}");
            1
        }
    }
}
