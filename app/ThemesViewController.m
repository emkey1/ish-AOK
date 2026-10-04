//
//  ThemesViewController.m
//  iSH
//
//  Created by Saagar Jha on 2/25/22.
//

#import "ThemesViewController.h"

#import "NSObject+SaneKVO.h"
#import "Theme.h"
#import "ThemeViewController.h"
#import "UIViewController+Extras.h"
#import "UserPreferences.h"
#import "WorkspaceViewController.h"

@interface ThemesViewController () <WorkspaceTextScaledPage>
@end

@implementation ThemesViewController {
    BOOL _singleRowEditing;
    BOOL _importButtonEditingMode;
    BOOL _pendingUpdate;
    Theme *_theme;
    NSMutableArray<Theme *> *_defaultThemes;
    NSMutableArray<Theme *> *_userThemes;
    BOOL _preferUserTheme;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    ISHSizeTableSectionTitlesOnMac(self.tableView);

    [UserPreferences.shared observe:@[@"theme"]
                            options:0 owner:self usingBlock:^(typeof(self) self) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self deferredReload];
        });
    }];
    
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(updateThemes:) name:ThemesUpdatedNotification object:nil];
    
    self.navigationItem.rightBarButtonItem = self.editButtonItem;
    [self deferredReload];
}

- (void)updateThemes:(NSNotification *)notification {
    dispatch_async(dispatch_get_main_queue(), ^{
        [self deferredReload];
    });
}

- (void)deferredReload {
    if (self.isEditing) {
        self->_pendingUpdate = YES;
    } else {
        self->_defaultThemes = [Theme.defaultThemes mutableCopy];
        self->_userThemes = [Theme.userThemes mutableCopy];
        [self updateTheme:UserPreferences.shared.theme];
        [self.tableView reloadData];
        self->_pendingUpdate = NO;
    }
}

- (void)updateTheme:(Theme *)theme {
    self->_theme = theme;
    self->_preferUserTheme = NO;
    for (Theme *theme in self->_defaultThemes) {
        if ([self->_theme.name isEqualToString:theme.name]) {
            for (Theme *theme in self->_userThemes) {
                if ([self->_theme.name isEqualToString:theme.name]) {
                    self->_preferUserTheme = YES;
                }
            }
            break;
        }
    }
}

- (void)setEditing:(BOOL)editing animated:(BOOL)animated {
    self->_importButtonEditingMode = editing;
    [self.tableView reloadSections:[NSIndexSet indexSetWithIndex:ImportSection] withRowAnimation:UITableViewRowAnimationAutomatic];
    
    if (!editing && self->_pendingUpdate) {
        [self deferredReload];
    }

    [super setEditing:editing animated:animated];
}

#pragma mark - Table view data source

enum {
    DefaultSection,
    UserSection,
    ImportSection,
    NumberOfSections,
};

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView {
    return NumberOfSections;
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    switch (section) {
        case DefaultSection:
            return self->_defaultThemes.count - ![self->_theme.name isEqualToString:@"Hot Dog Stand"];
        case UserSection:
            return self->_userThemes.count;
        case ImportSection:
            return self->_importButtonEditingMode && !self->_singleRowEditing;
        default:
            NSAssert(NO, @"unhandled section"); return 0;
    }
}

- (BOOL)shouldHideSection:(NSInteger)section {
    return (section == UserSection && !self->_userThemes.count) || (section == ImportSection && (self->_singleRowEditing || !self->_importButtonEditingMode));
}

- (NSString *)tableView:(UITableView *)tableView titleForHeaderInSection:(NSInteger)section {
    if ([self shouldHideSection:section]) {
        return nil;
    }
    switch (section) {
        case DefaultSection:
            return NSLocalizedString(@"Default Themes", @"Themes list section header");
        case UserSection:
            return NSLocalizedString(@"User Themes", @"Themes list section header");
        case ImportSection:
            return nil;
        default:
            NSAssert(NO, @"unhandled section"); return nil;
    }
}

- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    if ([self shouldHideSection:section]) {
        return nil;
    }
    switch (section) {
        case DefaultSection:
            return self->_preferUserTheme ? [NSString stringWithFormat:NSLocalizedString(@"The default theme \"%@\" is currently being overridden by a user theme.", @"Themes list footer; %@ is a theme name"), self->_theme.name] : nil;
        case ImportSection:
            return NSLocalizedString(@"User themes are stored in the iSH-AOK documents directory, under the \"themes\" folder. You can access them within iSH-AOK by running\n\n# mount -t real \"$(cat /proc/ish/documents)/themes\" [folder]\n\nand manipulating them from there.", @"Themes list footer under Import Theme; keep the mount command unchanged");
        default:
            return nil;
    }
}

- (CGFloat)tableView:(UITableView *)tableView heightForHeaderInSection:(NSInteger)section {
    return [self shouldHideSection:section] ? CGFLOAT_MIN : UITableViewAutomaticDimension;
}

- (CGFloat)tableView:(UITableView *)tableView heightForFooterInSection:(NSInteger)section {
    return [self shouldHideSection:section] ? CGFLOAT_MIN : UITableViewAutomaticDimension;
}

// At the text size of the Workspace window Settings is in; see
// WorkspaceTextScaledPage. Anywhere else the rows are left as they are.
- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    UITableViewCell *cell = [self unscaledTableView:tableView cellForRowAtIndexPath:indexPath];
    ISHWorkspaceScaleTableViewCell(cell, ISHWorkspaceTextScaleForViewController(self));
    return cell;
}

- (void)workspaceTextScaleDidChange {
    ISHWorkspaceRescaleTableView(self.tableView, ISHWorkspaceTextScaleForViewController(self));
}

- (UITableViewCell *)unscaledTableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:@"Theme" forIndexPath:indexPath];
    
    cell.textLabel.textColor = indexPath.section == ImportSection ? cell.tintColor : nil;
    cell.textLabel.enabled = YES;
    
    Theme *theme;
    
    switch (indexPath.section) {
        case DefaultSection:
            theme = self->_defaultThemes[indexPath.row];
            break;
        case UserSection:
            theme = self->_userThemes[indexPath.row];
            break;
        case ImportSection:
            cell.textLabel.text = NSLocalizedString(@"Import Theme", @"Themes list row that imports a theme file");
            cell.editingAccessoryType = UITableViewCellAccessoryNone;
            return cell;
    }
    
    cell.textLabel.text = theme.name;
    if ([theme.name isEqualToString:self->_theme.name] && (!self->_preferUserTheme || indexPath.section == UserSection)) {
        cell.accessoryType = UITableViewCellAccessoryCheckmark;
        cell.accessibilityTraits |= UIAccessibilityTraitSelected;
    } else {
        cell.accessoryType = UITableViewCellAccessoryNone;
        cell.accessibilityTraits &= ~UIAccessibilityTraitSelected;
    }
    cell.textLabel.enabled = ![theme.name isEqualToString:self->_theme.name] || indexPath.section != DefaultSection || !self->_preferUserTheme;
    cell.editingAccessoryType = UITableViewCellAccessoryDisclosureIndicator;
    
    return cell;
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    if (tableView.isEditing) {
        ThemeViewController *themeViewController = [self.storyboard instantiateViewControllerWithIdentifier:@"Theme"];
        switch (indexPath.section) {
            case DefaultSection:
                themeViewController.theme = self->_defaultThemes[indexPath.row];
                themeViewController.isEditable = NO;
                break;
            case UserSection:
                themeViewController.theme = self->_userThemes[indexPath.row];
                themeViewController.isEditable = YES;
                break;
            case ImportSection:
                [self importTheme];
                return;
        }
        [self.navigationController pushViewController:themeViewController animated:YES];
        [self setEditing:NO animated:YES];
    } else {
        Theme *theme;
        switch (indexPath.section) {
            case DefaultSection:
                theme = self->_defaultThemes[indexPath.row];
                break;
            case UserSection:
                theme = self->_userThemes[indexPath.row];
                break;
        }
        [self updateTheme:theme];
        [self.tableView performBatchUpdates:^{
            [self.tableView reloadSections:[NSIndexSet indexSetWithIndexesInRange:NSMakeRange(DefaultSection, UserSection - DefaultSection)] withRowAnimation:UITableViewRowAnimationAutomatic];
        } completion:nil];
        UserPreferences.shared.theme = theme;
    }
}

- (UITableViewCellEditingStyle)tableView:(UITableView *)tableView editingStyleForRowAtIndexPath:(NSIndexPath *)indexPath {
    switch (indexPath.section) {
        case UserSection:
            return UITableViewCellEditingStyleDelete;
        case ImportSection:
            return UITableViewCellEditingStyleInsert;
        default:
            return UITableViewCellEditingStyleNone;
    }
}

- (void)deleteUserThemeAtIndexPath:(NSIndexPath *)indexPath {
    [self->_userThemes[indexPath.row] deleteUserTheme];
    [self->_userThemes removeObjectAtIndex:indexPath.row];
    [self.tableView deleteRowsAtIndexPaths:@[indexPath] withRowAnimation:UITableViewRowAnimationAutomatic];
}

- (void)tableView:(UITableView *)tableView commitEditingStyle:(UITableViewCellEditingStyle)editingStyle forRowAtIndexPath:(NSIndexPath *)indexPath {
    switch (editingStyle) {
        case UITableViewCellEditingStyleInsert:
            [self importTheme];
            break;
        case UITableViewCellEditingStyleDelete:
            [self deleteUserThemeAtIndexPath:indexPath];
            break;
        default:
            NSAssert(NO, @"Invalid editing style");
    }
}

- (UISwipeActionsConfiguration *)tableView:(UITableView *)tableView trailingSwipeActionsConfigurationForRowAtIndexPath:(NSIndexPath *)indexPath {
    if (self.isEditing) {
        return nil;
    } else {
        NSMutableArray<UIContextualAction *> *actions = [NSMutableArray arrayWithObject:[UIContextualAction contextualActionWithStyle:UIContextualActionStyleNormal title:NSLocalizedString(@"Duplicate", @"Swipe action that copies a theme") handler:^(UIContextualAction *action, UIView *sourceView, void (^completionHandler)(BOOL)) {
            [(indexPath.section == DefaultSection ? self->_defaultThemes : self->_userThemes)[indexPath.row] duplicateAsUserTheme];
            [tableView performBatchUpdates:^{
                [tableView reloadSections:[NSIndexSet indexSetWithIndex:UserSection] withRowAnimation:UITableViewRowAnimationAutomatic];
            } completion:nil];
            completionHandler(YES);
        }]];
        if (indexPath.section == UserSection) {
            [actions addObject:[UIContextualAction contextualActionWithStyle:UIContextualActionStyleDestructive title:NSLocalizedString(@"Delete", @"Swipe action that deletes a user theme") handler:^(UIContextualAction *action, UIView *sourceView, void (^completionHandler)(BOOL)) {
                [self deleteUserThemeAtIndexPath:indexPath];
                completionHandler(YES);
            }]];
        }
        return [UISwipeActionsConfiguration configurationWithActions:actions];
    }
}

- (void)tableView:(UITableView *)tableView willBeginEditingRowAtIndexPath:(NSIndexPath *)indexPath {
    self->_singleRowEditing = YES;
    [super tableView:tableView willBeginEditingRowAtIndexPath:indexPath];
}

- (void)tableView:(UITableView *)tableView didEndEditingRowAtIndexPath:(NSIndexPath *)indexPath {
    [super tableView:tableView didEndEditingRowAtIndexPath:indexPath];
    self->_singleRowEditing = NO;
}

- (void)importTheme {
    UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc] initWithDocumentTypes:@[ @"public.json" ] inMode:UIDocumentPickerModeOpen];
    picker.delegate = self;
    if (@available(iOS 13, *)) {
    } else {
        picker.allowsMultipleSelection = YES;
    }
    [self presentViewController:picker animated:true completion:nil];
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)controller {
    [self.tableView deselectRowAtIndexPath:self.tableView.indexPathForSelectedRow animated:YES];
}

- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    for (NSURL *url in urls) {
        [url startAccessingSecurityScopedResource];
        [[[Theme alloc] initWithName:url.lastPathComponent.stringByDeletingPathExtension data:[NSData dataWithContentsOfURL:url]] addUserTheme];
        [url stopAccessingSecurityScopedResource];
    }
    [self documentPickerWasCancelled:controller];
    [self setEditing:NO animated:YES];
}

@end
