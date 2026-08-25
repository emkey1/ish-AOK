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
use tree_house::tree_sitter::Grammar;
use tree_sitter_language::LanguageFn;

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

// ---------------------------------------------------------------- grammars
//
// helix normally opens a grammar as a shared library at runtime. Here it
// cannot: iOS refuses to load code that is not signed into the app bundle, and
// the file would live inside fakefs where the system loader cannot see it
// anyway. So the grammars are LINKED IN, and this table is what helix asks
// instead -- see helix_loader::grammar::set_static_grammars, and the
// `impl TryFrom<LanguageFn> for Grammar` that tree-house-bindings already had.
//
// A name that is not here returns None, and helix falls through to the shared
// library exactly as before -- which simply will not be found, so that
// language goes unhighlighted rather than failing.
//
// Adding a language is two lines: the crate in Cargo.toml, and its arm here.
// It also needs its queries under the runtime directory, which is what carries
// the highlight rules; a grammar with no queries parses and colours nothing.
fn static_grammar(name: &str) -> Option<Grammar> {
    // The names are helix's, from languages.toml -- `grammar = "..."` where it
    // differs from the language name, which is why markdown appears twice and
    // typescript's grammar is javascript's.
    let language: LanguageFn = match name {
        "rust" => tree_sitter_rust::LANGUAGE,
        "c" => tree_sitter_c::LANGUAGE,
        "python" => tree_sitter_python::LANGUAGE,
        "json" => tree_sitter_json::LANGUAGE,
        "bash" => tree_sitter_bash::LANGUAGE,
        "go" => tree_sitter_go::LANGUAGE,
        "javascript" => tree_sitter_javascript::LANGUAGE,
        "html" => tree_sitter_html::LANGUAGE,
        "css" => tree_sitter_css::LANGUAGE,
        "lua" => tree_sitter_lua::LANGUAGE,
        "markdown" => tree_sitter_md::LANGUAGE,
        "markdown_inline" => tree_sitter_md::INLINE_LANGUAGE,
        "toml" => tree_sitter_toml_ng::LANGUAGE,
        "yaml" => tree_sitter_yaml::LANGUAGE,
        "diff" => tree_sitter_diff::LANGUAGE,
        "make" => tree_sitter_make::LANGUAGE,
        _ => return None,
    };
    match Grammar::try_from(language) {
        Ok(grammar) => Some(grammar),
        Err(err) => {
            // An ABI mismatch between the grammar crate and tree-house is the
            // one thing that can go wrong here, and silently dropping to "no
            // highlighting for this language" would hide it.
            eprintln!("hx: built-in grammar {name} is unusable: {err}");
            None
        }
    }
}

async fn run() -> Result<i32> {
    // Before anything reads a language configuration.
    helix_loader::grammar::set_static_grammars(static_grammar);

    // Where the queries and themes live. helix's own search covers the user's
    // config directory and $HELIX_RUNTIME, then the directory beside the
    // executable -- and a native program has no executable, so without this
    // there is nowhere for a stock install to find either: the grammars would
    // parse and nothing would say what to paint, and `:theme` would offer only
    // the two themes compiled into helix itself.
    //
    // Only when the user has not said otherwise. Someone who sets
    // HELIX_RUNTIME means it, and helix searches the config directory ahead of
    // this anyway, so a theme dropped in ~/.config/helix/themes still wins.
    if std::env::var_os("HELIX_RUNTIME").is_none() {
        std::env::set_var("HELIX_RUNTIME", "/AOK/native/libs/helix");
    }

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
    let rt_id = rt.handle().id();
    let code = match rt.block_on(run()) {
        Ok(code) => code,
        Err(e) => {
            eprintln!("hx: {e:?}");
            1
        }
    };
    // The log sink holds a file this RUN opened, and per-process `log` state
    // would otherwise carry it into the next run, where its fd number means
    // something else (kernel/native.h, the third bullet). Closed here, on the
    // run's own thread, which is the only place that close is routed right.
    helix_term::logging::shutdown(rt_id);
    // Dropped explicitly for the same reason, before this function returns to
    // native_exec_run_pending and the task starts tearing down: the runtime's
    // kqueue and worker teardown must happen while the task's fd table is
    // still the one they were created in.
    drop(rt);
    code
}
