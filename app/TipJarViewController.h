//
//  TipJarViewController.h
//  AOK-Workstation
//
//  Settings > Leave a Tip: three tips, as In-App Purchases, that unlock
//  nothing. The StoreKit side is TipJar.swift.
//

#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

extern UIViewController *ISHCreateTipJarViewController(void);

// Called once, at launch: finishes a tip that completed while no screen was
// waiting for it, such as an Ask to Buy approved later.
extern void ISHTipJarStartObservingTransactions(void);

NS_ASSUME_NONNULL_END
