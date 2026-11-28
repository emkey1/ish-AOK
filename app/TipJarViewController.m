//
//  TipJarViewController.m
//  AOK-Workstation
//
//  Settings > Leave a Tip. AOK-Workstation is free and stays the same for
//  everyone; a tip is a consumable In-App Purchase that unlocks nothing. The
//  names, descriptions and prices are the App Store's (TipJar.swift fetches
//  them), so they arrive in the person's language and currency; this screen
//  adds only the words around them.
//

#import "TipJarViewController.h"
#import "libiSH_AOKApp-Swift.h"

typedef NS_ENUM(NSInteger, ISHTipJarState) {
    ISHTipJarLoading,
    ISHTipJarLoaded,
    ISHTipJarUnavailable,  // the request failed, or the App Store had no tips
    ISHTipJarPurchasesOff, // Screen Time or a profile turned In-App Purchases off
};

enum {
    ISHTipJarIntroSection,
    ISHTipJarTipsSection,
    ISHTipJarSectionCount,
};

@interface ISHTipJarViewController : UITableViewController
@end

@implementation ISHTipJarViewController {
    ISHTipJarState _state;
    NSArray<AOKTipProduct *> *_tips;
    NSString *_unavailableReason;
    NSString *_purchasing; // the identifier of the tip whose payment sheet is up
}

- (instancetype)init {
    return [super initWithStyle:UITableViewStyleInsetGrouped];
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = NSLocalizedString(@"Leave a Tip", @"Title of the tip jar screen, where people can give the developer a tip, and of the Settings row that opens it");
    self.navigationItem.largeTitleDisplayMode = UINavigationItemLargeTitleDisplayModeNever;
    [self _load];
}

- (void)_load {
    if (!AOKTipJar.canMakePayments) {
        _state = ISHTipJarPurchasesOff;
        [self.tableView reloadData];
        return;
    }
    _state = ISHTipJarLoading;
    _unavailableReason = nil;
    [self.tableView reloadData];
    __weak typeof(self) weakSelf = self;
    [AOKTipJar loadTipsWithCompletion:^(NSArray<AOKTipProduct *> *tips, NSString *error) {
        [weakSelf _loadedTips:tips error:error];
    }];
}

- (void)_loadedTips:(NSArray<AOKTipProduct *> *)tips error:(nullable NSString *)error {
    _tips = tips;
    _unavailableReason = error;
    _state = tips.count != 0 ? ISHTipJarLoaded : ISHTipJarUnavailable;
    [self.tableView reloadData];
}

#pragma mark - Table

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView {
    return ISHTipJarSectionCount;
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    if (section == ISHTipJarTipsSection && _state == ISHTipJarLoaded)
        return _tips.count;
    return 1;
}

- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    if (section != ISHTipJarTipsSection || _state != ISHTipJarLoaded)
        return nil;
    return NSLocalizedString(@"Apple takes the payment and charges it to your Apple Account. AOK-Workstation receives nothing that identifies you.",
                             @"Tip jar screen: the note under the list of tips");
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    if (indexPath.section == ISHTipJarIntroSection) {
        UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:@"Intro"];
        if (!cell) {
            cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:@"Intro"];
            cell.selectionStyle = UITableViewCellSelectionStyleNone;
            cell.textLabel.numberOfLines = 0;
        }
        cell.textLabel.text = NSLocalizedString(@"AOK-Workstation is free, with no ads and nothing held back. If it is useful to you, a tip helps pay for its development. A tip unlocks nothing: every feature stays free for everyone.",
                                                @"Tip jar screen: the paragraph at the top, explaining what a tip is for");
        return cell;
    }

    UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:@"Tip"];
    if (!cell) {
        cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:@"Tip"];
        cell.textLabel.numberOfLines = 0;
        cell.detailTextLabel.numberOfLines = 0;
        cell.detailTextLabel.textColor = UIColor.secondaryLabelColor;
    }
    cell.textLabel.textColor = UIColor.labelColor;
    cell.selectionStyle = UITableViewCellSelectionStyleDefault;
    cell.accessoryView = nil;
    cell.accessibilityValue = nil;
    cell.accessibilityTraits = UIAccessibilityTraitNone;

    switch (_state) {
    case ISHTipJarLoaded: {
        AOKTipProduct *tip = _tips[indexPath.row];
        cell.textLabel.text = tip.displayName;
        cell.detailTextLabel.text = tip.detail.length != 0 ? tip.detail : nil;
        if ([_purchasing isEqualToString:tip.identifier]) {
            UIActivityIndicatorView *spinner = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium];
            [spinner startAnimating];
            cell.accessoryView = spinner;
        } else {
            UILabel *price = [UILabel new];
            price.text = tip.displayPrice;
            price.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
            price.adjustsFontForContentSizeCategory = YES;
            price.textColor = self.view.tintColor;
            [price sizeToFit];
            cell.accessoryView = price;
        }
        // The price is in the accessory view, which VoiceOver does not read
        // with the row.
        cell.accessibilityValue = tip.displayPrice;
        cell.accessibilityTraits = UIAccessibilityTraitButton;
        if (_purchasing != nil)
            cell.selectionStyle = UITableViewCellSelectionStyleNone;
        break;
    }
    case ISHTipJarLoading: {
        cell.textLabel.text = NSLocalizedString(@"Loading…", @"Tip jar status text while the App Store sends the tips and their prices");
        cell.detailTextLabel.text = nil;
        UIActivityIndicatorView *spinner = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium];
        [spinner startAnimating];
        cell.accessoryView = spinner;
        cell.selectionStyle = UITableViewCellSelectionStyleNone;
        break;
    }
    case ISHTipJarUnavailable: {
        cell.textLabel.text = NSLocalizedString(@"Tips are not available right now.", @"Tip jar status text when the App Store did not send the tips");
        NSString *retry = NSLocalizedString(@"Tap to try again.", @"Tip jar: second line of the row that says tips are not available; tapping the row asks the App Store again");
        cell.detailTextLabel.text = _unavailableReason.length != 0
            ? [NSString stringWithFormat:@"%@\n%@", _unavailableReason, retry]
            : retry;
        cell.accessibilityTraits = UIAccessibilityTraitButton;
        break;
    }
    case ISHTipJarPurchasesOff:
        cell.textLabel.text = NSLocalizedString(@"This device does not allow In-App Purchases.", @"Tip jar status text when Screen Time or a device profile has turned In-App Purchases off");
        cell.textLabel.textColor = UIColor.secondaryLabelColor;
        cell.detailTextLabel.text = nil;
        cell.selectionStyle = UITableViewCellSelectionStyleNone;
        break;
    }
    return cell;
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    if (indexPath.section != ISHTipJarTipsSection)
        return;
    if (_state == ISHTipJarUnavailable) {
        [self _load];
        return;
    }
    if (_state != ISHTipJarLoaded || _purchasing != nil || indexPath.row >= (NSInteger)_tips.count)
        return;

    AOKTipProduct *tip = _tips[indexPath.row];
    _purchasing = tip.identifier;
    [self.tableView reloadSections:[NSIndexSet indexSetWithIndex:ISHTipJarTipsSection] withRowAnimation:UITableViewRowAnimationNone];
    __weak typeof(self) weakSelf = self;
    [AOKTipJar purchaseTip:tip inScene:self.view.window.windowScene completion:^(AOKTipOutcome outcome, NSString *error) {
        [weakSelf _purchaseEndedWithOutcome:outcome error:error];
    }];
}

- (void)_purchaseEndedWithOutcome:(AOKTipOutcome)outcome error:(nullable NSString *)error {
    _purchasing = nil;
    [self.tableView reloadSections:[NSIndexSet indexSetWithIndex:ISHTipJarTipsSection] withRowAnimation:UITableViewRowAnimationNone];

    NSString *title = nil;
    NSString *message = nil;
    switch (outcome) {
    case AOKTipOutcomeThanked:
        title = NSLocalizedString(@"Thank You!", @"Tip jar: title of the alert after a tip went through");
        message = NSLocalizedString(@"Your tip helps keep AOK-Workstation free and in development.", @"Tip jar: message of the alert after a tip went through");
        break;
    case AOKTipOutcomePending:
        title = NSLocalizedString(@"Waiting for Approval", @"Tip jar: title of the alert when a tip needs approval first, from a parent (Ask to Buy) or a bank");
        message = NSLocalizedString(@"Your tip goes through once it is approved. Thank you!", @"Tip jar: message of the alert when a tip needs approval first");
        break;
    case AOKTipOutcomeFailed:
        title = NSLocalizedString(@"The Tip Did Not Go Through", @"Tip jar: title of the alert when a tip failed; nothing was charged");
        message = error.length != 0 ? error : NSLocalizedString(@"Please try again later.", @"Tip jar: message of the alert when a tip failed and the system gave no reason");
        break;
    case AOKTipOutcomeCancelled:
        return;
    }
    // Gone from the screen: the person has moved on, so nothing to say.
    if (self.view.window == nil || self.presentedViewController != nil)
        return;
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:title message:message preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"OK", @"Alert button") style:UIAlertActionStyleDefault handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

@end

UIViewController *ISHCreateTipJarViewController(void) {
    return [ISHTipJarViewController new];
}

void ISHTipJarStartObservingTransactions(void) {
    [AOKTipJar startObservingTransactions];
}
