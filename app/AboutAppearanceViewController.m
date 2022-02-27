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
@property (strong, nonatomic) IBOutlet UISwitch *hideStatusBar;
@property UIFontPickerViewController *fontPicker API_AVAILABLE(ios(13));
@end

@implementation AboutAppearanceViewController

- (void)viewDidLoad {
    [super viewDidLoad];
    [UserPreferences.shared observe:@[@"theme", @"fontSize", @"fontFamily", @"colorScheme", @"hideStatusBar"]
                            options:0 owner:self usingBlock:^(typeof(self) self) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self.tableView reloadData];
            [self setNeedsStatusBarAppearanceUpdate];
        });
    }];
    self.hideStatusBar.on = UserPreferences.shared.hideStatusBar;
}

- (void)viewDidAppear:(BOOL)animated {
    if (@available(iOS 13, *)) {
        // Initialize the font picker ASAP, as it takes about a quarter second to initialize (XPC crap) and appears invisible until then.
        // Re-initialize it after navigating away from it, to reset the table view highlight.
        UIFontPickerViewControllerConfiguration *config = [UIFontPickerViewControllerConfiguration new];
        config.filteredTraits = UIFontDescriptorTraitMonoSpace;
        self.fontPicker = [[UIFontPickerViewController alloc] initWithConfiguration:config];
        // Prevent the font picker from resizing the popup when it appears
        self.fontPicker.preferredContentSize = CGSizeZero;
        self.fontPicker.navigationItem.title = @"Font";
        self.fontPicker.delegate = self;
        self.fontPicker.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:@"Reset" style:UIBarButtonItemStylePlain target:self action:@selector(resetFont:)];
    }
}

#pragma mark - Table view data source

enum {
    PreviewSection,
    MainSection,
    ColorSchemeSection,
    CursorSection,
    StatusBarSection,
    NumberOfSections,
};

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView {
    return NumberOfSections;
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    switch (section) {
        case PreviewSection: return 2;
        case MainSection: return 3;
        case ColorSchemeSection: return 3;
        case CursorSection: return 2;
        case StatusBarSection: return 1;
        default: NSAssert(NO, @"unhandled section"); return 0;
    }
}

- (NSString *)tableView:(UITableView *)tableView titleForHeaderInSection:(NSInteger)section {
    switch (section) {
        case PreviewSection: return @"Preview";
        case ColorSchemeSection: return @"Color Scheme";
        case CursorSection: return @"Cursor";
        case StatusBarSection: return @"Status Bar";
        default: return nil;
    }
}

- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    switch (section) {
        case PreviewSection: return @"Change the color scheme used for the preview.";
        default: return nil;
    }
}

- (NSString *)reuseIdentifierForIndexPath:(NSIndexPath *)indexPath {
    switch (indexPath.section) {
        case PreviewSection: return @[@"Preview", @"Color Scheme Preview"][indexPath.row];
        case MainSection: return @[@"Theme Name", @"Font", @"Font Size"][indexPath.row];
        case ColorSchemeSection: return @"Color Scheme";
        case CursorSection: return @[@"Cursor Style", @"Blink Cursor"][indexPath.row];
        case StatusBarSection: return @"Status Bar";
        default: return nil;
    }
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    UserPreferences *prefs = [UserPreferences shared];
    UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:[self reuseIdentifierForIndexPath:indexPath] forIndexPath:indexPath];
    
    switch (indexPath.section) {
        case PreviewSection:
            if (indexPath.row == 0) {
                TerminalView *terminalView = [cell viewWithTag:1];
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
            cell.accessoryType = indexPath.row == UserPreferences.shared.colorScheme ? UITableViewCellAccessoryCheckmark : UITableViewCellAccessoryNone;
            break;
        
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
    }
}

- (void)selectFont:(id)sender {
    if (@available(iOS 13, *)) {
        [self.navigationController pushViewController:self.fontPicker animated:YES];
        return;
    }
    
    FontPickerViewController *fontPicker = [self.storyboard instantiateViewControllerWithIdentifier:@"FontPicker"];
    [self.navigationController pushViewController:fontPicker animated:YES];
}

- (void)fontPickerViewControllerDidPickFont:(UIFontPickerViewController *)viewController API_AVAILABLE(ios(13.0)) {
    UserPreferences.shared.fontFamily = viewController.selectedFontDescriptor.fontAttributes[UIFontDescriptorFamilyAttribute];
    [self.navigationController popToViewController:self animated:YES];
}

- (IBAction)resetFont:(UIBarButtonItem *)sender API_AVAILABLE(ios(13)) {
    UserPreferences.shared.fontFamily = nil;
    [self.navigationController popToViewController:self animated:YES];
}

- (IBAction)fontSizeChanged:(UIStepper *)sender {
    UserPreferences.shared.fontSize = @((int) sender.value);
}

- (IBAction)hideStatusBarChanged:(UISwitch *)sender {
    UserPreferences.shared.hideStatusBar = sender.on;
    [self setNeedsStatusBarAppearanceUpdate];
}

@end
