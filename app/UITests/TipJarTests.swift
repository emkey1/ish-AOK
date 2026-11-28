//
//  TipJarTests.swift
//  AOK-Workstation
//
//  Settings > Leave a Tip, against app/TipJar.storekit: StoreKit's local test
//  store, so nothing is charged and App Store Connect is never asked. Each
//  test drives the real screen and the real purchase path; SKTestSession
//  plays the App Store's part, including the failures.
//

import StoreKitTest
import XCTest

final class TipJarTests: XCTestCase {
    private var session: SKTestSession!
    private var app: XCUIApplication!

    override func setUpWithError() throws {
        continueAfterFailure = false
        session = try SKTestSession(configurationFileNamed: "TipJar")
        session.disableDialogs = true
        session.askToBuyEnabled = false
        session.clearTransactions()
        app = XCUIApplication()
        app.launchEnvironment["ISH_SESSION_RESUME"] = "fresh"
    }

    override func tearDown() async throws {
        if #available(iOS 17.0, *) {
            try? await session.setSimulatedError(nil, forAPI: .purchase)
            try? await session.setSimulatedError(nil, forAPI: .loadProducts)
        }
        session.askToBuyEnabled = false
        session.clearTransactions()
        await MainActor.run { app.terminate() }
    }

    @MainActor
    private func openTipJar() {
        app.launch()
        let settings = app.buttons["Settings"]
        XCTAssert(settings.waitForExistence(timeout: 60), "no Settings button")
        settings.tap()
        let row = app.tables.staticTexts["Leave a Tip"]
        XCTAssert(row.waitForExistence(timeout: 10), "Settings has no Leave a Tip row")
        row.tap()
        XCTAssert(app.navigationBars["Leave a Tip"].waitForExistence(timeout: 10), "the tip jar did not open")
    }

    @MainActor
    private func keepScreenshot(_ name: String) {
        let attachment = XCTAttachment(screenshot: XCUIScreen.main.screenshot())
        attachment.name = name
        attachment.lifetime = .keepAlways
        add(attachment)
    }

    @MainActor
    private func waitForAlert(_ title: String) -> XCUIElement {
        let alert = app.alerts[title]
        XCTAssert(alert.waitForExistence(timeout: 30), "no \"\(title)\" alert")
        return alert
    }

    @MainActor
    func testTipsAreListedCheapestFirstAndATipIsThanked() throws {
        openTipJar()
        let small = app.tables.staticTexts["Small Tip"]
        XCTAssert(small.waitForExistence(timeout: 30), "the tips did not load")
        let names = ["Small Tip", "Medium Tip", "Large Tip"]
        let tops = names.map { app.tables.staticTexts[$0].frame.minY }
        XCTAssertEqual(tops, tops.sorted(), "the tips are not cheapest first")
        for price in ["$1.99", "$4.99", "$9.99"] {
            XCTAssert(app.tables.staticTexts[price].exists, "no price \(price)")
        }
        keepScreenshot("tip-jar")

        app.tables.staticTexts["Medium Tip"].tap()
        let alert = waitForAlert("Thank You!")
        keepScreenshot("tip-thanked")
        alert.buttons["OK"].tap()

        let bought = session.allTransactions()
        XCTAssertEqual(bought.map(\.productIdentifier), ["aok_workstation.tip.medium"])
        XCTAssertEqual(bought.first?.state, .purchased)
    }

    @MainActor
    func testAskToBuyWaitsForApproval() throws {
        session.askToBuyEnabled = true
        openTipJar()
        let small = app.tables.staticTexts["Small Tip"]
        XCTAssert(small.waitForExistence(timeout: 30), "the tips did not load")
        small.tap()
        let alert = waitForAlert("Waiting for Approval")
        keepScreenshot("tip-pending")
        alert.buttons["OK"].tap()

        let pending = try XCTUnwrap(session.allTransactions().first)
        XCTAssert(pending.pendingAskToBuyConfirmation)
        // Approved while the app runs: TipJar.swift's Transaction.updates
        // listener finishes it, and no alert comes up for it.
        try session.approveAskToBuyTransaction(identifier: pending.identifier)
        XCTAssertFalse(app.alerts.firstMatch.waitForExistence(timeout: 5))
    }

    @MainActor
    func testAFailedTipSaysSo() async throws {
        guard #available(iOS 17.0, *) else { throw XCTSkip("simulated errors need iOS 17") }
        try await session.setSimulatedError(.generic(.unknown), forAPI: .purchase)
        openTipJar()
        let large = app.tables.staticTexts["Large Tip"]
        XCTAssert(large.waitForExistence(timeout: 30), "the tips did not load")
        large.tap()
        let alert = waitForAlert("The Tip Did Not Go Through")
        keepScreenshot("tip-failed")
        alert.buttons["OK"].tap()
        XCTAssertEqual(session.allTransactions().count, 0)
    }

    @MainActor
    func testTipsThatFailToLoadCanBeAskedForAgain() async throws {
        guard #available(iOS 17.0, *) else { throw XCTSkip("simulated errors need iOS 17") }
        try await session.setSimulatedError(.generic(.networkError(URLError(.notConnectedToInternet))), forAPI: .loadProducts)
        openTipJar()
        let unavailable = app.tables.staticTexts["Tips are not available right now."]
        XCTAssert(unavailable.waitForExistence(timeout: 30), "no unavailable row")
        keepScreenshot("tips-unavailable")

        try await session.setSimulatedError(nil, forAPI: .loadProducts)
        unavailable.tap()
        XCTAssert(app.tables.staticTexts["Small Tip"].waitForExistence(timeout: 30), "trying again did not load the tips")
    }
}
