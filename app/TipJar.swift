//
//  TipJar.swift
//  AOK-Workstation
//
//  Settings > Leave a Tip, the StoreKit side; the screen is
//  TipJarViewController.m. Swift because StoreKit 2 is Swift-only.
//
//  A tip is a consumable In-App Purchase that unlocks nothing, which App Review
//  guideline 3.1.1 allows ("tip" the developer through In-App Purchase). Apple
//  takes the payment; the app never sees who paid.
//
//  Nothing here has user-visible text of its own: the tips' names,
//  descriptions and prices are the App Store's, already localised, and every
//  error message is the system's.
//

import StoreKit
import UIKit

@objc(AOKTipProduct)
public final class AOKTipProduct: NSObject {
    @objc public let identifier: String
    @objc public let displayName: String
    @objc public let detail: String
    @objc public let displayPrice: String
    fileprivate let product: Product

    fileprivate init(_ product: Product) {
        self.product = product
        identifier = product.id
        displayName = product.displayName
        detail = product.description
        displayPrice = product.displayPrice
    }
}

@objc(AOKTipOutcome)
public enum AOKTipOutcome: Int {
    case thanked = 0   // paid, and the transaction is finished
    case cancelled = 1 // the person closed the payment sheet
    case pending = 2   // waiting on Ask to Buy or the bank; see `updates`
    case failed = 3
}

@objc(AOKTipJar)
public final class AOKTipJar: NSObject {
    // The same three are in app/TipJar.storekit, for testing, and in App
    // Store Connect.
    @objc public static let productIdentifiers = [
        "aok_workstation.tip.small",
        "aok_workstation.tip.medium",
        "aok_workstation.tip.large",
    ]

    // A purchase that completes away from purchaseTip (Ask to Buy approved
    // later, or one the app quit in the middle of) arrives here, and StoreKit
    // delivers an unfinished one again at every launch until it is finished.
    // A tip has nothing to deliver, so finishing it is all there is to do.
    private static let updates = Task.detached(priority: .background) {
        for await result in Transaction.updates {
            await AOKTipJar.finish(result)
        }
    }

    @objc public static func startObservingTransactions() {
        _ = updates
    }

    // False when Screen Time, or a device profile, turns In-App Purchases off.
    @objc public static var canMakePayments: Bool {
        AppStore.canMakePayments
    }

    // completion runs on the main queue with the tips, cheapest first, or with
    // none and the system's description of what failed. None and no error
    // means the App Store knows none of the identifiers.
    @objc(loadTipsWithCompletion:)
    public static func loadTips(completion: @escaping ([AOKTipProduct], String?) -> Void) {
        Task { @MainActor in
            do {
                let products = try await Product.products(for: productIdentifiers)
                completion(products.sorted { $0.price < $1.price }.map(AOKTipProduct.init), nil)
            } catch {
                completion([], error.localizedDescription)
            }
        }
    }

    // The sheet that confirms the payment is the system's; scene says which
    // window it goes over. completion runs on the main queue.
    @objc(purchaseTip:inScene:completion:)
    public static func purchase(_ tip: AOKTipProduct, in scene: UIWindowScene?,
                                completion: @escaping (AOKTipOutcome, String?) -> Void) {
        Task { @MainActor in
            do {
                let result: Product.PurchaseResult
                if #available(iOS 17.0, *), let scene {
                    result = try await tip.product.purchase(confirmIn: scene)
                } else {
                    result = try await tip.product.purchase()
                }
                switch result {
                case .success(let verification):
                    await finish(verification)
                    completion(.thanked, nil)
                case .userCancelled:
                    completion(.cancelled, nil)
                case .pending:
                    completion(.pending, nil)
                @unknown default:
                    completion(.failed, nil)
                }
            } catch StoreKitError.userCancelled {
                completion(.cancelled, nil)
            } catch {
                completion(.failed, error.localizedDescription)
            }
        }
    }

    // An unverified one too. Its signature did not check out, but a tip
    // withholds nothing, and an unfinished transaction comes back at every
    // launch.
    private static func finish(_ result: VerificationResult<Transaction>) async {
        switch result {
        case .verified(let transaction), .unverified(let transaction, _):
            await transaction.finish()
        }
    }
}
