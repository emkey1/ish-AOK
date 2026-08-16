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

@interface RootsTableViewController ()
// Archives found in the shared /AOK/persist/roots directory, shown as the
// "Root Cached Filesystems" section. Cached so the table data source is stable
// within a reload; refreshed on appear and when roots change.
@property (nonatomic, copy) NSArray<NSURL *> *cachedRootArchives;
@end

@interface RootDetailViewController : UITableViewController <UIDocumentPickerDelegate, UITextFieldDelegate>

@property (nonatomic) NSString *rootName;
@property (nonatomic) NSURL *exportURL;

@property (weak, nonatomic) IBOutlet UITextField *nameField;
@property (weak, nonatomic) IBOutlet UILabel *deleteLabel;
@property (weak, nonatomic) IBOutlet UITableViewCell *deleteCell;

@end

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
    return Roots.instance.bundledRootChoices;
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

- (void)_configurePopoverForAlert:(UIAlertController *)alert sender:(id)sender {
    UIPopoverPresentationController *popover = alert.popoverPresentationController;
    if (popover != nil) {
        if ([sender isKindOfClass:UIBarButtonItem.class]) {
            popover.barButtonItem = sender;
        } else if ([sender isKindOfClass:UIView.class]) {
            popover.sourceView = sender;
            popover.sourceRect = ((UIView *) sender).bounds;
        } else {
            popover.sourceView = self.view;
            popover.sourceRect = CGRectMake(CGRectGetMidX(self.view.bounds), CGRectGetMidY(self.view.bounds), 1, 1);
        }
    }
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

    UIAlertController *alert =
        [UIAlertController alertControllerWithTitle:group[@"displayName"]
                                             message:@"Choose an architecture."
                                      preferredStyle:UIAlertControllerStyleActionSheet];
    for (NSDictionary<NSString *, NSString *> *choice in variants) {
        [alert addAction:[UIAlertAction actionWithTitle:[self _archChoiceActionTitle:choice]
                                                  style:UIAlertActionStyleDefault
                                                handler:^(__unused UIAlertAction *action) {
            [self _confirmBundledImportChoiceIfNeeded:choice];
        }]];
    }
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel"
                                              style:UIAlertActionStyleCancel
                                            handler:nil]];
    [self _configurePopoverForAlert:alert sender:sender];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)presentImportOptionsFromSender:(id)sender {
    UIAlertController *alert =
        [UIAlertController alertControllerWithTitle:@"Import Filesystem"
                                            message:@"Choose a distribution or import a root archive from Files."
                                     preferredStyle:UIAlertControllerStyleActionSheet];

    NSArray<NSDictionary<NSString *, id> *> *groups =
        [self.officialFamilyGroups arrayByAddingObjectsFromArray:self.communityFamilyGroups];
    for (NSDictionary<NSString *, id> *group in groups) {
        NSString *displayName = group[@"displayName"];
        NSString *title = [group[@"tier"] isEqualToString:@"community"]
            ? [NSString stringWithFormat:@"%@ (Community)", displayName]
            : displayName;
        [alert addAction:[UIAlertAction actionWithTitle:title
                                                  style:UIAlertActionStyleDefault
                                                handler:^(__unused UIAlertAction *action) {
            [self _chooseArchitectureForGroup:group sender:sender];
        }]];
    }

    [alert addAction:[UIAlertAction actionWithTitle:@"Browse Files…"
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
    }]];

    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel"
                                              style:UIAlertActionStyleCancel
                                            handler:nil]];

    [self _configurePopoverForAlert:alert sender:sender];
    [self presentViewController:alert animated:YES completion:nil];
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
    [self reloadCachedRootArchives];
    [Roots.instance observe:@[@"roots", @"defaultRoot", @"initialBundledRootImportInProgress", @"initialBundledRootImportError"]
                    options:0 owner:self usingBlock:^(typeof(self) self) {
        [self reloadCachedRootArchives];
        [self updateEmptyState];
        [self.tableView reloadData];
    }];
    [self updateEmptyState];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    // Re-scan /AOK/persist/roots: archives may have been dropped in from the guest
    // (wget/scp/Files) since this screen was last shown.
    [self reloadCachedRootArchives];
    [self.tableView reloadData];
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

- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
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

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
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
    NSString *mountPoint = isDefaultRoot
        ? @"/"
        : [@"/AOK/roots/" stringByAppendingString:rootName];
    BOOL mounted = mount_exists_at_point(mountPoint.UTF8String);
    cell.detailTextLabel.text = mounted
        ? [NSString stringWithFormat:@"Mounted at %@", mountPoint]
        : nil;

    if (isDefaultRoot) {
        cell.accessibilityTraits |= UIAccessibilityTraitSelected;
    } else {
        cell.accessibilityTraits &= ~UIAccessibilityTraitSelected;
    }
    cell.accessibilityLabel = mounted
        ? [NSString stringWithFormat:@"%@, mounted at %@", rootName, mountPoint]
        : [NSString stringWithFormat:@"%@, not mounted", rootName];
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
    return [self sectionShowsCachedRoots:indexPath.section];
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

@implementation RootDetailViewController

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

// Renaming or deleting a root moves or removes its backing store, so neither
// is allowed for a root the app is holding open: the one booted as / this
// session, and the one chosen to boot next (whose store the next launch will
// go looking for under the name recorded in the default). Those are usually
// the same root but need not be -- picking "Boot this" on another filesystem
// changes the default immediately while / stays where it is -- and checking
// only the default left the running root editable. Roots enforces this too;
// this just keeps the controls from offering something that will be refused.
- (BOOL)isInUseRoot {
    return self.isDefaultRoot ||
        [self.rootName isEqualToString:Roots.instance.bootedRoot];
}

- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    if (section == 2) { // delete
        if ([self.rootName isEqualToString:Roots.instance.bootedRoot])
            return @"This filesystem can't be deleted or renamed because it's currently mounted as the root.";
        if (self.isDefaultRoot)
            return @"This filesystem can't be deleted or renamed because it's the one set to boot next.";
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

- (void)bootThis {
    Roots.instance.defaultRoot = self.rootName;
    AppDelegate *appDelegate = (AppDelegate *) UIApplication.sharedApplication.delegate;
    if ([appDelegate isKindOfClass:AppDelegate.class]) {
        [appDelegate exitApp];
    } else {
        exit(0);
    }
}

- (void)deleteFilesystem {
    if (self.isInUseRoot)
        return;
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Really delete?"
                                                                   message:@"I can't be bothered to implement any undo or regret UI so this is irreversable."
                                                            preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Delete" style:UIAlertActionStyleDestructive handler:^(UIAlertAction *action) {
        NSError *error;
        if (![Roots.instance destroyRootNamed:self.rootName error:&error]) {
            [self presentError:error title:@"Delete failed"];
        } else {
            [self.navigationController popViewControllerAnimated:YES];
        }
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)dealloc {
    self.exportURL = nil; // get it deleted
}

@end
