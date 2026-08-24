import Foundation
import UIKit

#if canImport(AppIntents)
import AppIntents

@available(iOS 16.0, *)
private enum ISHShortcutScene {
    static let workspaceActivityType = "app.ish.scene.workspace"
    static let terminalActivityType = "app.ish.scene.terminal"
    static let workspaceToolKey = "WorkspaceTool"
    static let terminalDisplayModeKey = "TerminalDisplayMode"
    static let sessionShellDisplayMode = "session-shell"
    static let systemConsoleDisplayMode = "system-console"
    static let browserToolIdentifier = "browser"
    static let themesToolIdentifier = "themes"
    static let bootImagesToolIdentifier = "filesystems"
}

@available(iOS 16.0, *)
enum ISHShortcutDestination: String, AppEnum {
    case dashboard
    case sessionShell
    case systemConsole
    case browser
    case themes
    case bootImages

    static var typeDisplayRepresentation: TypeDisplayRepresentation = "Destination"

    static var caseDisplayRepresentations: [ISHShortcutDestination: DisplayRepresentation] = [
        .dashboard: "Workspace Dashboard",
        .sessionShell: "Session Shell",
        .systemConsole: "System Console",
        .browser: "Browser",
        .themes: "Themes",
        .bootImages: "Boot Images",
    ]
}

@available(iOS 16.0, *)
struct ISHOpenShortcutIntent: AppIntent {
    static var title: LocalizedStringResource = "Open iSH-AOK"
    static var openAppWhenRun: Bool = true

    // The default lives on the @Parameter, not in an init() assignment --
    // assigning a @Parameter in a custom init() is a documented way to clobber
    // the user's configured value. (Note: on unsigned simulator builds the
    // configured enum value still resolves to this default, because the
    // LinkServices registry refuses the ad-hoc bundle -- same environmental
    // cause as App Shortcut tiles failing there; both work with real signing.)
    @Parameter(title: "Destination", default: .dashboard)
    var destination: ISHShortcutDestination

    init() {}

    init(destination: ISHShortcutDestination) {
        self.destination = destination
    }

    @MainActor
    func perform() async throws -> some IntentResult {
        let activity: NSUserActivity
        switch destination {
        case .dashboard:
            activity = NSUserActivity(activityType: ISHShortcutScene.workspaceActivityType)
        case .browser:
            activity = NSUserActivity(activityType: ISHShortcutScene.workspaceActivityType)
            activity.userInfo = [ISHShortcutScene.workspaceToolKey: ISHShortcutScene.browserToolIdentifier]
        case .themes:
            activity = NSUserActivity(activityType: ISHShortcutScene.workspaceActivityType)
            activity.userInfo = [ISHShortcutScene.workspaceToolKey: ISHShortcutScene.themesToolIdentifier]
        case .bootImages:
            activity = NSUserActivity(activityType: ISHShortcutScene.workspaceActivityType)
            activity.userInfo = [ISHShortcutScene.workspaceToolKey: ISHShortcutScene.bootImagesToolIdentifier]
        case .sessionShell:
            activity = NSUserActivity(activityType: ISHShortcutScene.terminalActivityType)
            activity.userInfo = [ISHShortcutScene.terminalDisplayModeKey: ISHShortcutScene.sessionShellDisplayMode]
        case .systemConsole:
            activity = NSUserActivity(activityType: ISHShortcutScene.terminalActivityType)
            activity.userInfo = [ISHShortcutScene.terminalDisplayModeKey: ISHShortcutScene.systemConsoleDisplayMode]
        }

        // A connected scene takes the destination directly: UIKit's
        // requestSceneSessionActivation only delivers the activity when it
        // creates a scene (iPad), and silently drops it on a single-scene
        // device. Fall back to scene activation for cold starts / new windows.
        if !ISHApplyShortcutActivityToConnectedScene(activity) {
            UIApplication.shared.requestSceneSessionActivation(nil,
                                                               userActivity: activity,
                                                               options: nil) { error in
                NSLog("iSH-AOK shortcut activation failed: %@", error.localizedDescription)
            }
        }
        return .result()
    }
}

@available(iOS 16.0, *)
struct ISHAppShortcutsProvider: AppShortcutsProvider {
    static var shortcutTileColor: ShortcutTileColor = .teal

    static var appShortcuts: [AppShortcut] {
        AppShortcut(intent: ISHOpenShortcutIntent(destination: .dashboard),
                    phrases: [
                        "Open workspace in \(.applicationName)",
                        "Show dashboard in \(.applicationName)",
                    ],
                    shortTitle: "Dashboard",
                    systemImageName: "square.grid.2x2")
        AppShortcut(intent: ISHOpenShortcutIntent(destination: .sessionShell),
                    phrases: [
                        "Open session shell in \(.applicationName)",
                        "Open shell in \(.applicationName)",
                    ],
                    shortTitle: "Session Shell",
                    systemImageName: "terminal")
        AppShortcut(intent: ISHOpenShortcutIntent(destination: .systemConsole),
                    phrases: [
                        "Open system console in \(.applicationName)",
                        "Open console in \(.applicationName)",
                    ],
                    shortTitle: "System Console",
                    systemImageName: "server.rack")
        AppShortcut(intent: ISHOpenShortcutIntent(destination: .browser),
                    phrases: [
                        "Open browser in \(.applicationName)",
                        "Show browser in \(.applicationName)",
                    ],
                    shortTitle: "Browser",
                    systemImageName: "globe")
        AppShortcut(intent: ISHOpenShortcutIntent(destination: .themes),
                    phrases: [
                        "Open themes in \(.applicationName)",
                        "Show themes in \(.applicationName)",
                    ],
                    shortTitle: "Themes",
                    systemImageName: "paintpalette")
        AppShortcut(intent: ISHOpenShortcutIntent(destination: .bootImages),
                    phrases: [
                        "Open boot images in \(.applicationName)",
                        "Show boot images in \(.applicationName)",
                    ],
                    shortTitle: "Boot Images",
                    systemImageName: "externaldrive")
    }
}

@objc(ISHAppShortcutsBridge)
final class ISHAppShortcutsBridge: NSObject {
    @objc static func refreshAppShortcuts() {
        guard #available(iOS 16.0, *) else {
            return
        }
        ISHAppShortcutsProvider.updateAppShortcutParameters()
    }
}
#endif
