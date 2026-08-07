//
//  FontPickerViewController.m
//  iSH
//
//  Created by Theodore Dubois on 10/26/19.
//

#import <CoreText/CoreText.h>

#import "FontPickerViewController.h"
#import "UserPreferences.h"

// UIFontPickerViewController can only filter on UIFontDescriptorTraitMonoSpace, and a
// font that skips that trait is simply absent from it with no way to override. That
// hides fonts the terminal handles fine: CaskaydiaCove Nerd Font clears
// post.isFixedPitch where its Mono sibling sets it, so CoreText withholds the trait
// even though every ASCII glyph -- and its powerline icons besides -- sits on one
// shared advance. Hence our own list, which measures the glyphs rather than taking the
// metadata's word for it.

// The characters a terminal actually tiles. A font that draws these at one shared
// advance will line up, whatever it claims about itself.
static const UniChar kGridSamples[] = {'i', 'l', '.', 'M', 'W', 'm', '0', '@', '#', ' '};
#define kGridSampleCount (sizeof(kGridSamples) / sizeof(*kGridSamples))

static BOOL fontIsGridAligned(NSString *fontName) {
    CTFontRef font = CTFontCreateWithName((__bridge CFStringRef) fontName, 16, NULL);
    if (font == NULL)
        return NO;
    CGGlyph glyphs[kGridSampleCount];
    // Comes back false if any sample is missing, which also drops icon-only and
    // non-Latin fonts that would leave the terminal full of tofu.
    BOOL complete = CTFontGetGlyphsForCharacters(font, kGridSamples, glyphs, kGridSampleCount);
    BOOL uniform = complete;
    if (complete) {
        CGSize advances[kGridSampleCount];
        CTFontGetAdvancesForGlyphs(font, kCTFontOrientationHorizontal, glyphs, advances, kGridSampleCount);
        for (size_t i = 1; i < kGridSampleCount; i++) {
            if (fabs(advances[i].width - advances[0].width) > 0.01) {
                uniform = NO;
                break;
            }
        }
    }
    CFRelease(font);
    return uniform;
}

// The face to measure and preview with: the plainest one the family offers.
static NSString *regularFontNameForFamily(NSString *family) {
    NSArray<NSString *> *names = [UIFont fontNamesForFamilyName:family];
    for (NSString *name in names) {
        UIFontDescriptorSymbolicTraits traits = [UIFont fontWithName:name size:16].fontDescriptor.symbolicTraits;
        if (!(traits & (UIFontDescriptorTraitBold | UIFontDescriptorTraitItalic)))
            return name;
    }
    return names.firstObject;
}

static BOOL fontSuitsTerminal(NSString *fontName) {
    if ([UIFont fontWithName:fontName size:16].fontDescriptor.symbolicTraits & UIFontDescriptorTraitMonoSpace)
        return YES;
    return fontIsGridAligned(fontName);
}

enum {
    FilterSection,
    FontSection,
    NumberOfSections,
};

@interface FontPickerViewController () <UISearchResultsUpdating>
@end

@implementation FontPickerViewController {
    NSArray<NSString *> *_gridAlignedFamilies;
    NSArray<NSString *> *_allFamilies;
    NSArray<NSString *> *_visibleFamilies;
    NSDictionary<NSString *, NSString *> *_previewNames;
    BOOL _showsAllFonts;
    UISearchController *_search;
}

- (instancetype)init {
    return [super initWithStyle:UITableViewStyleInsetGrouped];
}

// Measuring every family costs about a quarter second the first time, because each
// font has to be faulted in; afterwards CoreText's own caches make it negligible. So
// warm those caches in the background while the user is still on the settings screen,
// the same reason the system picker used to be built ahead of time.
+ (void)prewarm {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            NSArray<NSString *> *families = CFBridgingRelease(CTFontManagerCopyAvailableFontFamilyNames());
            for (NSString *family in families) {
                CTFontDescriptorRef descriptor = CTFontDescriptorCreateWithAttributes((__bridge CFDictionaryRef) @{
                    (id) kCTFontFamilyNameAttribute: family,
                });
                CTFontRef font = CTFontCreateWithFontDescriptor(descriptor, 16, NULL);
                if (font != NULL) {
                    CGGlyph glyphs[kGridSampleCount];
                    CTFontGetGlyphsForCharacters(font, kGridSamples, glyphs, kGridSampleCount);
                    CFRelease(font);
                }
                CFRelease(descriptor);
            }
        });
    });
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Font";
    self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:@"Reset" style:UIBarButtonItemStylePlain target:self action:@selector(resetFont:)];
    [self.tableView registerClass:UITableViewCell.class forCellReuseIdentifier:@"Font"];
    [self.tableView registerClass:UITableViewCell.class forCellReuseIdentifier:@"Filter"];

    [self loadFamilies];

    _search = [[UISearchController alloc] initWithSearchResultsController:nil];
    _search.searchResultsUpdater = self;
    _search.obscuresBackgroundDuringPresentation = NO;
    _search.searchBar.placeholder = @"Search";
    self.navigationItem.searchController = _search;
    self.navigationItem.hidesSearchBarWhenScrolling = NO;
    self.definesPresentationContext = YES;
}

- (void)loadFamilies {
    NSMutableArray<NSString *> *all = [NSMutableArray new];
    NSMutableArray<NSString *> *gridAligned = [NSMutableArray new];
    NSMutableDictionary<NSString *, NSString *> *previewNames = [NSMutableDictionary new];
    for (NSString *family in [UIFont.familyNames sortedArrayUsingSelector:@selector(localizedStandardCompare:)]) {
        NSString *name = regularFontNameForFamily(family);
        if (name == nil)
            continue;
        previewNames[family] = name;
        [all addObject:family];
        if (fontSuitsTerminal(name))
            [gridAligned addObject:family];
    }
    _allFamilies = all;
    _gridAlignedFamilies = gridAligned;
    _previewNames = previewNames;

    // If the font already in use only turns up in the full list, start there, so its
    // checkmark is somewhere the user can see it.
    NSString *current = UserPreferences.shared.fontFamily;
    _showsAllFonts = current != nil && ![gridAligned containsObject:current] && [all containsObject:current];
    [self updateVisibleFamilies];
}

- (void)updateVisibleFamilies {
    NSArray<NSString *> *families = _showsAllFonts ? _allFamilies : _gridAlignedFamilies;
    NSString *query = _search.searchBar.text;
    if (query.length > 0) {
        families = [families filteredArrayUsingPredicate:[NSPredicate predicateWithBlock:^BOOL(NSString *family, NSDictionary *bindings) {
            return [family localizedCaseInsensitiveContainsString:query];
        }]];
    }
    _visibleFamilies = families;
}

- (BOOL)isSearching {
    return _search.isActive && _search.searchBar.text.length > 0;
}

#pragma mark - Table view data source

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView {
    return NumberOfSections;
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    switch (section) {
        case FilterSection: return self.isSearching ? 0 : 1;
        case FontSection: return _visibleFamilies.count;
        default: NSAssert(NO, @"unhandled section"); return 0;
    }
}

- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    if (section == FilterSection && !self.isSearching)
        return @"Some installed fonts, several Nerd Fonts among them, never report themselves as monospaced. Turn this on to choose from every font on the device.";
    return nil;
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    if (indexPath.section == FilterSection) {
        UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:@"Filter" forIndexPath:indexPath];
        cell.textLabel.text = @"Show All Fonts";
        cell.selectionStyle = UITableViewCellSelectionStyleNone;
        UISwitch *toggle = [UISwitch new];
        toggle.on = _showsAllFonts;
        [toggle addTarget:self action:@selector(showAllFontsChanged:) forControlEvents:UIControlEventValueChanged];
        cell.accessoryView = toggle;
        return cell;
    }

    UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:@"Font" forIndexPath:indexPath];
    NSString *family = _visibleFamilies[indexPath.row];
    UIFont *font = [UIFont fontWithName:_previewNames[family] size:18];
    cell.textLabel.font = [[UIFontMetrics metricsForTextStyle:UIFontTextStyleBody] scaledFontForFont:font];
    cell.textLabel.adjustsFontForContentSizeCategory = YES;
    cell.textLabel.text = family;
    if ([family isEqualToString:UserPreferences.shared.fontFamily]) {
        cell.accessoryType = UITableViewCellAccessoryCheckmark;
        cell.accessibilityTraits |= UIAccessibilityTraitSelected;
    } else {
        cell.accessoryType = UITableViewCellAccessoryNone;
        cell.accessibilityTraits &= ~UIAccessibilityTraitSelected;
    }
    return cell;
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    if (indexPath.section != FontSection)
        return;
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    UserPreferences.shared.fontFamily = _visibleFamilies[indexPath.row];
    [self dismissSearchAndPop];
}

#pragma mark - Actions

- (void)showAllFontsChanged:(UISwitch *)sender {
    _showsAllFonts = sender.on;
    [self updateVisibleFamilies];
    [self.tableView reloadSections:[NSIndexSet indexSetWithIndex:FontSection] withRowAnimation:UITableViewRowAnimationAutomatic];
}

- (void)resetFont:(UIBarButtonItem *)sender {
    UserPreferences.shared.fontFamily = nil;
    [self dismissSearchAndPop];
}

// The search controller is presented on top of us, and a pop issued while it is still
// going down gets swallowed, stranding the user on a screen they just chose from. Wait
// for it to finish before leaving.
- (void)dismissSearchAndPop {
    if (!_search.isActive) {
        [self.navigationController popViewControllerAnimated:YES];
        return;
    }
    [_search dismissViewControllerAnimated:NO completion:^{
        [self.navigationController popViewControllerAnimated:YES];
    }];
}

#pragma mark - UISearchResultsUpdating

- (void)updateSearchResultsForSearchController:(UISearchController *)searchController {
    [self updateVisibleFamilies];
    [self.tableView reloadData];
}

@end
