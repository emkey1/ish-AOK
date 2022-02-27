//
//  ThemesViewController.m
//  iSH
//
//  Created by Saagar Jha on 2/25/22.
//

#import "ThemesViewController.h"

#import "NSObject+SaneKVO.h"
#import "Theme.h"
#import "UserPreferences.h"

@interface ThemesViewController ()

@end

@implementation ThemesViewController

- (void)viewDidLoad {
    [super viewDidLoad];
    
    [UserPreferences.shared observe:@[@"theme"]
                            options:0 owner:self usingBlock:^(typeof(self) self) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self.tableView reloadData];
        });
    }];
    
    // Uncomment the following line to preserve selection between presentations.
    // self.clearsSelectionOnViewWillAppear = NO;
    
    self.navigationItem.rightBarButtonItem = self.editButtonItem;
}

#pragma mark - Table view data source

enum {
    DefaultSection,
    UserSection,
    NumberOfSections,
};

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView {
    return NumberOfSections - 0;
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    switch (section) {
        case DefaultSection:
            return Theme.defaultThemes.count - ![UserPreferences.shared.theme.name isEqualToString:@"Hot Dog Stand"];
        case UserSection:
            return 1;
        default:
            NSAssert(NO, @"unhandled section"); return 0;
    }
}

- (NSString *)tableView:(UITableView *)tableView titleForHeaderInSection:(NSInteger)section {
    switch (section) {
        case DefaultSection:
            return @"Default Themes";
        case UserSection:
            return @"User Themes";
        default:
            NSAssert(NO, @"unhandled section"); return nil;
    }
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:@"Theme" forIndexPath:indexPath];
    
    switch (indexPath.section) {
        case DefaultSection:
            cell.textLabel.text = Theme.defaultThemes[indexPath.row].name;
            break;
        case UserSection:
            cell.textLabel.text = @"asasfd";
            break;
    }
    
    if ([cell.textLabel.text isEqualToString:UserPreferences.shared.theme.name]) {
        cell.accessoryType = UITableViewCellAccessoryCheckmark;
    } else {
        cell.accessoryType = UITableViewCellAccessoryNone;
    }
    cell.editingAccessoryType = UITableViewCellAccessoryDisclosureIndicator;
    
    return cell;
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    if (tableView.isEditing) {
        switch (indexPath.section) {
            case DefaultSection:
                break;
            case UserSection:
                break;
        }
    } else {
        switch (indexPath.section) {
            case DefaultSection:
                UserPreferences.shared.theme = Theme.defaultThemes[indexPath.row];
                break;
            case UserSection:
                break;
        }
    }
}

- (UITableViewCellEditingStyle)tableView:(UITableView *)tableView editingStyleForRowAtIndexPath:(NSIndexPath *)indexPath {
    return indexPath.section == UserSection ? UITableViewCellEditingStyleDelete : UITableViewCellEditingStyleNone;
}

- (void)tableView:(UITableView *)tableView commitEditingStyle:(UITableViewCellEditingStyle)editingStyle forRowAtIndexPath:(NSIndexPath *)indexPath {

}

- (UISwipeActionsConfiguration *)tableView:(UITableView *)tableView trailingSwipeActionsConfigurationForRowAtIndexPath:(NSIndexPath *)indexPath {
    if (tableView.isEditing) {
        return nil;
    } else {
        UIContextualAction *deleteAction = [UIContextualAction contextualActionWithStyle:UIContextualActionStyleDestructive title:@"Delete" handler:^(UIContextualAction *action, UIView *sourceView, void (^completionHandler)(BOOL)) {
            completionHandler(YES);
        }];
        UIContextualAction *duplicateAction = [UIContextualAction contextualActionWithStyle:UIContextualActionStyleNormal title:@"Duplicate" handler:^(UIContextualAction *action, UIView *sourceView, void (^completionHandler)(BOOL)) {
            completionHandler(YES);
        }];
        UISwipeActionsConfiguration *configuration = [UISwipeActionsConfiguration configurationWithActions:@[
            duplicateAction,
            deleteAction,
        ]];
        return configuration;
    }
}


@end
