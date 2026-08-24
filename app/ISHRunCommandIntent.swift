import Foundation

#if canImport(AppIntents)
import AppIntents

// The Shortcuts "Run Command" action: execute one command headlessly in the
// AOK guest and hand the merged stdout+stderr back to the shortcut. Execution
// goes through ISHGuestCommandRunner (app/GuestCommandRunner.m), which runs
// `/AOK/native/zsh -c` via run_guest_command_capture_shell and takes care of
// boot, threading, and background-suspension safety.
@available(iOS 16.0, *)
struct ISHRunCommandIntent: AppIntent {
    static var title: LocalizedStringResource = "Run Command"
    static var description = IntentDescription(
        "Runs a shell command in the iSH-AOK guest system and returns its output (stdout and stderr merged). The command runs headlessly under /AOK/native/zsh; the app does not need to be open.")
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
        Summary("Run \(\.$command) in iSH-AOK") {
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
        if failOnNonzeroExit && outcome.exited && outcome.exitCode != 0 {
            throw ISHRunCommandError.nonzeroExit(code: Int(outcome.exitCode), output: Self.trimmed(outcome.output))
        }
        return .result(value: outcome.output)
    }

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

    var localizedStringResource: LocalizedStringResource {
        switch self {
        case .disabled:
            return "Running commands from Shortcuts is turned off. Enable “Allow Shortcuts to Run Commands” in iSH-AOK’s settings."
        case .emptyCommand:
            return "The command is empty."
        case .failed(let reason):
            return "\(reason)"
        case .timedOut(let seconds, let partialOutput):
            return partialOutput.isEmpty
                ? "The command timed out after \(seconds) seconds."
                : "The command timed out after \(seconds) seconds. Partial output: \(partialOutput)"
        case .killed(let signal, let partialOutput):
            return partialOutput.isEmpty
                ? "The command was killed by signal \(signal)."
                : "The command was killed by signal \(signal). Partial output: \(partialOutput)"
        case .nonzeroExit(let code, let output):
            return output.isEmpty
                ? "The command exited with status \(code)."
                : "The command exited with status \(code). Output: \(output)"
        }
    }
}
#endif
