import Foundation

#if canImport(AppIntents)
import AppIntents

// The Shortcuts "Run Command" action: execute one command headlessly in the
// AOK guest and hand the merged stdout+stderr back to the shortcut. Execution
// goes through ISHGuestCommandRunner (app/GuestCommandRunner.m), which runs
// `/AOK/native/zsh -c` via run_guest_command_capture_shell -- or, when "Open
// Everything as Default User" is on, `su - <account> -c` as the UID-1000
// account -- and takes care of boot, threading, and background-suspension
// safety.
@available(iOS 16.0, *)
struct ISHRunCommandIntent: AppIntent {
    static var title: LocalizedStringResource = "Run Command"
    static var description = IntentDescription(
        "Runs a shell command in the AOK-iSH guest system and returns its output (stdout and stderr merged). The command runs headlessly; the app does not need to be open. It runs as root, or as the default user account when \"Open Everything as Default User\" is enabled in Settings.")
    static var openAppWhenRun: Bool = false

    @Parameter(title: "Command")
    var command: String

    // Background-launched intents get limited runtime from iOS; long jobs
    // should run with the app in the foreground. The cap keeps a shortcut from
    // asking for more than the system will plausibly grant.
    @Parameter(title: "Timeout (seconds)", default: 20, inclusiveRange: (1, 120))
    var timeout: Int

    @Parameter(title: "Fail on Non-Zero Exit", default: false)
    var failOnNonzeroExit: Bool

    static var parameterSummary: some ParameterSummary {
        Summary("Run \(\.$command) in AOK-iSH") {
            \.$timeout
            \.$failOnNonzeroExit
        }
    }

    func perform() async throws -> some IntentResult & ReturnsValue<String> {
        guard UserPreferences.shared().shortcutsRunCommandsEnabled else {
            throw ISHRunCommandError.disabled
        }
        let trimmed = command.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else {
            throw ISHRunCommandError.emptyCommand
        }
        let outcome = await withCheckedContinuation { continuation in
            ISHGuestCommandRunner.runCommand(trimmed, timeoutSeconds: timeout) { outcome in
                continuation.resume(returning: outcome)
            }
        }
        if let reason = outcome.failureReason {
            throw ISHRunCommandError.failed(reason)
        }
        if outcome.timedOut {
            throw ISHRunCommandError.timedOut(seconds: timeout, partialOutput: Self.trimmed(outcome.output))
        }
        if !outcome.exited && outcome.termSignal != 0 {
            throw ISHRunCommandError.killed(signal: Int(outcome.termSignal), partialOutput: Self.trimmed(outcome.output))
        }
        if failOnNonzeroExit && outcome.statusUnknown {
            throw ISHRunCommandError.statusUnknown(output: Self.trimmed(outcome.output))
        }
        if failOnNonzeroExit && outcome.exited && outcome.exitCode != 0 {
            throw ISHRunCommandError.nonzeroExit(code: Int(outcome.exitCode), output: Self.trimmed(outcome.output))
        }
        // Past the cap the command still ran to its end; what it printed after
        // was dropped, and the result says so on a line of its own.
        if outcome.truncated {
            return .result(value: outcome.output + "\n[AOK-iSH: output truncated at \(Self.outputLimitKB) KB]")
        }
        return .result(value: outcome.output)
    }

    // GuestCommandRunner.m's kISHShortcutOutputLimitKB.
    private static let outputLimitKB = 256

    // Output can be up to 256 KB; error messages get a short excerpt only.
    private static func trimmed(_ output: String, limit: Int = 1024) -> String {
        output.count <= limit ? output : String(output.prefix(limit)) + "…"
    }
}

@available(iOS 16.0, *)
enum ISHRunCommandError: Error, CustomLocalizedStringResourceConvertible {
    case disabled
    case emptyCommand
    case failed(String)
    case timedOut(seconds: Int, partialOutput: String)
    case killed(signal: Int, partialOutput: String)
    case nonzeroExit(code: Int, output: String)
    case statusUnknown(output: String)

    var localizedStringResource: LocalizedStringResource {
        // Already-translated text, wrapped: a resource whose key is the
        // finished string falls back to that string wherever it is displayed.
        LocalizedStringResource(stringLiteral: message)
    }

    // Resolved here, in our own process, on purpose. App Intents hands a
    // thrown error to Shortcuts as an NSError and resolves the description
    // itself -- in `en`, whatever locale the resource carries. Logged on
    // 2026-11-15 as CFBundle `localizationNames: [en]` in this very process,
    // microseconds after Locale.current (ja_JP), Bundle.main's preferred
    // localizations (ja) and the resource's own locale (ja_JP) all said
    // Japanese, and String(localized:) in the same breath returned
    // 「コマンドが空です。」 So every one of these came out in English on
    // a Japanese iPad while the same action's title, summary and parameters --
    // which travel in Metadata.appintents and are localized by whoever shows
    // them -- were translated. Naming a bundle does not help: the lookup was
    // already landing in iSH-AOK.app, just under en.lproj.
    //
    // Keep the English written out inside each String(localized:) call. The
    // compiler extracts the catalogue keys from those literals, so folding
    // them into a helper that takes the literal would put every one of them
    // out of its reach and silently empty this surface from the catalogue
    // again. That is also why .failed resolves "\(reason)" rather than
    // returning `reason` directly: it keeps the extracted `%@` key, which the
    // catalogue carries as Do Not Translate.
    private var message: String {
        switch self {
        case .disabled:
            return String(localized: "Running commands from Shortcuts is turned off. Enable “Allow Shortcuts to Run Commands” in AOK-iSH’s settings.")
        case .emptyCommand:
            return String(localized: "The command is empty.")
        case .failed(let reason):
            return String(localized: "\(reason)")
        case .timedOut(let seconds, let partialOutput):
            return partialOutput.isEmpty
                ? String(localized: "The command timed out after \(seconds) seconds.")
                : String(localized: "The command timed out after \(seconds) seconds. Partial output: \(partialOutput)")
        case .killed(let signal, let partialOutput):
            return partialOutput.isEmpty
                ? String(localized: "The command was killed by signal \(signal).")
                : String(localized: "The command was killed by signal \(signal). Partial output: \(partialOutput)")
        case .nonzeroExit(let code, let output):
            return output.isEmpty
                ? String(localized: "The command exited with status \(code).")
                : String(localized: "The command exited with status \(code). Output: \(output)")
        case .statusUnknown(let output):
            return output.isEmpty
                ? String(localized: "The command's exit status could not be determined, so Fail on Non-Zero Exit cannot vouch for it.")
                : String(localized: "The command's exit status could not be determined, so Fail on Non-Zero Exit cannot vouch for it. Output: \(output)")
        }
    }
}
#endif
