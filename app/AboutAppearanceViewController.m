//
//  ThemeViewController.m
//  iSH
//
//  Created by Charlie Melbye on 11/12/18.
//

#import "AboutAppearanceViewController.h"
#import "FontPickerViewController.h"
#import "TerminalView.h"
#import "ThemesViewController.h"
#import "UserPreferences.h"
#import "NSObject+SaneKVO.h"

@interface AboutAppearanceViewController ()
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
    [UserPreferences.shared observe:@[@"theme", @"fontSize", @"fontFamily", @"colorScheme", @"workspaceStyle"]
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

#if !ISH_LINUX
    if (![NSUserDefaults.standardUserDefaults boolForKey:@"recovery"]) {
        _terminal = [Terminal createPseudoTerminal:&_tty];
        [_terminal sendOutput:previewString length:(int)strlen(previewString)];
    }
#endif
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
        case MainSection: return 3;
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
        case PreviewSection: return @"Preview";
        case ColorSchemeSection: return @"Color Scheme";
        case WorkspaceStyleSection: return @"Workspace Style";
        case CursorSection: return @"Cursor";
        case StatusBarSection: return @"Status Bar";
        case TerminalButtonsSection: return @"Terminal Buttons";
        case WorkspaceLaunchSection: return [self supportsWorkspaceLaunchCount] ? @"Desktops at Launch" : nil;
        default: return nil;
    }
}

- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    switch (section) {
        case PreviewSection: return @"Change the color scheme used for the preview.";
        case WorkspaceStyleSection: return @"Modern is a flat, redesigned desktop; Classic keeps the original look. Both stay available and only restyle the Workspace.";
        case TerminalButtonsSection: return @"Show the settings (gear) and terminal-switcher buttons on the terminal. Turn this off for a cleaner terminal.";
        case WorkspaceLaunchSection: return [self supportsWorkspaceLaunchCount] ? @"How many in-app Desktops to open automatically at launch." : nil;
        default: return nil;
    }
}

- (NSString *)reuseIdentifierForIndexPath:(NSIndexPath *)indexPath {
    switch (indexPath.section) {
        case PreviewSection: return @[@"Preview", @"Color Scheme Preview"][indexPath.row];
        case MainSection: return @[@"Theme Name", @"Font", @"Font Size"][indexPath.row];
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
    } else {
        return UITableViewAutomaticDimension;
    }
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
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
                    cell.detailTextLabel.font = [UIFont fontWithName:UserPreferences.shared.fontFamily size:cell.detailTextLabel.font.pointSize];
                    break;
                case 2: {
                    UserPreferences *prefs = [UserPreferences shared];
                    UILabel *label = [cell viewWithTag:1];
                    UIStepper *stepper = [cell viewWithTag:2];
                    label.text = prefs.fontSize.stringValue;
                    stepper.value = prefs.fontSize.doubleValue;
                    cell.selectionStyle = UITableViewCellSelectionStyleNone;
                    break;
                }
            }
            break;
            
        case ColorSchemeSection:
            switch (indexPath.row) {
                case 0:
                    cell.textLabel.text = @"Match System";
                    break;
                case 1:
                    cell.textLabel.text = @"Light";
                    break;
                case 2:
                    cell.textLabel.text = @"Dark";
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
                    cell.textLabel.text = @"Classic";
                    break;
                case 1:
                    cell.textLabel.text = @"Modern";
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
            cell.textLabel.text = @"Show Settings & Switcher";
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
            cell.textLabel.text = count == 1 ? @"1 desktop" : [NSString stringWithFormat:@"%ld desktops", (long)count];
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
