//
//  LicencesViewController.m
//  AOK-iSH
//
//  Most of the licences of what is built into AOK-iSH ask for their notice to
//  travel with the binary, so the texts ship in the app rather than behind a
//  link: Licences.json, written by tools/gen-licences.py from the very trees
//  the build compiles. This screen lists it, a section per group, and shows
//  one component's texts in full. Not a web view, for the same reason as the
//  Legal screen: what has to accompany the app must not need the network.
//

#import "LicencesViewController.h"

@interface ISHLicence : NSObject
@property (nonatomic, copy) NSString *name;
@property (nonatomic, copy) NSString *version;
@property (nonatomic, copy) NSString *licence;
@property (nonatomic, copy) NSString *url;
@property (nonatomic, copy) NSArray<NSArray *> *texts; // [title, index into the texts array]
@end

@implementation ISHLicence
@end

@interface ISHLicenceSection : NSObject
@property (nonatomic, copy) NSString *group;
@property (nonatomic, copy) NSArray<ISHLicence *> *items;
@end

@implementation ISHLicenceSection
@end

static NSString *ISHStringOrEmpty(id value) {
    return [value isKindOfClass:NSString.class] ? value : @"";
}

// The sections and the shared texts, read once: the file is most of a
// megabyte and does not change while the app runs.
static BOOL ISHLoadLicences(NSArray<ISHLicenceSection *> **sectionsOut, NSArray<NSString *> **textsOut) {
    static NSArray<ISHLicenceSection *> *sections;
    static NSArray<NSString *> *texts;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSURL *url = [NSBundle.mainBundle URLForResource:@"Licences" withExtension:@"json"];
        NSData *data = url ? [NSData dataWithContentsOfURL:url] : nil;
        NSDictionary *root = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
        if (![root isKindOfClass:NSDictionary.class])
            return;
        NSArray *rawTexts = root[@"texts"];
        NSArray *groups = root[@"groups"];
        NSArray *components = root[@"components"];
        if (![rawTexts isKindOfClass:NSArray.class] || ![groups isKindOfClass:NSArray.class] ||
            ![components isKindOfClass:NSArray.class])
            return;
        NSMutableDictionary<NSString *, NSMutableArray<ISHLicence *> *> *byGroup = [NSMutableDictionary new];
        for (NSDictionary *c in components) {
            if (![c isKindOfClass:NSDictionary.class])
                continue;
            ISHLicence *licence = [ISHLicence new];
            licence.name = ISHStringOrEmpty(c[@"name"]);
            licence.version = ISHStringOrEmpty(c[@"version"]);
            licence.licence = ISHStringOrEmpty(c[@"licence"]);
            licence.url = ISHStringOrEmpty(c[@"url"]);
            licence.texts = [c[@"texts"] isKindOfClass:NSArray.class] ? c[@"texts"] : @[];
            NSString *group = ISHStringOrEmpty(c[@"group"]);
            if (!byGroup[group])
                byGroup[group] = [NSMutableArray new];
            [byGroup[group] addObject:licence];
        }
        NSMutableArray<ISHLicenceSection *> *list = [NSMutableArray new];
        for (NSString *group in groups) {
            if (![group isKindOfClass:NSString.class] || byGroup[group].count == 0)
                continue;
            ISHLicenceSection *section = [ISHLicenceSection new];
            section.group = group;
            section.items = byGroup[group];
            [list addObject:section];
        }
        sections = list;
        texts = rawTexts;
    });
    *sectionsOut = sections;
    *textsOut = texts;
    return sections != nil;
}

static NSString *_Nullable ISHLicenceGroupTitle(NSString *group) {
    if ([group isEqualToString:@"components"])
        return NSLocalizedString(@"Built into the app", @"Licences screen section header: the libraries and programs compiled into or shipped inside AOK-iSH");
    if ([group isEqualToString:@"crates"])
        return NSLocalizedString(@"Rust crates", @"Licences screen section header: the Rust libraries (crates) compiled into the app, mostly for the Helix editor");
    if ([group isEqualToString:@"rust-std"])
        return NSLocalizedString(@"Rust standard library", @"Licences screen section header: the Rust standard library and the crates it is built from");
    return nil;
}

static NSString *ISHLicenceTitle(ISHLicence *licence) {
    return licence.version.length ? [NSString stringWithFormat:@"%@ %@", licence.name, licence.version] : licence.name;
}

#pragma mark - One component

// Licence files are hard-wrapped at 70-80 columns for a terminal, and in a
// proportional font on a phone every one of those breaks lands mid-line: "...any
// express or implied warranty. In" / "no event will...". So a paragraph is
// joined up again for display -- only the line breaks change, never a word, and
// the stored text stays exactly as each project ships it. A line is joined to
// the next only if it is long enough to have been wrapped at the margin, has
// more than one word (a file name in a list does not), both have letters in
// them (so rules of === or --- stay), it does not end in a colon, and the next
// does not start a list item ("1.", "(a)", "-", "*") or a "Field:" line, as in
// Debian's copyright files.
static BOOL ISHLicenceLinesJoin(NSString *line, NSString *next) {
    static NSRegularExpression *item;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        item = [NSRegularExpression regularExpressionWithPattern:
            @"^(?:[-*•+]\\s|\\(?[0-9]{1,3}[.)]\\s|\\([A-Za-z0-9]{1,4}\\)\\s|[A-Za-z]\\)\\s|[A-Z][A-Za-z-]*:(?:\\s|$))" options:0 error:nil];
    });
    if (line.length < 50)
        return NO;
    NSString *trimmedLine = [line stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
    NSString *trimmedNext = [next stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
    if (trimmedNext.length == 0 || [trimmedLine hasSuffix:@":"] ||
        [trimmedLine rangeOfCharacterFromSet:NSCharacterSet.whitespaceCharacterSet].location == NSNotFound)
        return NO;
    if ([trimmedLine rangeOfCharacterFromSet:NSCharacterSet.letterCharacterSet].location == NSNotFound ||
        [trimmedNext rangeOfCharacterFromSet:NSCharacterSet.letterCharacterSet].location == NSNotFound)
        return NO;
    return [item firstMatchInString:trimmedNext options:0 range:NSMakeRange(0, trimmedNext.length)] == nil;
}

static NSString *ISHReflowLicenceText(NSString *text) {
    NSArray<NSString *> *lines = [text componentsSeparatedByString:@"\n"];
    NSMutableArray<NSString *> *out = [NSMutableArray arrayWithCapacity:lines.count];
    NSString *previous = nil; // the last physical line, which decides a join
    for (NSString *line in lines) {
        if (previous && out.count && ISHLicenceLinesJoin(previous, line)) {
            NSString *joined = [out.lastObject stringByAppendingFormat:@" %@",
                [line stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet]];
            [out replaceObjectAtIndex:out.count - 1 withObject:joined];
        } else {
            [out addObject:line];
        }
        previous = line;
    }
    return [out componentsJoinedByString:@"\n"];
}

@interface ISHLicenceTextViewController : UIViewController
@property (nonatomic) ISHLicence *licence;
@property (nonatomic, copy) NSArray<NSString *> *texts;
@end

@implementation ISHLicenceTextViewController

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = self.licence.name;
    self.navigationItem.largeTitleDisplayMode = UINavigationItemLargeTitleDisplayModeNever;
    self.view.backgroundColor = UIColor.systemBackgroundColor;

    // Licence texts are English wherever they are read: they are the legal
    // instrument, and a translation is not. So is this header around them.
    NSMutableString *text = [NSMutableString stringWithString:ISHLicenceTitle(self.licence)];
    if (self.licence.licence.length)
        [text appendFormat:@"\n%@", self.licence.licence];
    if (self.licence.url.length)
        [text appendFormat:@"\n%@", self.licence.url];
    for (NSArray *ref in self.licence.texts) {
        if (ref.count != 2 || ![ref[0] isKindOfClass:NSString.class] || ![ref[1] isKindOfClass:NSNumber.class])
            continue;
        NSUInteger index = [ref[1] unsignedIntegerValue];
        if (index >= self.texts.count)
            continue;
        [text appendFormat:@"\n\n— %@ —\n\n%@", ref[0], ISHReflowLicenceText(self.texts[index])];
    }

    UITextView *textView = [[UITextView alloc] initWithFrame:CGRectZero];
    textView.editable = NO;
    textView.selectable = YES;
    textView.alwaysBounceVertical = YES;
    textView.backgroundColor = UIColor.clearColor;
    textView.font = [UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];
    textView.adjustsFontForContentSizeCategory = YES;
    textView.textContainerInset = UIEdgeInsetsMake(16, 16, 32, 16);
    textView.dataDetectorTypes = UIDataDetectorTypeLink;
    textView.translatesAutoresizingMaskIntoConstraints = NO;
    textView.text = text;
    [self.view addSubview:textView];
    [NSLayoutConstraint activateConstraints:@[
        [textView.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor],
        [textView.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor],
        [textView.leadingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.leadingAnchor],
        [textView.trailingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.trailingAnchor],
    ]];
}

@end

#pragma mark - The list

@interface ISHLicencesViewController : UITableViewController <UISearchResultsUpdating>
@end

@implementation ISHLicencesViewController {
    NSArray<ISHLicenceSection *> *_allSections;
    NSArray<ISHLicenceSection *> *_sections;
    NSArray<NSString *> *_texts;
    UISearchController *_search;
}

- (instancetype)init {
    return [super initWithStyle:UITableViewStyleInsetGrouped];
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = NSLocalizedString(@"Licences", @"Title of the Licences screen, the Settings row that opens it, and the Legal screen's button that opens it");
    self.navigationItem.largeTitleDisplayMode = UINavigationItemLargeTitleDisplayModeNever;
    NSArray<ISHLicenceSection *> *sections = nil;
    NSArray<NSString *> *texts = nil;
    ISHLoadLicences(&sections, &texts);
    _allSections = sections ?: @[];
    _sections = _allSections;
    _texts = texts ?: @[];

    _search = [[UISearchController alloc] initWithSearchResultsController:nil];
    _search.searchResultsUpdater = self;
    _search.obscuresBackgroundDuringPresentation = NO;
    self.navigationItem.searchController = _search;
    self.navigationItem.hidesSearchBarWhenScrolling = NO;
    self.definesPresentationContext = YES;
}

- (void)updateSearchResultsForSearchController:(UISearchController *)searchController {
    NSString *query = [searchController.searchBar.text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
    if (query.length == 0) {
        _sections = _allSections;
    } else {
        NSMutableArray<ISHLicenceSection *> *filtered = [NSMutableArray new];
        for (ISHLicenceSection *section in _allSections) {
            NSIndexSet *hits = [section.items indexesOfObjectsPassingTest:^BOOL(ISHLicence *l, NSUInteger i, BOOL *stop) {
                return [l.name localizedCaseInsensitiveContainsString:query] ||
                       [l.licence localizedCaseInsensitiveContainsString:query];
            }];
            if (hits.count == 0)
                continue;
            ISHLicenceSection *s = [ISHLicenceSection new];
            s.group = section.group;
            s.items = [section.items objectsAtIndexes:hits];
            [filtered addObject:s];
        }
        _sections = filtered;
    }
    [self.tableView reloadData];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView {
    return _sections.count;
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    return _sections[section].items.count;
}

- (NSString *)tableView:(UITableView *)tableView titleForHeaderInSection:(NSInteger)section {
    return ISHLicenceGroupTitle(_sections[section].group);
}

- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    if (![_sections[section].group isEqualToString:@"app"])
        return nil;
    return NSLocalizedString(@"AOK-iSH and the open-source software built into it, each with its licence and the notices it asks to be shipped with.",
                             @"Licences screen: the note under AOK-iSH's own entry, saying what the list is");
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:@"Licence"];
    if (!cell) {
        cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:@"Licence"];
        cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
        cell.textLabel.numberOfLines = 0;
        cell.detailTextLabel.numberOfLines = 0;
        cell.detailTextLabel.textColor = UIColor.secondaryLabelColor;
    }
    ISHLicence *licence = _sections[indexPath.section].items[indexPath.row];
    cell.textLabel.text = ISHLicenceTitle(licence);
    cell.detailTextLabel.text = licence.licence;
    return cell;
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    ISHLicenceTextViewController *detail = [ISHLicenceTextViewController new];
    detail.licence = _sections[indexPath.section].items[indexPath.row];
    detail.texts = _texts;
    [self.navigationController pushViewController:detail animated:YES];
}

@end

UIViewController *ISHCreateLicencesViewController(void) {
    return [ISHLicencesViewController new];
}
