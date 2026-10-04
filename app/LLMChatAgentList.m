//
//  LLMChatAgentList.m
//  iSH-AOK
//
//  The chats whose agents want attention: waiting for approval, working,
//  or finished with an answer nobody has read yet. Opened from the status
//  panel's "Other chats" line or /agents; picking one opens that chat.
//

#import "LLMChatInternal.h"
#import "LLMChatAgent.h"

typedef NS_ENUM(NSInteger, ISHLLMAgentListSection) {
    ISHLLMAgentListSectionApproval,
    ISHLLMAgentListSectionWorking,
    ISHLLMAgentListSectionFinished,
    ISHLLMAgentListSectionCount,
};

@implementation LLMAgentListViewController {
    NSArray<NSArray<ISHLLMAgent *> *> *_sections;
    NSTimer *_timer;
}

- (instancetype)init {
    return [super initWithStyle:UITableViewStyleInsetGrouped];
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = NSLocalizedString(@"Agents", @"agent list screen title");
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(reload) name:ISHLLMAgentStateDidChangeNotification object:nil];
    [self reload];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    __weak typeof(self) weakSelf = self;
    _timer = [NSTimer scheduledTimerWithTimeInterval:1.0 repeats:YES block:^(NSTimer *timer) {
        typeof(self) self = weakSelf;
        if (self == nil) {
            [timer invalidate];
            return;
        }
        [self.tableView reloadData];
    }];
}

- (void)viewWillDisappear:(BOOL)animated {
    [super viewWillDisappear:animated];
    [_timer invalidate];
    _timer = nil;
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
    [_timer invalidate];
}

- (void)reload {
    NSMutableArray<ISHLLMAgent *> *approval = [NSMutableArray array];
    NSMutableArray<ISHLLMAgent *> *working = [NSMutableArray array];
    NSMutableArray<ISHLLMAgent *> *finished = [NSMutableArray array];
    for (ISHLLMAgent *agent in [ISHLLMAgentManager.shared agentsWantingAttention]) {
        if (agent.pendingApproval != nil)
            [approval addObject:agent];
        else if (agent.busy)
            [working addObject:agent];
        else
            [finished addObject:agent];
    }
    _sections = @[approval, working, finished];
    [self.tableView reloadData];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView {
    (void) tableView;
    return ISHLLMAgentListSectionCount;
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    (void) tableView;
    return (NSInteger) _sections[(NSUInteger) section].count;
}

- (NSString *)tableView:(UITableView *)tableView titleForHeaderInSection:(NSInteger)section {
    (void) tableView;
    if (_sections[(NSUInteger) section].count == 0)
        return nil;
    switch ((ISHLLMAgentListSection) section) {
        case ISHLLMAgentListSectionApproval: return NSLocalizedString(@"Needs your approval", @"agent list section header");
        case ISHLLMAgentListSectionWorking: return NSLocalizedString(@"Working", @"agent list section header");
        case ISHLLMAgentListSectionFinished: return NSLocalizedString(@"New answers", @"agent list section header");
        case ISHLLMAgentListSectionCount: break;
    }
    return nil;
}

- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    (void) tableView;
    if (section != ISHLLMAgentListSectionCount - 1)
        return nil;
    BOOL empty = YES;
    for (NSArray *rows in _sections)
        empty = empty && rows.count == 0;
    return empty
        ? NSLocalizedString(@"No chat is working or waiting. A chat keeps working when you switch to another one or close the window; chats that need you show up here.", @"agent list footer")
        : NSLocalizedString(@"Each chat works on its own, with its own destination. A chat that needs approval waits until you open it. Swipe to stop one.", @"agent list footer");
}

static NSString *ISHLLMAgentListElapsed(NSDate *since) {
    if (since == nil)
        return @"";
    NSInteger seconds = MAX((NSInteger) 0, (NSInteger) -since.timeIntervalSinceNow);
    return [NSString stringWithFormat:@"%ld:%02ld", (long) (seconds / 60), (long) (seconds % 60)];
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:nil];
    ISHLLMAgent *agent = _sections[(NSUInteger) indexPath.section][(NSUInteger) indexPath.row];
    cell.textLabel.text = agent.title.length > 0 ? agent.title : NSLocalizedString(@"New Chat", @"title of a chat that has no name yet");
    NSMutableArray<NSString *> *parts = [NSMutableArray array];
    [parts addObject:[agent statusLine]];
    if (agent.phaseDetail.length > 0)
        [parts addObject:agent.phaseDetail];
    if (agent.busy)
        [parts addObject:ISHLLMAgentListElapsed(agent.replyStarted)];
    else if (agent.lastFinished != nil)
        [parts addObject:[NSDateFormatter localizedStringFromDate:agent.lastFinished dateStyle:NSDateFormatterNoStyle timeStyle:NSDateFormatterShortStyle]];
    if (agent.parent != nil)
        [parts addObject:[NSString stringWithFormat:NSLocalizedString(@"sub-agent of %@", @"agent list row detail; %@ is the parent chat"), agent.parent.title ?: NSLocalizedString(@"a chat", @"agent list row detail, parent chat with no name")]];
    cell.detailTextLabel.text = [parts componentsJoinedByString:@" · "];
    cell.detailTextLabel.textColor = agent.pendingApproval != nil ? UIColor.systemOrangeColor : UIColor.secondaryLabelColor;
    cell.accessoryType = [agent.sessionID isEqualToString:self.currentSessionID] ? UITableViewCellAccessoryCheckmark : UITableViewCellAccessoryDisclosureIndicator;
    return cell;
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    NSString *sessionID = _sections[(NSUInteger) indexPath.section][(NSUInteger) indexPath.row].sessionID;
    void (^selected)(NSString *) = self.agentSelected;
    [self dismissViewControllerAnimated:YES completion:^{
        if (selected != nil)
            selected(sessionID);
    }];
}

- (UISwipeActionsConfiguration *)tableView:(UITableView *)tableView trailingSwipeActionsConfigurationForRowAtIndexPath:(NSIndexPath *)indexPath {
    (void) tableView;
    ISHLLMAgent *agent = _sections[(NSUInteger) indexPath.section][(NSUInteger) indexPath.row];
    if (!agent.busy)
        return nil;
    UIContextualAction *stop = [UIContextualAction contextualActionWithStyle:UIContextualActionStyleDestructive title:NSLocalizedString(@"Stop", @"swipe action, stop an agent")
                                                                      handler:^(__unused UIContextualAction *action, __unused UIView *sourceView, void (^completion)(BOOL)) {
        [agent stop];
        completion(YES);
    }];
    return [UISwipeActionsConfiguration configurationWithActions:@[stop]];
}

@end
