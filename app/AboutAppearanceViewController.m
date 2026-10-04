//
//  ThemeViewController.m
//  iSH
//
//  Created by Charlie Melbye on 11/12/18.
//

#import "AboutAppearanceViewController.h"
#import "AppDelegate.h"
#import "FontPickerViewController.h"
#import "TerminalView.h"
#import "ThemesViewController.h"
#import "UserPreferences.h"
#import "NSObject+SaneKVO.h"
#import "UIViewController+Extras.h"
#import "WorkspaceViewController.h"

@interface AboutAppearanceViewController () <WorkspaceTextScaledPage>
@property (strong, nonatomic) IBOutlet UISwitch *blinkCursor;
@property (strong, nonatomic) IBOutlet UISegmentedControl *cursorStyle;
@property (strong, nonatomic) IBOutlet UISwitch *hideStatusBar;
@end

char *previewString = "# cat /proc/ish/colors\r\n"
"\x1B[30m" "iSH" "\x1B[39m "
"\x1B[31m" "iSH" "\x1B[39m "
"\x1B[32m" "iSH" "\x1B[39m "
"\x1B[33m" "iSH" "\x1B[39m "
"\x1B[34m" "iSH" "\x1B[39m "
"\x1B[35m" "iSH" "\x1B[39m "
"\x1B[36m" "iSH" "\x1B[39m "
"\x1B[37m" "iSH" "\x1B[39m" "\r\n\x1B[7m"
"\x1B[40m" "iSH" "\x1B[39m "
"\x1B[41m" "iSH" "\x1B[39m "
"\x1B[42m" "iSH" "\x1B[39m "
"\x1B[43m" "iSH" "\x1B[39m "
"\x1B[44m" "iSH" "\x1B[39m "
"\x1B[45m" "iSH" "\x1B[39m "
"\x1B[46m" "iSH" "\x1B[39m "
"\x1B[47m" "iSH" "\x1B[39m" "\x1B[0m\x1B[1m\r\n"
"\x1B[90m" "iSH" "\x1B[39m "
"\x1B[91m" "iSH" "\x1B[39m "
"\x1B[92m" "iSH" "\x1B[39m "
"\x1B[93m" "iSH" "\x1B[39m "
"\x1B[94m" "iSH" "\x1B[39m "
"\x1B[95m" "iSH" "\x1B[39m "
"\x1B[96m" "iSH" "\x1B[39m "
"\x1B[97m" "iSH" "\x1B[39m" "\r\n\x1B[7m"
"\x1B[100m" "iSH" "\x1B[39m "
"\x1B[101m" "iSH" "\x1B[39m "
"\x1B[102m" "iSH" "\x1B[39m "
"\x1B[103m" "iSH" "\x1B[39m "
"\x1B[104m" "iSH" "\x1B[39m "
"\x1B[105m" "iSH" "\x1B[39m "
"\x1B[106m" "iSH" "\x1B[39m "
"\x1B[107m" "iSH" "\x1B[39m" "\x1B[0m\r\n"
"# ";

@implementation AboutAppearanceViewController {
    TerminalView *_terminalView;
    Terminal *_terminal;
    struct tty *_tty;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    ISHSizeTableSectionTitlesOnMac(self.tableView);
    [UserPreferences.shared observe:@[@"theme", @"fontSize", @"lineHeight", @"fontFamily", @"colorScheme", @"workspaceStyle"]
                            options:0 owner:self usingBlock:^(typeof(self) self) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self.tableView reloadData];
        });
    }];

    [UserPreferences.shared observe:@[@"cursorStyle", @"blinkCursor", @"hideStatusBar"]
                            options:0 owner:self usingBlock:^(typeof(self) self) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self updateOtherControls];
        });
    }];
    [self updateOtherControls];

    if (![NSUserDefaults.standardUserDefaults boolForKey:@"recovery"]) {
        // Borrow init for the duration, as UpgradeRootViewController does for
        // its own preview terminal. Making a pty reaches kernel code that
        // expects to be running as SOME process -- the slave node takes its
        // ownership from the caller -- and this is the UI thread, which is not
        // one.
        struct task *previousCurrent = NULL;
        if ([AppDelegate pushUsableInitTaskAsCurrent:&previousCurrent]) {
            _terminal = [Terminal createPseudoTerminal:&_tty];
            [AppDelegate popCurrentTask:previousCurrent];
        }
        [_terminal sendOutput:previewString length:(int)strlen(previewString)];
    }
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    // Get the cost of faulting in every installed font out of the way now, so pushing
    // the font picker doesn't stall.
    [FontPickerViewController prewarm];
}

#pragma mark - Table view data source

enum {
    PreviewSection,
    MainSection,
    ColorSchemeSection,
    WorkspaceStyleSection,
    CursorSection,
    StatusBarSection,
    TerminalButtonsSection,
    WorkspaceLaunchSection,
    NumberOfSections,
};

// In-app Desktops work on every device, so the launch count always applies.
- (BOOL)supportsWorkspaceLaunchCount {
    return YES;
}

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView {
    return NumberOfSections;
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    switch (section) {
        case PreviewSection: return 2;
        case MainSection: return 4;
        case ColorSchemeSection: return 3;
        case WorkspaceStyleSection: return 2;
        case CursorSection: return 2;
        case StatusBarSection: return 1;
        case TerminalButtonsSection: return 1;
        case WorkspaceLaunchSection: return [self supportsWorkspaceLaunchCount] ? 4 : 0;
        default: NSAssert(NO, @"unhandled section"); return 0;
    }
}

- (NSString *)tableView:(UITableView *)tableView titleForHeaderInSection:(NSInteger)section {
    switch (section) {
        case PreviewSection: return NSLocalizedString(@"Preview", @"Appearance settings section header");
        case ColorSchemeSection: return NSLocalizedString(@"Color Scheme", @"Appearance settings section header");
        case WorkspaceStyleSection: return NSLocalizedString(@"Workspace Style", @"Appearance settings section header");
        case CursorSection: return NSLocalizedString(@"Cursor", @"Appearance settings section header");
        case StatusBarSection: return NSLocalizedString(@"Status Bar", @"Appearance settings section header");
        case TerminalButtonsSection: return NSLocalizedString(@"Terminal Buttons", @"Appearance settings section header");
        case WorkspaceLaunchSection: return [self supportsWorkspaceLaunchCount] ? NSLocalizedString(@"Desktops at Launch", @"Appearance settings section header") : nil;
        default: return nil;
    }
}

- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    switch (section) {
        case PreviewSection: return NSLocalizedString(@"Change the color scheme used for the preview.", @"Appearance settings section footer");
        case WorkspaceStyleSection: return NSLocalizedString(@"Modern is a flat, redesigned desktop; Classic keeps the original look. Both stay available and only restyle the Workspace.", @"Appearance settings section footer");
        case TerminalButtonsSection: return NSLocalizedString(@"Show the settings (gear) and terminal-switcher buttons on the terminal. Turn this off for a cleaner terminal.", @"Appearance settings section footer");
        case WorkspaceLaunchSection: return [self supportsWorkspaceLaunchCount] ? NSLocalizedString(@"How many in-app Desktops to open automatically at launch.", @"Appearance settings section footer") : nil;
        default: return nil;
    }
}

- (NSString *)reuseIdentifierForIndexPath:(NSIndexPath *)indexPath {
    switch (indexPath.section) {
        case PreviewSection: return @[@"Preview", @"Color Scheme Preview"][indexPath.row];
        // Line Height reuses the Font Size prototype: same title label, value
        // label and stepper. Everything that differs is set in code, because a
        // recycled cell would otherwise arrive carrying the other row's range,
        // the other row's title, and the other row's action.
        case MainSection: return @[@"Theme Name", @"Font", @"Font Size", @"Font Size"][indexPath.row];
        case ColorSchemeSection: return @"Color Scheme";
        case WorkspaceStyleSection: return @"Color Scheme";
        case CursorSection: return @[@"Cursor Style", @"Blink Cursor"][indexPath.row];
        case StatusBarSection: return @"Status Bar";
        case TerminalButtonsSection: return @"Color Scheme";
        case WorkspaceLaunchSection: return @"Color Scheme";
        default: return nil;
    }
}

- (CGFloat)tableView:(UITableView *)tableView heightForRowAtIndexPath:(NSIndexPath *)indexPath {
    if (indexPath.section == PreviewSection && indexPath.row == 0) {
        // Try a best-effort guess as to how big the preview should be.
        return [@"\n\n\n\n\n\n" sizeWithAttributes:@{NSFontAttributeName: UserPreferences.shared.approximateFont}].height + 10;
    } else if (indexPath.section == PreviewSection) {
        return UITableViewAutomaticDimension;
    } else {
        // The Font Size and Line Height rows centre a label and a stepper with
        // nothing above or below, so they would stay 44 points tall whatever
        // the text size.
        return ISHWorkspaceTextScaledRowHeight(UITableViewAutomaticDimension, ISHWorkspaceTextScaleForViewController(self));
    }
}

// At the text size of the Workspace window Settings is in; see
// WorkspaceTextScaledPage. Anywhere else the rows are left as they are. Not the
// preview section: a terminal drawn at the terminal's own font size, and the
// control that switches its colours.
- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    UITableViewCell *cell = [self unscaledTableView:tableView cellForRowAtIndexPath:indexPath];
    if (indexPath.section != PreviewSection)
        ISHWorkspaceScaleTableViewCell(cell, ISHWorkspaceTextScaleForViewController(self));
    return cell;
}

- (void)workspaceTextScaleDidChange {
    ISHWorkspaceRescaleTableView(self.tableView, ISHWorkspaceTextScaleForViewController(self));
}

- (UITableViewCell *)unscaledTableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:[self reuseIdentifierForIndexPath:indexPath] forIndexPath:indexPath];
    cell.selectionStyle = UITableViewCellSelectionStyleDefault;
    
    switch (indexPath.section) {
        case PreviewSection:
            switch (indexPath.row) {
                case 0:
                    _terminalView = [cell viewWithTag:1];
                    _terminalView.userInteractionEnabled = NO;
                    _terminalView.terminal = _terminal;
                    break;
                case 1: {
                    UISegmentedControl *segmentedControl = [cell viewWithTag:1];
                    [segmentedControl addTarget:self action:@selector(changePreviewTheme:) forControlEvents:UIControlEventValueChanged];
                    [self changePreviewTheme:segmentedControl];
                    cell.selectionStyle = UITableViewCellSelectionStyleNone;
                    break;
                }
            }
            break;
            
        case MainSection:
            switch (indexPath.row) {
                case 0:
                    cell.detailTextLabel.text = UserPreferences.shared.theme.name;
                    break;
                case 1:
                    cell.detailTextLabel.text = UserPreferences.shared.fontFamilyUserFacingName;
                    // The size before any Workspace text scale. This label may
                    // carry one, and the new face would otherwise be scaled twice.
                    cell.detailTextLabel.font = [UIFont fontWithName:UserPreferences.shared.fontFamily
                                                                size:ISHWorkspaceUnscaledFont(cell.detailTextLabel).pointSize];
                    break;
                case 2:
                case 3: {
                    UserPreferences *prefs = [UserPreferences shared];
                    UILabel *title = [cell viewWithTag:3];
                    UILabel *label = [cell viewWithTag:1];
                    UIStepper *stepper = [cell viewWithTag:2];
                    // Shared prototype, so nothing may be left to the storyboard --
                    // including the action it wired, which belongs to Font Size.
                    [stepper removeTarget:nil action:NULL forControlEvents:UIControlEventValueChanged];
                    if (indexPath.row == 2) {
                        title.text = NSLocalizedString(@"Font Size", @"Appearance settings row label");
                        stepper.minimumValue = 1;
                        stepper.maximumValue = 72;
                        stepper.stepValue = 1;
                        stepper.value = prefs.fontSize.doubleValue;
                        label.text = prefs.fontSize.stringValue;
                        [stepper addTarget:self action:@selector(fontSizeChanged:)
                          forControlEvents:UIControlEventValueChanged];
                    } else {
                        title.text = NSLocalizedString(@"Line Height", @"Appearance settings row label");
                        // Inside the bounds UserPreferences and hterm both enforce, and
                        // narrower: this closes a one-or-two-pixel band, so the useful
                        // range is just below 1. 0.05 is a visible step at every font
                        // size without being a jump.
                        stepper.minimumValue = 0.7;
                        stepper.maximumValue = 1.3;
                        stepper.stepValue = 0.05;
                        stepper.value = prefs.lineHeight.doubleValue;
                        label.text = [NSString stringWithFormat:@"%.2f", prefs.lineHeight.doubleValue];
                        [stepper addTarget:self action:@selector(lineHeightChanged:)
                          forControlEvents:UIControlEventValueChanged];
                    }
                    cell.selectionStyle = UITableViewCellSelectionStyleNone;
                    break;
                }
            }
            break;
            
        case ColorSchemeSection:
            switch (indexPath.row) {
                case 0:
                    cell.textLabel.text = NSLocalizedString(@"Match System", @"Color scheme option");
                    break;
                case 1:
                    cell.textLabel.text = NSLocalizedString(@"Light", @"Color scheme option");
                    break;
                case 2:
                    cell.textLabel.text = NSLocalizedString(@"Dark", @"Color scheme option");
                    break;
            }
            if (indexPath.row == UserPreferences.shared.colorScheme) {
                cell.accessoryType = UITableViewCellAccessoryCheckmark;
                cell.accessibilityTraits |= UIAccessibilityTraitSelected;
            } else {
                cell.accessoryType = UITableViewCellAccessoryNone;
                cell.accessibilityTraits &= ~UIAccessibilityTraitSelected;
            }
            break;

        case WorkspaceStyleSection:
            switch (indexPath.row) {
                case 0:
                    cell.textLabel.text = NSLocalizedString(@"Classic", @"Workspace style option");
                    break;
                case 1:
                    cell.textLabel.text = NSLocalizedString(@"Modern", @"Workspace style option");
                    break;
            }
            if (indexPath.row == UserPreferences.shared.workspaceStyle) {
                cell.accessoryType = UITableViewCellAccessoryCheckmark;
                cell.accessibilityTraits |= UIAccessibilityTraitSelected;
            } else {
                cell.accessoryType = UITableViewCellAccessoryNone;
                cell.accessibilityTraits &= ~UIAccessibilityTraitSelected;
            }
            break;

        case TerminalButtonsSection:
            cell.textLabel.text = NSLocalizedString(@"Show Settings & Switcher", @"Appearance settings toggle for the terminal quick buttons");
            if (UserPreferences.shared.showTerminalQuickButtons) {
                cell.accessoryType = UITableViewCellAccessoryCheckmark;
                cell.accessibilityTraits |= UIAccessibilityTraitSelected;
            } else {
                cell.accessoryType = UITableViewCellAccessoryNone;
                cell.accessibilityTraits &= ~UIAccessibilityTraitSelected;
            }
            break;

        case WorkspaceLaunchSection: {
            NSInteger count = indexPath.row + 1;
            cell.textLabel.text = count == 1 ? NSLocalizedString(@"1 desktop", @"Desktops at Launch option") : [NSString stringWithFormat:NSLocalizedString(@"%ld desktops", @"Desktops at Launch option; %ld is 2 or more"), (long)count];
            if (count == UserPreferences.shared.workspaceLaunchCount) {
                cell.accessoryType = UITableViewCellAccessoryCheckmark;
                cell.accessibilityTraits |= UIAccessibilityTraitSelected;
            } else {
                cell.accessoryType = UITableViewCellAccessoryNone;
                cell.accessibilityTraits &= ~UIAccessibilityTraitSelected;
            }
            break;
        }

        case CursorSection:
        case StatusBarSection:
            cell.selectionStyle = UITableViewCellSelectionStyleNone;
            break;
    }
    
    return cell;
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    
    switch (indexPath.section) {
        case MainSection:
            switch (indexPath.row) {
                case 0: { // theme
                    ThemesViewController *themesViewController = [self.storyboard instantiateViewControllerWithIdentifier:@"Themes"];
                    [self.navigationController pushViewController:themesViewController animated:YES];
                    break;
                }
                case 1: // font family
                    [self selectFont:nil];
                    break;
            }
            break;
        case ColorSchemeSection:
            [UserPreferences.shared setColorScheme:indexPath.row];
            break;
        case WorkspaceStyleSection:
            [UserPreferences.shared setWorkspaceStyle:indexPath.row];
            break;
        case TerminalButtonsSection:
            UserPreferences.shared.showTerminalQuickButtons = !UserPreferences.shared.showTerminalQuickButtons;
            [tableView reloadSections:[NSIndexSet indexSetWithIndex:TerminalButtonsSection] withRowAnimation:UITableViewRowAnimationNone];
            break;
        case WorkspaceLaunchSection:
            UserPreferences.shared.workspaceLaunchCount = indexPath.row + 1;
            [tableView reloadSections:[NSIndexSet indexSetWithIndex:WorkspaceLaunchSection] withRowAnimation:UITableViewRowAnimationNone];
            break;
    }
}

- (void)updateOtherControls {
    self.hideStatusBar.on = UserPreferences.shared.hideStatusBar;
    self.cursorStyle.selectedSegmentIndex = UserPreferences.shared.cursorStyle;
    self.blinkCursor.on = UserPreferences.shared.blinkCursor;
    [self setNeedsStatusBarAppearanceUpdate];
}

- (void)changePreviewTheme:(UISegmentedControl *)sender {
    _terminalView.overrideAppearance = sender.selectedSegmentIndex ? OverrideAppearanceDark : OverrideAppearanceLight;
    _terminalView.backgroundColor = [[UIColor alloc] ish_initWithHexString:(sender.selectedSegmentIndex ? UserPreferences.shared.theme.darkPalette : UserPreferences.shared.theme.lightPalette).backgroundColor];
}

- (void)selectFont:(id)sender {
    [self.navigationController pushViewController:[FontPickerViewController new] animated:YES];
}

- (IBAction)lineHeightChanged:(UIStepper *)sender {
    // Rounded because a stepper accumulates its step in binary: 0.05 twenty
    // times is not 1, and the value label would show 0.95000000000000007.
    UserPreferences.shared.lineHeight = @(round(sender.value * 100) / 100);
}

- (IBAction)fontSizeChanged:(UIStepper *)sender {
    UserPreferences.shared.fontSize = @((int) sender.value);
}

- (IBAction)hideStatusBarChanged:(UISwitch *)sender {
    UserPreferences.shared.hideStatusBar = sender.on;
    [self setNeedsStatusBarAppearanceUpdate];
}

- (IBAction)cursorStyleChanged:(UISegmentedControl *)sender {
    [UserPreferences.shared setCursorStyle:sender.selectedSegmentIndex];
}

- (IBAction)blinkCursorChanged:(UISwitch *)sender {
    [UserPreferences.shared setBlinkCursor:sender.on];
}
@end
