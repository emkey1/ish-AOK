//
//  ThemeViewController.m
//  libiSHApp
//
//  Created by Saagar Jha on 7/16/22.
//

#import "ThemeViewController.h"

#import "Theme.h"
#import "WorkspaceViewController.h"

#define COLORS 16

static NSString *ColorName(NSInteger index) {
    NSString *colorNames[] = {
        NSLocalizedString(@"Black", @"Terminal palette color name"),
        NSLocalizedString(@"Red", @"Terminal palette color name"),
        NSLocalizedString(@"Green", @"Terminal palette color name"),
        NSLocalizedString(@"Yellow", @"Terminal palette color name"),
        NSLocalizedString(@"Blue", @"Terminal palette color name"),
        NSLocalizedString(@"Magenta", @"Terminal palette color name"),
        NSLocalizedString(@"Cyan", @"Terminal palette color name"),
        NSLocalizedString(@"White", @"Terminal palette color name"),
        NSLocalizedString(@"Bright Black", @"Terminal palette color name"),
        NSLocalizedString(@"Bright Red", @"Terminal palette color name"),
        NSLocalizedString(@"Bright Green", @"Terminal palette color name"),
        NSLocalizedString(@"Bright Yellow", @"Terminal palette color name"),
        NSLocalizedString(@"Bright Blue", @"Terminal palette color name"),
        NSLocalizedString(@"Bright Magenta", @"Terminal palette color name"),
        NSLocalizedString(@"Bright Cyan", @"Terminal palette color name"),
        NSLocalizedString(@"Bright White", @"Terminal palette color name"),
    };
    return colorNames[index];
}

struct PaletteTextFields {
    UITextField *foregroundTextField;
    UITextField *backgroundTextField;
    UITextField *cursorTextField;
    NSArray<UITextField *> *colorTextFields;
};

@interface ThemeViewController () <WorkspaceTextScaledPage>
@end

@implementation ThemeViewController {
    UITextField *_nameTextField;
    UISwitch *_singlePaletteSwitch;
    UISwitch *_lightOverrideSwitch;
    UISwitch *_darkOverrideSwitch;
    BOOL _touchedOverrideSwitches;
    struct PaletteTextFields _paletteTextFields[2];
    BOOL _duplicated;
}

- (UITextField *)detailTextFieldWithText:(NSString *)text monospaced:(BOOL)monospaced {
    UITextField *textField = [UITextField new];
    textField.tag = 1;
    [textField addTarget:self action:@selector(textFieldChanged:) forControlEvents:UIControlEventEditingChanged];
    textField.text = textField.placeholder = text;
    textField.translatesAutoresizingMaskIntoConstraints = NO;
    textField.textAlignment = NSTextAlignmentRight;
    if (@available(iOS 13.0, *)) {
        if (monospaced) {
            textField.font = [UIFont monospacedSystemFontOfSize:textField.font.pointSize weight:UIFontWeightRegular];
        }
    }
    return textField;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    
    self.navigationItem.title = self.theme.name;
    
    _nameTextField = [self detailTextFieldWithText:_theme.name monospaced: NO];
    _nameTextField.accessibilityLabel = NSLocalizedString(@"Theme Name", @"Theme editor accessibility label for the name field");
    _singlePaletteSwitch = [UISwitch new];
    _singlePaletteSwitch.on = self.theme.lightPalette == self.theme.darkPalette;
    [_singlePaletteSwitch addTarget:self action:@selector(singlePaletteChanged:) forControlEvents:UIControlEventValueChanged];
    
    for (int i = 0; i < sizeof(_paletteTextFields) / sizeof(*_paletteTextFields); ++i) {
        Palette *palette = i ? self.theme.darkPalette : self.theme.lightPalette;
        _paletteTextFields[i].foregroundTextField = [self detailTextFieldWithText:palette.foregroundColor monospaced:YES];
        _paletteTextFields[i].foregroundTextField.accessibilityLabel = NSLocalizedString(@"Foreground Color", @"Theme editor palette field");
        _paletteTextFields[i].backgroundTextField = [self detailTextFieldWithText:palette.backgroundColor monospaced:YES];
        _paletteTextFields[i].backgroundTextField.accessibilityLabel = NSLocalizedString(@"Background Color", @"Theme editor palette field");
        _paletteTextFields[i].cursorTextField = [self detailTextFieldWithText:palette.cursorColor monospaced:YES];
        _paletteTextFields[i].cursorTextField.accessibilityLabel = NSLocalizedString(@"Cursor Color", @"Theme editor palette field");
        NSMutableArray<UITextField *> *textFields = [NSMutableArray new];
        for (int j = 0; j < COLORS; ++j) {
            UITextField *textField = [self detailTextFieldWithText:palette.colorPaletteOverrides ? palette.colorPaletteOverrides[j] : nil monospaced: YES];
            textField.accessibilityLabel = [NSString stringWithFormat:NSLocalizedString(@"Color %d", @"Theme editor accessibility label for palette entry number %d"), j];
            textField.autocorrectionType = UITextAutocorrectionTypeNo;
            textField.autocapitalizationType = UITextAutocapitalizationTypeNone;
            [textFields addObject:textField];
        }
        _paletteTextFields[i].colorTextFields = textFields;
    }
    
    if (!self.isEditable) {
        _singlePaletteSwitch.enabled = NO;
    }
    
    _lightOverrideSwitch = [UISwitch new];
    _lightOverrideSwitch.on = self.theme.appearance.lightOverride;
    [_lightOverrideSwitch addTarget:self action:@selector(touchedOverrideSwitch:) forControlEvents:UIControlEventValueChanged];
    _darkOverrideSwitch = [UISwitch new];
    _darkOverrideSwitch.on = self.theme.appearance.darkOverride;
    [_darkOverrideSwitch addTarget:self action:@selector(touchedOverrideSwitch:) forControlEvents:UIControlEventValueChanged];
    
    self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:NSLocalizedString(@"Duplicate", @"Theme editor button that copies the theme") style:UIBarButtonItemStylePlain target:self action:@selector(duplicate:)];
    self.navigationItem.rightBarButtonItem.accessibilityHint = NSLocalizedString(@"Creates a copy of the current theme for editing.", @"Accessibility hint for the theme duplication button");
}

- (void)duplicate:(UIBarButtonItem *)sender {
    [self.theme duplicateAsUserTheme];
    self->_duplicated = YES;
    [self.navigationController popViewControllerAnimated:YES];
}

Palette *createPalette(struct PaletteTextFields *paletteTextFields) {
    NSMutableArray<NSString *> *colors = [NSMutableArray new];
    for (UITextField *textField in paletteTextFields->colorTextFields) {
        if (textField.text.length) {
            [colors addObject:textField.text];
        }
    }
    return [[Palette alloc] initWithForegroundColor:paletteTextFields->foregroundTextField.text
                                    backgroundColor:paletteTextFields->backgroundTextField.text
                                        cursorColor:paletteTextFields->cursorTextField.text.length ? paletteTextFields->cursorTextField.text : nil
                              colorPaletteOverrides:colors.count == COLORS ? colors : nil];
}

- (void)viewDidDisappear:(BOOL)animated {
    [super viewDidDisappear:animated];
    if (self.isEditable && !self->_duplicated && [self validateTheme]) {
        Theme *theme;
        ThemeAppearance *appearance = self->_touchedOverrideSwitches ? [[ThemeAppearance alloc] initWithLightOverride:self->_lightOverrideSwitch.on darkOverride:self->_darkOverrideSwitch.on] : nil;
        if (_singlePaletteSwitch.on) {
            theme = [[Theme alloc] initWithName:_nameTextField.text palette:createPalette(_paletteTextFields + 0) appearance:appearance];
        } else {
            theme = [[Theme alloc] initWithName:_nameTextField.text lightPalette:createPalette(_paletteTextFields + 0) darkPalette:createPalette(_paletteTextFields + 1) appearance:appearance];
        }
        [self.theme replaceWithUserTheme:theme];
    }
}

#pragma mark - Table view data source

enum {
    NameSection,
    SinglePaletteSection,
    PaletteSection,
    PaletteSection2,
    UIOverrideSection,
    NumberOfSections,
};

enum {
    ForegroundRow,
    BackgroundRow,
    CursorRow,
    NumberOfRows = CursorRow + COLORS + 1,
};

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView {
    return NumberOfSections;
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    if ([self shouldHideSection:section]) {
        return 0;
    }
    switch (section) {
        case NameSection:
            return 1;
        case SinglePaletteSection:
            return 1;
        case PaletteSection:
        case PaletteSection2:
            return NumberOfRows;
        case UIOverrideSection:
            return 2;
        default:
            NSAssert(NO, @"unhandled section"); return 0;
    }
}

- (BOOL)shouldHideSection:(NSInteger)section {
    return section == PaletteSection2 && _singlePaletteSwitch.on;
}

- (NSString *)tableView:(UITableView *)tableView titleForHeaderInSection:(NSInteger)section {
    if ([self shouldHideSection:section]) {
        return nil;
    }
    switch (section) {
        case PaletteSection:
            return _singlePaletteSwitch.on ? NSLocalizedString(@"Palette", @"Theme editor section header") : NSLocalizedString(@"Light Palette", @"Theme editor section header");
        case PaletteSection2:
            return NSLocalizedString(@"Dark Palette", @"Theme editor section header");
        case UIOverrideSection:
            return NSLocalizedString(@"UI Overrides", @"Theme editor section header");
        default:
            return nil;
    }
}

- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    if ([self shouldHideSection:section]) {
        return nil;
    }
    switch (section) {
        case NameSection:
            return ![_nameTextField.text isEqualToString:self.theme.name] && [Theme themeForName:_nameTextField.text includingDefaultThemes:NO] ? NSLocalizedString(@"A user theme with this name already exists.", @"Theme editor footer when the name is taken") : nil;
        case SinglePaletteSection:
            return NSLocalizedString(@"When this is enabled, light and dark color schemes will share a single palette.", @"Theme editor footer under Single Palette");
        case UIOverrideSection:
            return NSLocalizedString(@"Use a customized color scheme for user interface elements (keyboard, status bar) rather than one that matches the current palette.", @"Theme editor footer under UI Overrides");
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
// WorkspaceTextScaledPage. Anywhere else the rows are left as they are. The
// colour fields being edited are in the rows too, which is why a scale change
// here re-fonts rows in place rather than reloading while one has the keyboard.
- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    UITableViewCell *cell = [self unscaledTableView:tableView cellForRowAtIndexPath:indexPath];
    ISHWorkspaceScaleTableViewCell(cell, ISHWorkspaceTextScaleForViewController(self));
    return cell;
}

- (void)workspaceTextScaleDidChange {
    ISHWorkspaceRescaleTableView(self.tableView, ISHWorkspaceTextScaleForViewController(self));
}

- (UITableViewCell *)unscaledTableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:@"ThemeSetting" forIndexPath:indexPath];
    [[cell viewWithTag:1] removeFromSuperview];
    if (self.isEditable) {
        cell.detailTextLabel.hidden = YES;
    } else {
        cell.detailTextLabel.hidden = NO;
        cell.detailTextLabel.enabled = NO;
    }
    cell.accessoryView = nil;
    switch (indexPath.section) {
        case NameSection:
            cell.textLabel.text = NSLocalizedString(@"Name", @"Theme editor row label for the theme name");
            if (self.isEditable) {
                [cell.contentView addSubview:_nameTextField];
                [NSLayoutConstraint activateConstraints:@[
                    [_nameTextField.leadingAnchor constraintEqualToSystemSpacingAfterAnchor:cell.textLabel.trailingAnchor multiplier:1],
                    [_nameTextField.trailingAnchor constraintEqualToAnchor:cell.detailTextLabel.trailingAnchor],
                    [_nameTextField.firstBaselineAnchor constraintEqualToAnchor:cell.detailTextLabel.firstBaselineAnchor],
                ]];
            } else {
                cell.detailTextLabel.text = self.theme.name;
                if (@available(iOS 13.0, *)) {
                    // The size before any Workspace text scale. A reused cell's
                    // label may carry one, and the new face would be scaled twice.
                    cell.detailTextLabel.font = [UIFont systemFontOfSize:ISHWorkspaceUnscaledFont(cell.detailTextLabel).pointSize];
                }
            }
            break;
        case SinglePaletteSection:
            cell.textLabel.text = NSLocalizedString(@"Single Palette", @"Theme editor switch label");
            cell.detailTextLabel.hidden = YES;
            cell.accessoryView = _singlePaletteSwitch;
            break;
        case PaletteSection:
        case PaletteSection2: {
            UITextField *detailTextField;
            switch (indexPath.row) {
                case ForegroundRow:
                    cell.textLabel.text = NSLocalizedString(@"Foreground Color", @"Theme editor palette row");
                    detailTextField = _paletteTextFields[indexPath.section - PaletteSection].foregroundTextField;
                    break;
                case BackgroundRow:
                    cell.textLabel.text = NSLocalizedString(@"Background Color", @"Theme editor palette row");
                    detailTextField = _paletteTextFields[indexPath.section - PaletteSection].backgroundTextField;
                    break;
                case CursorRow:
                    cell.textLabel.text = NSLocalizedString(@"Cursor Color", @"Theme editor palette row");
                    detailTextField = _paletteTextFields[indexPath.section - PaletteSection].cursorTextField;
                    break;
                default:
                    cell.textLabel.text = ColorName(indexPath.row - CursorRow - 1);
                    detailTextField = _paletteTextFields[indexPath.section - PaletteSection].colorTextFields[indexPath.row - CursorRow - 1];
                    break;
            }
            if (self.isEditable) {
                [cell.contentView addSubview:detailTextField];
                [NSLayoutConstraint activateConstraints:@[
                    [detailTextField.leadingAnchor constraintEqualToSystemSpacingAfterAnchor:cell.textLabel.trailingAnchor multiplier:1],
                    [detailTextField.trailingAnchor constraintEqualToAnchor:cell.detailTextLabel.trailingAnchor],
                    [detailTextField.firstBaselineAnchor constraintEqualToAnchor:cell.detailTextLabel.firstBaselineAnchor],
                ]];
            } else {
                cell.detailTextLabel.text = detailTextField.text;
                if (@available(iOS 13.0, *)) {
                    // Unscaled, as for the name above.
                    cell.detailTextLabel.font = [UIFont monospacedSystemFontOfSize:ISHWorkspaceUnscaledFont(cell.detailTextLabel).pointSize
                                                                            weight:UIFontWeightRegular];
                }
            }
            break;
        }
        case UIOverrideSection:
            cell.detailTextLabel.hidden = YES;
            switch (indexPath.row) {
                case 0:
                    cell.textLabel.text = NSLocalizedString(@"Use Dark UI for Light Color Scheme", @"Theme editor UI override switch");
                    cell.accessoryView = self->_lightOverrideSwitch;
                    break;
                case 1:
                    cell.textLabel.text = NSLocalizedString(@"Use Light UI for Dark Color Scheme", @"Theme editor UI override switch");
                    cell.accessoryView = self->_darkOverrideSwitch;
                    break;
                default:
                    NSAssert(NO, @"Invalid row");
            }
            break;
    }
    
    if (!self.isEditable) {
        cell.textLabel.enabled = NO;
    }
    
    return cell;
}

- (BOOL)validateTheme {
    BOOL validName = _nameTextField.text.length && ([_nameTextField.text isEqualToString:self.theme.name] || ![Theme themeForName:_nameTextField.text includingDefaultThemes:NO]);
    _nameTextField.textColor = validName ? nil : UIColor.systemRedColor;
    [self.tableView reloadSections:[NSIndexSet indexSetWithIndex:NameSection] withRowAnimation:UITableViewRowAnimationNone];
    BOOL validColors = YES;
    BOOL (^validColor)(UITextField *) = ^(UITextField *textField) {
        BOOL valid = !![[UIColor alloc] ish_initWithHexString:textField.text];
        textField.textColor = valid ? nil : UIColor.systemRedColor;
        return valid;
    };
    for (int i = 0; i < sizeof(_paletteTextFields) / sizeof(*_paletteTextFields) - _singlePaletteSwitch.on; ++i) {
        validColors &= validColor(_paletteTextFields[i].foregroundTextField);
        validColors &= validColor(_paletteTextFields[i].backgroundTextField);
        validColors &= !_paletteTextFields[i].cursorTextField.text.length || validColor(_paletteTextFields[i].cursorTextField);
        int empty = 0;
        int valid = 0;
        for (int j = 0; j < COLORS; ++j) {
            empty += !_paletteTextFields[i].colorTextFields[j].text.length;
            valid += validColor(_paletteTextFields[i].colorTextFields[j]);
        }
        validColors &= (empty == COLORS || valid == COLORS);
    }
    // Just report validity. This used to ASSIGN hidesBackButton as a side effect
    // of the return, so clearing the Name field or half-typing a hex colour made
    // the back button disappear mid-keystroke and the screen became a dead end.
    // The guard bought nothing either: -viewDidDisappear already gates
    // persistence on this same check, so an invalid theme cannot be saved by any
    // exit path -- trapping the user was never what kept the data clean.
    return validName && validColors;
}

- (void)textFieldChanged:(UITextField *)sender {
    // Hack to keep the keyboard up across a table view update
    UITextRange *selectedRange = sender.isFirstResponder ? sender.selectedTextRange : nil;
    [self validateTheme];
    if (selectedRange) {
        [sender becomeFirstResponder];
        [sender setSelectedTextRange:selectedRange];
    }
}

- (void)singlePaletteChanged:(UISwitch *)sender {
    [self.tableView performBatchUpdates:^{
        for (int i = 0; i < NumberOfRows; ++i) {
            if (sender.on) {
                [self.tableView deleteRowsAtIndexPaths:@[[NSIndexPath indexPathForRow:i inSection:PaletteSection2]] withRowAnimation:UITableViewRowAnimationFade];
            } else {
                [self.tableView insertRowsAtIndexPaths:@[[NSIndexPath indexPathForRow:i inSection:PaletteSection2]] withRowAnimation:UITableViewRowAnimationFade];
            }
        }
        [self.tableView reloadSections:[NSIndexSet indexSetWithIndexesInRange:NSMakeRange(PaletteSection, PaletteSection2 - PaletteSection)] withRowAnimation:UITableViewRowAnimationAutomatic];
    } completion:nil];
    [self validateTheme];
}

- (void)touchedOverrideSwitch:(UISwitch *)sender {
    self->_touchedOverrideSwitches = YES;
}

@end
