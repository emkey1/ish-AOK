//
//  RootsTableViewController.m
//  iSH
//
//  Created by Theodore Dubois on 6/7/20.
//

#import "AppDelegate.h"
#import "Roots.h"
#import "RootsTableViewController.h"
#import "ProgressReportViewController.h"
#import "UIApplication+OpenURL.h"
#import "UIViewController+Extras.h"
#import "NSObject+SaneKVO.h"
#import "UserPreferences.h"
#import "WorkspaceViewController.h"
#include "kernel/fs.h"

@interface RootsTableViewController () <WorkspaceTextScaledPage>
// Archives found in the shared /AOK/persist/roots directory, shown as the
// "Root Cached Filesystems" section. Cached so the table data source is stable
// within a reload; refreshed on appear and when roots change.
@property (nonatomic, copy) NSArray<NSURL *> *cachedRootArchives;
@end

@interface RootDetailViewController : UITableViewController <UIDocumentPickerDelegate, UITextFieldDelegate, WorkspaceTextScaledPage>

@property (nonatomic) NSString *rootName;
@property (nonatomic) NSURL *exportURL;

@property (weak, nonatomic) IBOutlet UITextField *nameField;
@property (weak, nonatomic) IBOutlet UILabel *deleteLabel;
@property (weak, nonatomic) IBOutlet UITableViewCell *deleteCell;

@end

// A root the app is holding open: the one booted as / this session, or the one
// set to boot next. Neither can be renamed or deleted; Roots refuses both, and
// the controls should not offer what will be refused. See -[RootDetailView-
// Controller isInUseRoot] for why it is both and not only the default.
static BOOL RootNameIsInUse(NSString *rootName) {
    return [rootName isEqualToString:Roots.instance.defaultRoot] ||
        [rootName isEqualToString:Roots.instance.bootedRoot];
}

// Asks, then deletes. The one confirmation behind both ways of deleting a
// machine -- the Delete Filesystem row on its own screen and a swipe on its row
// in the list (#575) -- so the two say the same thing and refuse the same
// roots. The title names the root: from the list, a swipe can land on the row
// next to the one meant. done(YES) once the root is gone; done(NO) on Cancel,
// or on a failure, which has already been shown.
static void RootConfirmAndDelete(UIViewController *host, NSString *rootName,
                                 void (^done)(BOOL deleted)) {
    if (RootNameIsInUse(rootName)) {
        done(NO);
        return;
    }
    NSString *message = @"I can't be bothered to implement any undo or regret UI so this is irreversible.";
    // Its saved sessions go with it (Roots destroyRootNamed), so say so.
    NSUInteger sessions = ISHSessionCountForRoot(ISHSessionRootIdentityNamed(rootName));
    if (sessions > 0)
        message = [message stringByAppendingFormat:@"\n\n%@ saved session%@ from this filesystem will be deleted too.",
                   sessions == 1 ? @"The" : [NSString stringWithFormat:@"%lu", (unsigned long) sessions],
                   sessions == 1 ? @"" : @"s"];
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:[NSString stringWithFormat:@"Delete \u201c%@\u201d?", rootName]
                                                                   message:message
                                                            preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:^(UIAlertAction *action) {
        done(NO);
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Delete" style:UIAlertActionStyleDestructive handler:^(UIAlertAction *action) {
        NSError *error;
        if (![Roots.instance destroyRootNamed:rootName error:&error]) {
            [host presentError:error title:@"Delete failed"];
            done(NO);
        } else {
            done(YES);
        }
    }]];
    [host presentViewController:alert animated:YES completion:nil];
}

@implementation RootsTableViewController

- (BOOL)_bundledChoiceRequiresAMD64Bringup:(NSDictionary<NSString *, NSString *> *)choice {
    return [choice[@"guestABI"] isEqualToString:@"amd64"];
}

- (NSString *)_bundledChoiceSubtitle:(NSDictionary<NSString *, NSString *> *)choice {
    NSString *subtitle;
    if ([self _bundledChoiceRequiresAMD64Bringup:choice]) {
        subtitle = @"x86_64 (amd64) guest rootfs.";
    } else if ([choice[@"guestABI"] isEqualToString:@"arm64"]) {
        subtitle = @"arm64 (native AArch64) guest rootfs.";
    } else if ([choice[@"guestABI"] isEqualToString:@"riscv64"]) {
        subtitle = @"riscv64 (RISC-V) guest rootfs.";
    } else {
        subtitle = @"i386 guest rootfs.";
    }
    if ([Roots.instance bundledRootChoiceNeedsDownload:choice]) {
        NSString *size = choice[@"downloadSize"];
        subtitle = size.length != 0
            ? [subtitle stringByAppendingFormat:@" Downloads %@.", size]
            : [subtitle stringByAppendingString:@" Downloads on use."];
    }
    return subtitle;
}

- (void)_beginBundledImportChoice:(NSDictionary<NSString *, NSString *> *)choice {
    [self startBundledImportChoice:choice];
}

- (void)_confirmBundledImportChoiceIfNeeded:(NSDictionary<NSString *, NSString *> *)choice {
    // x86_64/amd64 roots import directly now, like i386 — no experimental confirmation prompt.
    [self _beginBundledImportChoice:choice];
}

- (void)_completeRootSelectionWithName:(NSString *)rootName {
    if (rootName.length == 0)
        return;
    Roots.instance.defaultRoot = rootName;
    if (self.rootSelectionHandler != nil)
        self.rootSelectionHandler(rootName);
}

- (NSArray<NSDictionary<NSString *, NSString *> *> *)bundledChoices {
    return Roots.instance.offeredRootChoices;
}

// Groups bundledChoices by distro family (kBundledRootFamilyKey in Roots.m) so
// each distro shows as one row, with architecture offered as a sub-choice
// instead of one flat row per distro_arch combination. Each group dictionary
// has "displayName" (NSString), "tier" (NSString), and "variants"
// (NSArray<NSDictionary> of the underlying choice dicts, in declared order).
- (NSArray<NSDictionary<NSString *, id> *> *)_familyGroupsForTier:(NSString *)tier {
    NSMutableArray<NSString *> *order = [NSMutableArray array];
    NSMutableDictionary<NSString *, NSMutableArray<NSDictionary<NSString *, NSString *> *> *> *byFamily = [NSMutableDictionary dictionary];
    for (NSDictionary<NSString *, NSString *> *choice in self.bundledChoices) {
        if (![choice[@"tier"] isEqualToString:tier])
            continue;
        NSString *family = choice[@"family"] ?: choice[@"identifier"];
        NSMutableArray<NSDictionary<NSString *, NSString *> *> *variants = byFamily[family];
        if (variants == nil) {
            variants = [NSMutableArray array];
            byFamily[family] = variants;
            [order addObject:family];
        }
        [variants addObject:choice];
    }
    NSMutableArray<NSDictionary<NSString *, id> *> *groups = [NSMutableArray array];
    for (NSString *family in order) {
        NSArray<NSDictionary<NSString *, NSString *> *> *variants = byFamily[family];
        NSString *displayName = variants.firstObject[@"familyDisplayName"] ?: variants.firstObject[@"displayName"];
        [groups addObject:@{
            @"family": family,
            @"displayName": displayName,
            @"tier": tier,
            @"variants": variants,
        }];
    }
    return groups;
}

- (NSArray<NSDictionary<NSString *, id> *> *)officialFamilyGroups {
    return [self _familyGroupsForTier:@"official"];
}

- (NSArray<NSDictionary<NSString *, id> *> *)communityFamilyGroups {
    return [self _familyGroupsForTier:@"community"];
}

- (BOOL)showsOfficialChoicesSection {
    return self.officialFamilyGroups.count != 0;
}

- (BOOL)showsCommunityChoicesSection {
    return self.communityFamilyGroups.count != 0;
}

- (BOOL)showsInstalledRootsSection {
    return Roots.instance.roots.count != 0;
}

- (BOOL)showsCachedRootsSection {
    return self.cachedRootArchives.count != 0;
}

- (void)reloadCachedRootArchives {
    self.cachedRootArchives = Roots.instance.cachedRootArchiveURLs;
}

// Sections appear in this fixed order, each shown only when non-empty:
// Installed Filesystems, Root Cached Filesystems, Official Distributions, Community Distributions.
- (NSInteger)installedRootsSectionIndex {
    return self.showsInstalledRootsSection ? 0 : NSNotFound;
}
- (NSInteger)cachedRootsSectionIndex {
    if (!self.showsCachedRootsSection)
        return NSNotFound;
    return self.showsInstalledRootsSection ? 1 : 0;
}
- (NSInteger)officialChoicesSectionIndex {
    if (!self.showsOfficialChoicesSection)
        return NSNotFound;
    NSInteger index = 0;
    if (self.showsInstalledRootsSection)
        index++;
    if (self.showsCachedRootsSection)
        index++;
    return index;
}
- (NSInteger)communityChoicesSectionIndex {
    if (!self.showsCommunityChoicesSection)
        return NSNotFound;
    NSInteger index = 0;
    if (self.showsInstalledRootsSection)
        index++;
    if (self.showsCachedRootsSection)
        index++;
    if (self.showsOfficialChoicesSection)
        index++;
    return index;
}

- (BOOL)sectionShowsInstalledRoots:(NSInteger)section {
    return self.showsInstalledRootsSection && section == self.installedRootsSectionIndex;
}

- (BOOL)sectionShowsCachedRoots:(NSInteger)section {
    return self.showsCachedRootsSection && section == self.cachedRootsSectionIndex;
}

- (BOOL)sectionShowsOfficialChoices:(NSInteger)section {
    return self.showsOfficialChoicesSection && section == self.officialChoicesSectionIndex;
}

- (BOOL)sectionShowsCommunityChoices:(NSInteger)section {
    return self.showsCommunityChoicesSection && section == self.communityChoicesSectionIndex;
}

- (NSIndexPath *)selectedIndexPathForSender:(id)sender {
    if ([sender isKindOfClass:UITableViewCell.class]) {
        return [self.tableView indexPathForCell:sender];
    }
    if ([sender isKindOfClass:UIGestureRecognizer.class]) {
        UIView *view = ((UIGestureRecognizer *) sender).view;
        if ([view isKindOfClass:UITableViewCell.class])
            return [self.tableView indexPathForCell:(UITableViewCell *) view];
    }
    return self.tableView.indexPathForSelectedRow;
}

- (void)finishInitialSelectionIfNeededFromEmptyState:(BOOL)wasInitialSelection {
    if (!wasInitialSelection || Roots.instance.needsInitialRootSelection)
        return;
    [NSNotificationCenter.defaultCenter postNotificationName:RootsDidFinishInitialSelectionNotification object:nil];
}

- (void)startBundledImportChoice:(NSDictionary<NSString *, NSString *> *)choice {
    NSString *identifier = choice[@"identifier"];
    NSString *displayName = choice[@"displayName"];
    BOOL wasInitialSelection = Roots.instance.needsInitialRootSelection;
    NSString *initialWindow = choice[@"initialWindow"];

    ProgressReportViewController *progressVC = [self.storyboard instantiateViewControllerWithIdentifier:@"progress"];
    progressVC.title = [NSString stringWithFormat:@"Importing %@", displayName];
    [self presentViewController:progressVC animated:YES completion:nil];

    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSError *error = nil;
        BOOL success = [Roots.instance importBundledRootChoice:identifier error:&error progressReporter:progressVC];
        dispatch_async(dispatch_get_main_queue(), ^{
            [progressVC dismissViewControllerAnimated:YES completion:^{
                if (!success) {
                    if (error != nil)
                        [self presentError:error title:@"Import failed"];
                    return;
                }
                NSString *currentInitialWindow =
                    [NSUserDefaults.standardUserDefaults stringForKey:kPreferenceInitialWindowKey];
                if (!self.choosesRootOnSelection &&
                    wasInitialSelection &&
                    ![currentInitialWindow isEqualToString:ISHInitialWindowWorkspaceValue] &&
                    ![currentInitialWindow isEqualToString:ISHInitialWindowWaylandValue] &&
                    ([initialWindow isEqualToString:@"terminal"] ||
                     [initialWindow isEqualToString:@"session-shell"])) {
                    [NSUserDefaults.standardUserDefaults setObject:initialWindow
                                                            forKey:kPreferenceInitialWindowKey];
                }
                if (self.choosesRootOnSelection) {
                    [self _completeRootSelectionWithName:Roots.instance.roots.lastObject];
                    return;
                }
                [self finishInitialSelectionIfNeededFromEmptyState:wasInitialSelection];
            }];
        });
    });
}

// One-line label for an architecture variant when offering it as a sub-choice
// under a distro-family row -- distinct from _bundledChoiceSubtitle, which is
// a full sentence used under a single-variant family's own row.
- (NSString *)_archChoiceActionTitle:(NSDictionary<NSString *, NSString *> *)choice {
    NSString *abi = choice[@"guestABI"];
    NSString *label;
    if ([abi isEqualToString:@"amd64"]) {
        label = @"x86_64 (amd64)";
    } else if ([abi isEqualToString:@"arm64"]) {
        label = @"arm64";
    } else if ([abi isEqualToString:@"riscv64"]) {
        label = @"riscv64";
    } else {
        label = @"i386";
    }
    if ([Roots.instance bundledRootChoiceNeedsDownload:choice]) {
        NSString *size = choice[@"downloadSize"];
        return size.length != 0
            ? [NSString stringWithFormat:@"%@ — Downloads %@", label, size]
            : [NSString stringWithFormat:@"%@ — Downloads on first use", label];
    }
    return [NSString stringWithFormat:@"%@ (Bundled)", label];
}

// Distro-family rows with more than one architecture variant present an
// architecture-choice action sheet instead of importing directly, so a distro
// with several supported guest ABIs doesn't need its own long-list row per ABI.
- (void)_chooseArchitectureForGroup:(NSDictionary<NSString *, id> *)group sender:(id)sender {
    NSArray<NSDictionary<NSString *, NSString *> *> *variants = group[@"variants"];
    if (variants.count <= 1) {
        if (variants.count == 1)
            [self _confirmBundledImportChoiceIfNeeded:variants.firstObject];
        return;
    }

    ISHActionSheet *alert = [ISHActionSheet actionSheetWithTitle:group[@"displayName"]
                                                         message:@"Choose an architecture."];
    for (NSDictionary<NSString *, NSString *> *choice in variants) {
        [alert addActionWithTitle:[self _archChoiceActionTitle:choice]
                            style:UIAlertActionStyleDefault
                          handler:^(__unused UIAlertAction *action) {
            [self _confirmBundledImportChoiceIfNeeded:choice];
        }];
    }
    [alert addActionWithTitle:@"Cancel"
                        style:UIAlertActionStyleCancel
                      handler:nil];
    [alert presentFromViewController:self source:sender];
}

- (void)presentImportOptionsFromSender:(id)sender {
    ISHActionSheet *alert = [ISHActionSheet actionSheetWithTitle:@"Import Filesystem"
                                                         message:@"Choose a distribution or import a root archive from Files."];

    NSArray<NSDictionary<NSString *, id> *> *groups =
        [self.officialFamilyGroups arrayByAddingObjectsFromArray:self.communityFamilyGroups];
    for (NSDictionary<NSString *, id> *group in groups) {
        NSString *displayName = group[@"displayName"];
        NSString *title = [group[@"tier"] isEqualToString:@"community"]
            ? [NSString stringWithFormat:@"%@ (Community)", displayName]
            : displayName;
        [alert addActionWithTitle:title
                            style:UIAlertActionStyleDefault
                          handler:^(__unused UIAlertAction *action) {
            [self _chooseArchitectureForGroup:group sender:sender];
        }];
    }

    [alert addActionWithTitle:@"Browse Files…"
                        style:UIAlertActionStyleDefault
                      handler:^(__unused UIAlertAction *action) {
        UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc]
                                                  initWithDocumentTypes:@[@"public.tar-archive", @"org.gnu.gnu-zip-archive", @"public.bzip2-archive"]
                                                  inMode:UIDocumentPickerModeImport];
        [self presentViewController:picker animated:YES completion:nil];
        if (@available(iOS 13, *)) {
            picker.shouldShowFileExtensions = YES;
        }
        picker.delegate = self;
    }];

    [alert addActionWithTitle:@"Cancel"
                        style:UIAlertActionStyleCancel
                      handler:nil];

    [alert presentFromViewController:self source:sender];
}

- (void)updateEmptyState {
    if (Roots.instance.roots.count != 0 || self.cachedRootArchives.count != 0 || self.bundledChoices.count != 0) {
        self.tableView.backgroundView = nil;
        self.tableView.scrollEnabled = YES;
        self.navigationItem.rightBarButtonItem.enabled = YES;
        return;
    }

    UILabel *label = [[UILabel alloc] init];
    label.numberOfLines = 0;
    label.textAlignment = NSTextAlignmentCenter;
    if (@available(iOS 13.0, *)) {
        label.textColor = UIColor.secondaryLabelColor;
    } else {
        label.textColor = UIColor.grayColor;
    }
    label.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    ISHWorkspaceScaleTextFont(label, ISHWorkspaceTextScaleForViewController(self));
    if (Roots.instance.initialBundledRootImportInProgress) {
        label.text = @"Extracting the bundled filesystem.\nThis can take a moment on first launch.";
    } else if (Roots.instance.initialBundledRootImportError != nil) {
        label.text = [NSString stringWithFormat:@"%@\n\nTap Import to add a filesystem manually after freeing space.",
                      Roots.instance.initialBundledRootImportError.localizedDescription];
    } else {
        label.text = @"No filesystems are available.\nTap Import to add a root filesystem.";
    }
    label.translatesAutoresizingMaskIntoConstraints = NO;

    UIView *container = [[UIView alloc] initWithFrame:self.tableView.bounds];
    UIActivityIndicatorView *spinner = nil;
    if (Roots.instance.initialBundledRootImportInProgress) {
        if (@available(iOS 13, *)) {
            spinner = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium];
        } else {
            spinner = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleGray];
        }
        spinner.translatesAutoresizingMaskIntoConstraints = NO;
        [spinner startAnimating];
        [container addSubview:spinner];
    }
    [container addSubview:label];

    NSMutableArray<NSLayoutConstraint *> *constraints = [NSMutableArray array];
    if (spinner != nil) {
        [constraints addObject:[spinner.centerXAnchor constraintEqualToAnchor:container.centerXAnchor]];
        [constraints addObject:[spinner.bottomAnchor constraintEqualToAnchor:label.topAnchor constant:-16]];
    }
    [constraints addObject:[label.centerXAnchor constraintEqualToAnchor:container.centerXAnchor]];
    [constraints addObject:[label.centerYAnchor constraintEqualToAnchor:container.centerYAnchor constant:spinner != nil ? 18 : 0]];
    [constraints addObject:[label.leadingAnchor constraintGreaterThanOrEqualToAnchor:container.leadingAnchor constant:24]];
    [constraints addObject:[label.trailingAnchor constraintLessThanOrEqualToAnchor:container.trailingAnchor constant:-24]];
    [NSLayoutConstraint activateConstraints:constraints];
    self.tableView.backgroundView = container;
    self.tableView.scrollEnabled = !Roots.instance.initialBundledRootImportInProgress;
    self.navigationItem.rightBarButtonItem.enabled = !Roots.instance.initialBundledRootImportInProgress;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    ISHSizeTableSectionTitlesOnMac(self.tableView);
    [self reloadCachedRootArchives];
    [Roots.instance observe:@[@"roots", @"defaultRoot", @"initialBundledRootImportInProgress", @"initialBundledRootImportError"]
                    options:0 owner:self usingBlock:^(typeof(self) self) {
        [self reloadCachedRootArchives];
        [self updateEmptyState];
        [self.tableView reloadData];
    }];
    // A refresh that lands while this screen is up rewrites the distribution
    // rows under the user, so redraw when the catalogue changes rather than
    // leaving them looking at the list the app was built with.
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(rootCatalogDidChange:)
                                               name:RootsCatalogDidChangeNotification
                                             object:nil];
    [self updateEmptyState];
}

- (void)rootCatalogDidChange:(__unused NSNotification *)notification {
    [self updateEmptyState];
    [self.tableView reloadData];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    // Re-scan /AOK/persist/roots: archives may have been dropped in from the guest
    // (wget/scp/Files) since this screen was last shown.
    [self reloadCachedRootArchives];
    [self.tableView reloadData];
    // Opening this screen is the moment the catalogue matters, so check for a
    // newer one. Rate-limited inside, so flipping back and forth is free.
    [Roots.instance refreshRootCatalogFromNetwork];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView {
    NSInteger sections = 0;
    if (self.showsInstalledRootsSection)
        sections++;
    if (self.showsCachedRootsSection)
        sections++;
    if (self.showsOfficialChoicesSection)
        sections++;
    if (self.showsCommunityChoicesSection)
        sections++;
    return MAX(sections, 1);
}
- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    if ([self sectionShowsInstalledRoots:section])
        return Roots.instance.roots.count;
    if ([self sectionShowsCachedRoots:section])
        return self.cachedRootArchives.count;
    if ([self sectionShowsOfficialChoices:section])
        return self.officialFamilyGroups.count;
    if ([self sectionShowsCommunityChoices:section])
        return self.communityFamilyGroups.count;
    return 0;
}

- (NSString *)tableView:(UITableView *)tableView titleForHeaderInSection:(NSInteger)section {
    if ([self sectionShowsInstalledRoots:section]) {
        if (self.showsOfficialChoicesSection || self.showsCommunityChoicesSection || self.showsCachedRootsSection)
            return @"Installed Filesystems";
        return self.choosesRootOnSelection ? @"Choose a Filesystem" : nil;
    }
    if ([self sectionShowsCachedRoots:section]) {
        return @"Root Cached Filesystems (/AOK/persist/roots)";
    }
    if ([self sectionShowsOfficialChoices:section]) {
        if (self.showsInstalledRootsSection || self.showsCachedRootsSection || self.showsCommunityChoicesSection)
            return @"Official Distributions";
        return @"Choose a Filesystem";
    }
    if ([self sectionShowsCommunityChoices:section]) {
        return @"Community Distributions";
    }
    return nil;
}

// Say why the Files app shows nothing, on the screen where a user would expect
// their filesystems to be browsable from. Silence here reads as a broken
// feature; the reason is a platform limit, not something they can fix.
static BOOL ISHRunningAsIOSAppOnMac(void) {
    if (@available(iOS 14.0, *)) {
        NSProcessInfo *info = NSProcessInfo.processInfo;
        return info.isiOSAppOnMac || info.isMacCatalystApp;
    }
    return NO;
}

- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    if ([self sectionShowsInstalledRoots:section]) {
        if (ISHRunningAsIOSAppOnMac()) {
            NSString *note = @"Browsing these from the Files app isn't available on a Mac — "
                              "Apple's File Provider framework doesn't support the kind of "
                              "extension iSH-AOK uses, so it has been turned off here rather "
                              "than left to fail. Everything else works as usual.";
            if (self.choosesRootOnSelection)
                return [@"Tap a filesystem to make it active and continue booting.\n\n" stringByAppendingString:note];
            return note;
        }
    }
    if ([self sectionShowsInstalledRoots:section] && self.choosesRootOnSelection) {
        return @"Tap a filesystem to make it active and continue booting.";
    }
    if ([self sectionShowsCachedRoots:section]) {
        return @"Archives in /AOK/persist/roots (shared across all filesystems). Tap one to install it as a new filesystem. Swipe to delete.";
    }
    if ([self sectionShowsOfficialChoices:section]) {
        if (!self.showsInstalledRootsSection)
            return @"Choose a distribution below (you'll be asked which architecture if more than one is available), or tap Import to browse for another archive.";
        return @"Maintained and regression-tested as part of iSH-AOK. Can be imported again at any time.";
    }
    if ([self sectionShowsCommunityChoices:section]) {
        return @"Contributed or experimental, without the same support guarantees as the official distributions above. Downloaded on first use into /AOK/persist/roots, where they can be deleted afterward.";
    }
    return nil;
}

// The row for the filesystem actually running as / is tinted, not merely
// annotated. A subtitle among identical-looking subtitles is something you
// read only if you already suspect the rows differ; the whole failure in
// issue #575 was someone reasonably not suspecting that, tapping the running
// root, and reading its disabled Delete button as "AOK cannot delete
// filesystems" rather than "not this one".
//
// Green rather than the tint colour: this says "live", and it must not be
// mistaken for selection or for the separate "default root" checkmark, which
// marks a preference about the NEXT launch and is a different row as often as
// not. Written as a dynamic provider so it resolves per trait collection --
// a single literal colour legible on white is close to invisible on black.
//
// The values are picked against the grouped-cell background each mode actually
// renders, measured from a screenshot rather than assumed: #FFFFFF in light
// and #2C2C2E in dark. The dark one is the reason these are not simply a
// colour and a darker version of it -- an earlier dark green sat at a 1.13
// luminance ratio against its neighbours, which is a tint you find only once
// you already know to look. These give roughly 1.31 (light) and 1.73 (dark),
// and keep the subtitle above the WCAG AA 4.5 threshold on both.
static UIColor *RootRowInUseBackgroundColor(void) {
    return [UIColor colorWithDynamicProvider:^UIColor *(UITraitCollection *traits) {
        return traits.userInterfaceStyle == UIUserInterfaceStyleDark
            ? [UIColor colorWithRed:0.08 green:0.36 blue:0.19 alpha:1.0]
            : [UIColor colorWithRed:0.76 green:0.92 blue:0.79 alpha:1.0];
    }];
}

static UIColor *RootRowInUseAccentColor(void) {
    return [UIColor colorWithDynamicProvider:^UIColor *(UITraitCollection *traits) {
        return traits.userInterfaceStyle == UIUserInterfaceStyleDark
            ? [UIColor colorWithRed:0.55 green:0.93 blue:0.65 alpha:1.0]
            : [UIColor colorWithRed:0.03 green:0.40 blue:0.16 alpha:1.0];
    }];
}

// At the text size of the Workspace window this list is in; see
// WorkspaceTextScaledPage. Anywhere else the rows are left as they are. Applied
// after the branch below has set its own fonts, to every branch alike, so a
// dequeued cell cannot carry one row's size into another.
- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    UITableViewCell *cell = [self unscaledTableView:tableView cellForRowAtIndexPath:indexPath];
    ISHWorkspaceScaleTableViewCell(cell, ISHWorkspaceTextScaleForViewController(self));
    return cell;
}

- (void)workspaceTextScaleDidChange {
    [self updateEmptyState];
    ISHWorkspaceRescaleTableView(self.tableView, ISHWorkspaceTextScaleForViewController(self));
}

- (UITableViewCell *)unscaledTableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    if ([self sectionShowsOfficialChoices:indexPath.section] || [self sectionShowsCommunityChoices:indexPath.section]) {
        UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:@"BundledRootChoice"];
        if (cell == nil)
            cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:@"BundledRootChoice"];
        NSDictionary<NSString *, id> *group = [self sectionShowsOfficialChoices:indexPath.section]
            ? self.officialFamilyGroups[indexPath.row]
            : self.communityFamilyGroups[indexPath.row];
        NSArray<NSDictionary<NSString *, NSString *> *> *variants = group[@"variants"];
        cell.textLabel.text = group[@"displayName"];
        if (variants.count == 1) {
            cell.detailTextLabel.text = [self _bundledChoiceSubtitle:variants.firstObject];
            cell.accessoryType = UITableViewCellAccessoryNone;
        } else {
            NSMutableArray<NSString *> *archLabels = [NSMutableArray array];
            for (NSDictionary<NSString *, NSString *> *choice in variants) {
                NSString *abi = choice[@"guestABI"];
                if ([abi isEqualToString:@"amd64"])
                    [archLabels addObject:@"x86_64"];
                else if (abi.length != 0)
                    [archLabels addObject:abi];
                else
                    [archLabels addObject:@"i386"];
            }
            cell.detailTextLabel.text = [NSString stringWithFormat:@"%lu architectures: %@",
                                          (unsigned long) variants.count,
                                          [archLabels componentsJoinedByString:@", "]];
            cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
        }
        cell.accessibilityTraits &= ~UIAccessibilityTraitSelected;
        cell.selectionStyle = UITableViewCellSelectionStyleDefault;
        return cell;
    }

    if ([self sectionShowsCachedRoots:indexPath.section]) {
        UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:@"CachedRootArchive"];
        if (cell == nil)
            cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:@"CachedRootArchive"];
        NSURL *archive = self.cachedRootArchives[indexPath.row];
        cell.textLabel.text = archive.lastPathComponent;
        NSNumber *size = nil;
        [archive getResourceValue:&size forKey:NSURLFileSizeKey error:nil];
        cell.detailTextLabel.text = size != nil
            ? [NSByteCountFormatter stringFromByteCount:size.longLongValue countStyle:NSByteCountFormatterCountStyleFile]
            : nil;
        cell.accessoryType = UITableViewCellAccessoryNone;
        cell.accessibilityTraits &= ~UIAccessibilityTraitSelected;
        cell.selectionStyle = UITableViewCellSelectionStyleDefault;
        return cell;
    }

    NSString *ident = @"Root";
    BOOL isDefaultRoot = [Roots.instance.roots[indexPath.row] isEqual:Roots.instance.defaultRoot];
    if (isDefaultRoot)
        ident = @"Default Root";
    NSString *rootName = Roots.instance.roots[indexPath.row];
    UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:ident forIndexPath:indexPath];
    cell.textLabel.text = rootName;

    // "Mounted at <point>", and deliberately NOTHING when the root did not
    // mount. A root can pass every validity check the list is built from --
    // its directory, its data/ and its meta.db all exist -- and still fail to
    // mount, e.g. after its contents were deleted out from under it, leaving
    // meta.db unusable. Those entries then look exactly like healthy ones
    // here, which is how several came to sit in the list indefinitely with no
    // way to tell them apart. The subtitle's absence is the tell.
    //
    // Asked of the live mount table rather than repeating the checks the mount
    // attempt already made, because only the mount table knows whether the
    // attempt actually succeeded. The mount point directory is created before
    // do_mount runs, so its existence proves nothing.
    //
    // Which root is at / is bootedRoot, NOT defaultRoot. They are the same
    // until the user picks a different default, and from then until the next
    // launch the list said the newly-chosen root was mounted at / -- it isn't
    // yet -- and filed the root actually running underneath them in with the
    // ordinary ones under /AOK/roots. Every entry then reads "Mounted at ...",
    // so the one that cannot be deleted looks exactly like the ones that can.
    // That is how someone came to try deleting the filesystem they had booted
    // from, and to conclude from the disabled button that AOK had no delete
    // feature at all (issue #575).
    //
    // Falls back to defaultRoot only while bootedRoot is still nil, which is
    // the pre-boot state; once the app has booted a root it is authoritative.
    NSString *bootedRoot = Roots.instance.bootedRoot;
    BOOL isBootedRoot = bootedRoot != nil
        ? [rootName isEqualToString:bootedRoot]
        : isDefaultRoot;
    NSString *mountPoint = isBootedRoot
        ? @"/"
        : [@"/AOK/roots/" stringByAppendingString:rootName];
    BOOL mounted = mount_exists_at_point(mountPoint.UTF8String);
    // Say outright that this one is the running system and why it is the one
    // entry that will not delete, rather than leaving the disabled button to
    // be interpreted -- and colour the row so the difference is visible before
    // anything is read at all.
    //
    // Both branches set every property the other one touches. These cells come
    // from dequeueReusableCellWithIdentifier, so a tint applied here and not
    // undone reappears on whichever ordinary root later inherits the cell --
    // which would point at the wrong filesystem, a worse failure than the one
    // being fixed. Keyed on isBootedRoot alone rather than (mounted &&
    // isBootedRoot): if the running root ever fails to show up in the mount
    // table, losing the warning is the last thing that should happen.
    //
    // The point sizes match the storyboard prototype (17 and 11); the bold
    // weight and the accent are the only differences, so nothing reflows
    // between the two states. A Workspace window's text size scales both
    // alike afterwards, in -tableView:cellForRowAtIndexPath:.
    if (isBootedRoot) {
        cell.backgroundColor = RootRowInUseBackgroundColor();
        cell.textLabel.font = [UIFont boldSystemFontOfSize:17];
        cell.detailTextLabel.font = [UIFont boldSystemFontOfSize:11];
        cell.detailTextLabel.textColor = RootRowInUseAccentColor();
        // A filled dot as well as the colour: the same distinction has to
        // survive greyscale, and a colour-blind reader gets no cue from green.
        cell.detailTextLabel.text =
            @"\u25cf IN USE \u2014 mounted at / \u00b7 can't be deleted";
    } else {
        // secondarySystemGrouped, not nil: this is a grouped table, and a nil
        // background is transparent rather than default -- the cell would lose
        // its card and show the table's own backdrop through.
        cell.backgroundColor = UIColor.secondarySystemGroupedBackgroundColor;
        cell.textLabel.font = [UIFont systemFontOfSize:17];
        cell.detailTextLabel.font = [UIFont systemFontOfSize:11];
        cell.detailTextLabel.textColor = UIColor.secondaryLabelColor;
        NSString *where = mounted ? [NSString stringWithFormat:@"Mounted at %@", mountPoint] : nil;
        // The default that is not yet running -- "Next Launch" on its screen
        // leaves exactly this -- needs saying, or the choice is invisible
        // until the next launch acts on it.
        if (isDefaultRoot)
            where = where != nil ? [@"Boots next \u00b7 " stringByAppendingString:where] : @"Boots next";
        cell.detailTextLabel.text = where;
    }

    if (isDefaultRoot) {
        cell.accessibilityTraits |= UIAccessibilityTraitSelected;
    } else {
        cell.accessibilityTraits &= ~UIAccessibilityTraitSelected;
    }
    if (isBootedRoot)
        cell.accessibilityLabel = [NSString stringWithFormat:
            @"%@, in use, mounted at /, can't be deleted", rootName];
    else if (mounted)
        cell.accessibilityLabel = [NSString stringWithFormat:
            @"%@%@, mounted at %@", rootName, isDefaultRoot ? @", boots next" : @"", mountPoint];
    else
        cell.accessibilityLabel = [NSString stringWithFormat:@"%@%@, not mounted",
            rootName, isDefaultRoot ? @", boots next" : @""];
    return cell;
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    if ([self sectionShowsOfficialChoices:indexPath.section]) {
        UITableViewCell *cell = [tableView cellForRowAtIndexPath:indexPath];
        [tableView deselectRowAtIndexPath:indexPath animated:YES];
        [self _chooseArchitectureForGroup:self.officialFamilyGroups[indexPath.row] sender:cell];
        return;
    }
    if ([self sectionShowsCommunityChoices:indexPath.section]) {
        UITableViewCell *cell = [tableView cellForRowAtIndexPath:indexPath];
        [tableView deselectRowAtIndexPath:indexPath animated:YES];
        [self _chooseArchitectureForGroup:self.communityFamilyGroups[indexPath.row] sender:cell];
        return;
    }
    if ([self sectionShowsCachedRoots:indexPath.section]) {
        [tableView deselectRowAtIndexPath:indexPath animated:YES];
        if (indexPath.row < (NSInteger) self.cachedRootArchives.count)
            [self importArchiveAtURL:self.cachedRootArchives[indexPath.row] securityScoped:NO];
        return;
    }
    if (self.choosesRootOnSelection && [self sectionShowsInstalledRoots:indexPath.section]) {
        [tableView deselectRowAtIndexPath:indexPath animated:YES];
        [self _completeRootSelectionWithName:Roots.instance.roots[indexPath.row]];
        return;
    }
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
}

- (BOOL)tableView:(UITableView *)tableView canEditRowAtIndexPath:(NSIndexPath *)indexPath {
    return [self sectionShowsCachedRoots:indexPath.section] ||
        [self deletableRootAtIndexPath:indexPath] != nil;
}

// #575: deleting a machine used to be reachable only from its own screen, and
// swiping its row here did nothing at all -- only the cached archives could be
// edited -- so the list read as having no delete. The row the app is holding
// open (see RootNameIsInUse) still has none; its own row already says why.
- (NSString *)deletableRootAtIndexPath:(NSIndexPath *)indexPath {
    if (self.choosesRootOnSelection || ![self sectionShowsInstalledRoots:indexPath.section])
        return nil;
    NSOrderedSet<NSString *> *roots = Roots.instance.roots;
    if (indexPath.row < 0 || indexPath.row >= (NSInteger) roots.count)
        return nil;
    NSString *rootName = roots[indexPath.row];
    return RootNameIsInUse(rootName) ? nil : rootName;
}

- (UISwipeActionsConfiguration *)tableView:(UITableView *)tableView
    trailingSwipeActionsConfigurationForRowAtIndexPath:(NSIndexPath *)indexPath {
    NSString *rootName = [self deletableRootAtIndexPath:indexPath];
    // nil keeps the standard Delete (commitEditingStyle) for the cached archives.
    if (rootName == nil)
        return nil;
    UIContextualAction *delete = [UIContextualAction
        contextualActionWithStyle:UIContextualActionStyleDestructive
                            title:@"Delete"
                          handler:^(UIContextualAction *action, UIView *sourceView, void (^completion)(BOOL)) {
        // NO, and at once: the answer comes from the alert, and a Delete there
        // removes the row through the roots observer's reload. A destructive
        // action that reported YES would have the table remove it as well.
        completion(NO);
        RootConfirmAndDelete(self, rootName, ^(BOOL deleted) {});
    }];
    UISwipeActionsConfiguration *config = [UISwipeActionsConfiguration configurationWithActions:@[delete]];
    config.performsFirstActionWithFullSwipe = NO;
    return config;
}

- (void)tableView:(UITableView *)tableView commitEditingStyle:(UITableViewCellEditingStyle)editingStyle forRowAtIndexPath:(NSIndexPath *)indexPath {
    if (editingStyle != UITableViewCellEditingStyleDelete)
        return;
    if (![self sectionShowsCachedRoots:indexPath.section])
        return;
    if (indexPath.row >= (NSInteger) self.cachedRootArchives.count)
        return;
    NSURL *archiveURL = self.cachedRootArchives[indexPath.row];
    NSError *error = nil;
    if (![NSFileManager.defaultManager removeItemAtURL:archiveURL error:&error]) {
        [self presentError:error title:@"Delete failed"];
        return;
    }
    // A full reload rather than an animated row delete: removing the last
    // cached archive changes section membership (showsCachedRootsSection),
    // which an animated single-row delete can't express safely.
    [self reloadCachedRootArchives];
    [tableView reloadData];
}

- (void)prepareForSegue:(UIStoryboardSegue *)segue sender:(id)sender {
    NSIndexPath *indexPath = [self selectedIndexPathForSender:sender];
    if (indexPath == nil || ![self sectionShowsInstalledRoots:indexPath.section])
        return;
    RootDetailViewController *vc = segue.destinationViewController;
    vc.rootName = Roots.instance.roots[indexPath.row];
}

- (BOOL)shouldPerformSegueWithIdentifier:(NSString *)identifier sender:(id)sender {
    NSIndexPath *indexPath = [self selectedIndexPathForSender:sender];
    if (indexPath != nil && ([self sectionShowsOfficialChoices:indexPath.section] || [self sectionShowsCommunityChoices:indexPath.section]))
        return NO;
    if (indexPath != nil && [self sectionShowsCachedRoots:indexPath.section])
        return NO;
    if (self.choosesRootOnSelection && indexPath != nil && [self sectionShowsInstalledRoots:indexPath.section])
        return NO;
    return [super shouldPerformSegueWithIdentifier:identifier sender:sender];
}

- (IBAction)importFilesystem:(id)sender {
    [self presentImportOptionsFromSender:self.navigationItem.rightBarButtonItem ?: sender];
}

- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    NSAssert(urls.count == 1, @"somehow picked multiple documents");
    // Document-picker URLs are security-scoped; cached /root/roots archives live
    // in our own container and aren't.
    [self importArchiveAtURL:urls.firstObject securityScoped:YES];
}

- (void)importArchiveAtURL:(NSURL *)url securityScoped:(BOOL)securityScoped {
    NSString *fileName = url.lastPathComponent.stringByDeletingPathExtension;
    if ([fileName hasSuffix:@".tar"])
        fileName = fileName.stringByDeletingPathExtension;
    // Replace characters RootNameIsValid rejects (spaces, etc.) so an archive
    // named "my backup.tar.gz" imports instead of failing name validation.
    static NSCharacterSet *disallowed;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        disallowed = [[NSCharacterSet characterSetWithCharactersInString:
            @"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-"] invertedSet];
    });
    fileName = [[fileName componentsSeparatedByCharactersInSet:disallowed] componentsJoinedByString:@"_"];
    while ([fileName hasPrefix:@"."])
        fileName = [fileName substringFromIndex:1];
    if (fileName.length == 0)
        fileName = @"imported";
    unsigned i = 2;
    NSString *name = fileName;
    while ([Roots.instance.roots containsObject:name]) {
        // Use '_' (not a space) so the deduped name still passes RootNameIsValid.
        name = [NSString stringWithFormat:@"%@_%u", fileName, i++];
    }

    ProgressReportViewController *progressVC = [self.storyboard instantiateViewControllerWithIdentifier:@"progress"];
    progressVC.title = [NSString stringWithFormat:@"Importing %@", name];
    [self presentViewController:progressVC animated:YES completion:nil];
    BOOL wasInitialSelection = Roots.instance.needsInitialRootSelection;

    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSError *error;
        if (securityScoped)
            [url startAccessingSecurityScopedResource];
        BOOL success = [Roots.instance importRootFromArchive:url name:name error:&error progressReporter:progressVC];
        if (securityScoped)
            [url stopAccessingSecurityScopedResource];
        dispatch_async(dispatch_get_main_queue(), ^{
            [progressVC dismissViewControllerAnimated:YES completion:^{
                if (!success) {
                    if (error != nil)
                        [self presentError:error title:@"Import failed"];
                    return;
                }
                if (self.choosesRootOnSelection) {
                    [self _completeRootSelectionWithName:name];
                    return;
                }
                [self finishInitialSelectionIfNeededFromEmptyState:wasInitialSelection];
            }];
        });
    });
}

@end

@implementation RootDetailViewController {
    CGFloat _nameFieldInset;   // the storyboard's, before any text scale
    BOOL _hasNameFieldInset;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    ISHSizeTableSectionTitlesOnMac(self.tableView);
}

- (void)viewWillAppear:(BOOL)animated {
    self.nameField.text = self.rootName;
    [self update];
}

- (void)update {
    BOOL locked = self.isInUseRoot;
    self.navigationItem.title = self.rootName;
    self.nameField.enabled = !locked;
    self.nameField.clearButtonMode = locked ? UITextFieldViewModeNever : UITextFieldViewModeAlways;
    self.nameField.accessibilityLabel = @"Filesystem Name";
    self.deleteLabel.enabled = !locked;
    self.deleteCell.selectionStyle = !locked ? UITableViewCellSelectionStyleDefault : UITableViewCellSelectionStyleNone;
    [self.tableView reloadData];
}

// At the text size of the Workspace window this page is in; see
// WorkspaceTextScaledPage. Anywhere else the rows are left as they are.
- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    UITableViewCell *cell = [super tableView:tableView cellForRowAtIndexPath:indexPath];
    if (indexPath.section == 1 && indexPath.row == 0) {
        BOOL inert = self.bootRowIsInert;
        for (UIView *view in cell.contentView.subviews)
            if ([view isKindOfClass:UILabel.class])
                ((UILabel *) view).enabled = !inert;
        cell.selectionStyle = inert ? UITableViewCellSelectionStyleNone : UITableViewCellSelectionStyleDefault;
        if (inert)
            cell.accessibilityTraits |= UIAccessibilityTraitNotEnabled;
        else
            cell.accessibilityTraits &= ~UIAccessibilityTraitNotEnabled;
    }
    CGFloat scale = ISHWorkspaceTextScaleForViewController(self);
    ISHWorkspaceScaleTableViewCell(cell, scale);
    if ([self.nameField isDescendantOfView:cell])
        [self scaleNameFieldInset:scale];
    return cell;
}

// Every row is one line, but the Name row also lays out its field with
// constraints that do not reach the bottom of the cell, so its height cannot be
// trusted to follow the text. 44 points at the scale holds a line at every step.
- (CGFloat)tableView:(UITableView *)tableView heightForRowAtIndexPath:(NSIndexPath *)indexPath {
    return ISHWorkspaceTextScaledRowHeight([super tableView:tableView heightForRowAtIndexPath:indexPath],
                                           ISHWorkspaceTextScaleForViewController(self));
}

- (void)workspaceTextScaleDidChange {
    CGFloat scale = ISHWorkspaceTextScaleForViewController(self);
    [self scaleNameFieldInset:scale];
    ISHWorkspaceRescaleTableView(self.tableView, scale);
}

// The storyboard starts the name field 75 points in, room for "Name" at 17
// points. A larger "Name" would run under a long filesystem name.
- (void)scaleNameFieldInset:(CGFloat)scale {
    if (!_hasNameFieldInset && scale <= 1.0)
        return;
    for (NSLayoutConstraint *constraint in self.nameField.superview.constraints) {
        if (constraint.firstItem != self.nameField || constraint.firstAttribute != NSLayoutAttributeLeading)
            continue;
        if (!_hasNameFieldInset) {
            _nameFieldInset = constraint.constant;
            _hasNameFieldInset = YES;
        }
        constraint.constant = _nameFieldInset * MAX(1.0, scale);
    }
}

- (IBAction)nameChanged:(id)sender {
    NSString *newName = self.nameField.text;
    NSError *err;
    if (![Roots.instance renameRoot:self.rootName toName:newName error:&err]) {
        self.nameField.text = self.rootName;
        [self presentError:err title:@"Rename failed"];
        return;
    }
    self.rootName = newName;
    [self update];
}

- (BOOL)textFieldShouldReturn:(UITextField *)textField {
    [textField resignFirstResponder];
    return NO;
}

- (BOOL)isDefaultRoot {
    return [self.rootName isEqualToString:Roots.instance.defaultRoot];
}

- (BOOL)isBootedRoot {
    return [self.rootName isEqualToString:Roots.instance.bootedRoot];
}

// Running from it and booting it next: Boot From This Filesystem has nothing
// left to do.
- (BOOL)bootRowIsInert {
    return self.isBootedRoot && self.isDefaultRoot;
}

// Renaming or deleting a root moves or removes its backing store, so neither
// is allowed for a root the app is holding open: the one booted as / this
// session, and the one chosen to boot next (whose store the next launch will
// go looking for under the name recorded in the default). Those are usually
// the same root but need not be -- picking "Boot this" on another filesystem
// changes the default immediately while / stays where it is -- and checking
// only the default left the running root editable. Roots enforces this too;
// this just keeps the controls from offering something that will be refused.
- (BOOL)isInUseRoot {
    return RootNameIsInUse(self.rootName);
}

- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    if (section == 1) { // boot
        if (self.bootRowIsInert)
            return @"iSH-AOK is running from this filesystem, and boots it next time too.";
        if (self.isBootedRoot)
            return @"iSH-AOK is running from this filesystem, but another one is set to boot next.";
        if (self.isDefaultRoot)
            return @"This filesystem boots the next time iSH-AOK opens.";
        return @"Takes effect the next time iSH-AOK opens. You choose whether that is now.";
    }
    if (section == 2) { // delete
        // The rule and then the way out of it (#575): without the second
        // sentence this read as "iSH-AOK cannot delete machines".
        if ([self.rootName isEqualToString:Roots.instance.bootedRoot])
            return @"This filesystem can't be deleted or renamed because it's currently mounted as the root. "
                   @"To delete it, choose Boot From This Filesystem on another one; once iSH-AOK is running from that one, this one can go.";
        if (self.isDefaultRoot)
            return @"This filesystem can't be deleted or renamed because it's the one set to boot next. "
                   @"To delete it, choose Boot From This Filesystem on another one first.";
    }
    return [super tableView:tableView titleForFooterInSection:section];
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    if (indexPath.section == 0 && indexPath.row == 1)
        [self browseFiles];
    if (indexPath.section == 0 && indexPath.row == 2)
        [self exportFilesystem];
    if (indexPath.section == 1 && indexPath.row == 0)
        [self bootThis];
    if (indexPath.section == 2 && indexPath.row == 0)
        [self deleteFilesystem];
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
}

- (void)browseFiles {
    // The root now lives one level down, inside the single "iSH-AOK" domain.
    NSURL *url = [[NSFileProviderManager.defaultManager.documentStorageURL
                   URLByAppendingPathComponent:@"iSH-AOK"]
                  URLByAppendingPathComponent:self.rootName];
    NSURLComponents *components = [NSURLComponents componentsWithURL:url resolvingAgainstBaseURL:NO];
    components.scheme = @"shareddocuments";
    [UIApplication openURL:components.string];
}

- (void)exportFilesystem {
    self.exportURL = [[NSFileManager.defaultManager.temporaryDirectory
                       URLByAppendingPathComponent:[NSProcessInfo.processInfo globallyUniqueString]]
                      URLByAppendingPathComponent:[NSString stringWithFormat:@"%@.tar.gz", self.rootName]];
    [NSFileManager.defaultManager createDirectoryAtURL:self.exportURL.URLByDeletingLastPathComponent
                           withIntermediateDirectories:YES
                                            attributes:nil
                                                 error:nil];
    ProgressReportViewController *progressVC = [self.storyboard instantiateViewControllerWithIdentifier:@"progress"];
    progressVC.title = [NSString stringWithFormat:@"Exporting %@", self.rootName];
    [self presentViewController:progressVC animated:YES completion:nil];

    // witness the callback hell
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSError *err;
        BOOL success = [Roots.instance exportRootNamed:self.rootName toArchive:self.exportURL error:&err progressReporter:progressVC];
        dispatch_async(dispatch_get_main_queue(), ^{
            [progressVC dismissViewControllerAnimated:YES completion:^{
                if (!success) {
                    if (err != nil)
                        [self presentError:err title:@"Export failed"];
                    return;
                }

                UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc]
                                                          initWithURL:self.exportURL
                                                          inMode:UIDocumentPickerModeExportToService];
                picker.delegate = self;
                if (@available(iOS 13, *)) {
                    picker.shouldShowFileExtensions = YES;
                }
                [self presentViewController:picker animated:YES completion:nil];
            }];
        });
    });
}

- (void)setExportURL:(NSURL *)exportURL {
    [NSFileManager.defaultManager removeItemAtURL:_exportURL.URLByDeletingLastPathComponent error:nil];
    _exportURL = exportURL;
}

// This used to quit iSH-AOK on the spot: one tap, no question, every running
// program ended, under a footer that promised a restart iOS does not let an app
// perform -- it landed on the home screen. The choice only takes effect at a
// launch either way, so ask whether that launch is now or whenever it comes.
- (void)bootThis {
    if (self.bootRowIsInert)
        return;
    NSString *name = self.rootName;
    NSString *title, *message;
    BOOL offerLater = YES, offerQuit = YES;
    if (self.isBootedRoot) {
        // Running from it, with another set to boot next: all that is left to
        // choose is booting it next time too. Quitting would only bring the
        // same root back.
        title = [NSString stringWithFormat:@"Keep booting \u201c%@\u201d?", name];
        message = @"iSH-AOK is running from it now, but another filesystem is set to boot next. "
                  @"Next Launch makes this one boot next time instead.";
        offerQuit = NO;
    } else if (self.isDefaultRoot) {
        title = [NSString stringWithFormat:@"\u201c%@\u201d boots next", name];
        message = @"It boots the next time iSH-AOK opens. Quit Now closes iSH-AOK, ending every running program, "
                  @"so that it boots when you open it again.";
        offerLater = NO;
    } else {
        title = [NSString stringWithFormat:@"Boot \u201c%@\u201d from now on?", name];
        message = @"iSH-AOK can't restart itself. Next Launch keeps everything running and switches the next time "
                  @"iSH-AOK opens. Quit Now closes it, ending every running program, and boots this filesystem when "
                  @"you open it again.";
    }
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:title
                                                                   message:message
                                                            preferredStyle:UIAlertControllerStyleAlert];
    if (offerLater) {
        UIAlertAction *later = [UIAlertAction actionWithTitle:@"Next Launch" style:UIAlertActionStyleDefault handler:^(UIAlertAction *action) {
            Roots.instance.defaultRoot = name;
            [self update];
        }];
        [alert addAction:later];
        alert.preferredAction = later;
    }
    if (offerQuit) {
        [alert addAction:[UIAlertAction actionWithTitle:@"Quit Now" style:UIAlertActionStyleDestructive handler:^(UIAlertAction *action) {
            Roots.instance.defaultRoot = name;
            AppDelegate *appDelegate = (AppDelegate *) UIApplication.sharedApplication.delegate;
            if ([appDelegate isKindOfClass:AppDelegate.class]) {
                [appDelegate exitApp];
            } else {
                exit(0);
            }
        }]];
    }
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)deleteFilesystem {
    RootConfirmAndDelete(self, self.rootName, ^(BOOL deleted) {
        if (deleted)
            [self.navigationController popViewControllerAnimated:YES];
    });
}

- (void)dealloc {
    self.exportURL = nil; // get it deleted
}

@end
