#import <Cocoa/Cocoa.h>

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static NSStatusItem *gStatusItem;
static NSMenu       *gMenu;
static NSObject     *gOutputLock;
static NSString     *gInitialIcon;
static NSString     *gInitialTooltip;

// Log window
static NSWindow     *gLogWindow;
static NSTextView   *gLogTextView;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void emit(NSString *method, NSDictionary *params) {
    @synchronized (gOutputLock) {
        NSMutableDictionary *msg = [NSMutableDictionary dictionary];
        msg[@"method"] = method;
        if (params) msg[@"params"] = params;

        NSData *data = [NSJSONSerialization dataWithJSONObject:msg options:0 error:nil];
        if (data) {
            fwrite(data.bytes, 1, data.length, stdout);
            fputc('\n', stdout);
            fflush(stdout);
        }
    }
}

// Vector-based icon drawing: sharper on Retina and smaller binary footprint
static NSImage *createDefaultIcon() {
    return [NSImage imageWithSize:NSMakeSize(22, 22) flipped:NO drawingHandler:^BOOL(NSRect dstRect) {
        [[NSColor colorWithRed:0.18 green:0.68 blue:0.20 alpha:1.0] setFill];
        [[NSBezierPath bezierPathWithOvalInRect:NSInsetRect(dstRect, 3, 3)] fill];
        return YES;
    }];
}

static NSImage *processImage(NSData *data) {
    NSImage *img = [[NSImage alloc] initWithData:data];
    if (img) {
        [img setSize:NSMakeSize(22, 22)];
        [img setTemplate:YES]; // Allows Dark/Light mode switching
    }
    return img;
}

// ---------------------------------------------------------------------------
// Log Window
// ---------------------------------------------------------------------------

@interface LogWindowDelegate : NSObject <NSWindowDelegate>
@end

@implementation LogWindowDelegate
- (BOOL)windowShouldClose:(NSWindow *)sender {
    if (gParentDead) {
        // Parent is gone — actually exit
        [NSApp terminate:nil];
        return YES;
    }
    // Hide instead of closing — minimize to tray
    [sender orderOut:nil];
    return NO;
}
@end

static LogWindowDelegate *gLogWindowDelegate;
static BOOL gParentDead = NO;

static void createLogWindow(void) {
    NSRect frame = NSMakeRect(200, 200, 800, 500);
    gLogWindow = [[NSWindow alloc]
        initWithContentRect:frame
        styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                   NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
        backing:NSBackingStoreBuffered
        defer:NO];
    [gLogWindow setTitle:@"Console"];
    [gLogWindow setMinSize:NSMakeSize(400, 200)];

    gLogWindowDelegate = [[LogWindowDelegate alloc] init];
    [gLogWindow setDelegate:gLogWindowDelegate];

    // Create scroll view + text view
    NSScrollView *scrollView = [[NSScrollView alloc] initWithFrame:[[gLogWindow contentView] bounds]];
    [scrollView setHasVerticalScroller:YES];
    [scrollView setHasHorizontalScroller:NO];
    [scrollView setAutoresizingMask:(NSViewWidthSizable | NSViewHeightSizable)];

    gLogTextView = [[NSTextView alloc] initWithFrame:[[scrollView contentView] bounds]];
    [gLogTextView setEditable:NO];
    [gLogTextView setSelectable:YES];
    [gLogTextView setAutoresizingMask:NSViewWidthSizable];
    [gLogTextView setFont:[NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightRegular]];

    // Dark theme
    [gLogTextView setBackgroundColor:[NSColor colorWithRed:0.12 green:0.12 blue:0.12 alpha:1.0]];
    [gLogTextView setTextColor:[NSColor colorWithRed:0.80 green:0.80 blue:0.80 alpha:1.0]];
    [gLogTextView setInsertionPointColor:[NSColor whiteColor]];

    // Allow horizontal scrolling for long lines
    [[gLogTextView textContainer] setWidthTracksTextView:NO];
    [[gLogTextView textContainer] setContainerSize:NSMakeSize(CGFLOAT_MAX, CGFLOAT_MAX)];
    [gLogTextView setHorizontallyResizable:YES];
    [scrollView setHasHorizontalScroller:YES];

    [scrollView setDocumentView:gLogTextView];
    [[gLogWindow contentView] addSubview:scrollView];

    [gLogWindow makeKeyAndOrderFront:nil];
}

static void appendLogText(NSString *text) {
    if (!gLogTextView) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        NSTextStorage *storage = [gLogTextView textStorage];
        NSDictionary *attrs = @{
            NSForegroundColorAttributeName: [NSColor colorWithRed:0.80 green:0.80 blue:0.80 alpha:1.0],
            NSFontAttributeName: [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightRegular],
        };
        NSAttributedString *str = [[NSAttributedString alloc] initWithString:text attributes:attrs];
        [storage appendAttributedString:str];
        // Auto-scroll to bottom
        [gLogTextView scrollRangeToVisible:NSMakeRange([[storage string] length], 0)];
    });
}

static void showLogWindow(void) {
    dispatch_async(dispatch_get_main_queue(), ^{
        [gLogWindow makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
    });
}

static void hideLogWindow(void) {
    dispatch_async(dispatch_get_main_queue(), ^{
        [gLogWindow orderOut:nil];
    });
}

// ---------------------------------------------------------------------------
// Menu Implementation
// ---------------------------------------------------------------------------

@interface TrayMenuTarget : NSObject <NSMenuDelegate> @end
@implementation TrayMenuTarget
- (void)menuItemClicked:(NSMenuItem *)sender {
    if (sender.representedObject) emit(@"clicked", @{@"id": sender.representedObject});
}
- (void)menuWillOpen:(NSMenu *)menu { emit(@"menuRequested", nil); }
@end

static TrayMenuTarget *gTarget;

static void buildMenuItems(NSMenu *menu, NSArray *items) {
    for (NSDictionary *cfg in items) {
        @autoreleasepool {
            if ([cfg[@"separator"] boolValue]) {
                [menu addItem:[NSMenuItem separatorItem]];
                continue;
            }

            NSMenuItem *mi = [[NSMenuItem alloc] initWithTitle:cfg[@"title"] ?: @""
                                                        action:@selector(menuItemClicked:)
                                                 keyEquivalent:@""];
            mi.target = gTarget;
            mi.representedObject = cfg[@"id"];
            mi.toolTip = cfg[@"tooltip"];
            mi.enabled = cfg[@"enabled"] ? [cfg[@"enabled"] boolValue] : YES;
            if ([cfg[@"checked"] boolValue]) mi.state = NSControlStateValueOn;

            NSArray *children = cfg[@"items"];
            if (children.count > 0) {
                NSMenu *sub = [[NSMenu alloc] initWithTitle:cfg[@"title"] ?: @""];
                buildMenuItems(sub, children);
                mi.submenu = sub;
            }
            [menu addItem:mi];
        }
    }
}

// ---------------------------------------------------------------------------
// IO Logic
// ---------------------------------------------------------------------------

static void stdinReaderThread(void) {
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;

    while ((len = getline(&line, &cap, stdin)) > 0) {
        @autoreleasepool {
            if (line[len - 1] == '\n') line[--len] = '\0';
            if (len == 0) continue;

            NSData *data = [NSData dataWithBytesNoCopy:line length:len freeWhenDone:NO];
            NSDictionary *msg = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
            if (!msg) continue;

            NSString *method = msg[@"method"];
            NSDictionary *params = msg[@"params"];

            if ([method isEqualToString:@"setIcon"]) {
                NSData *iconData = [[NSData alloc] initWithBase64EncodedString:params[@"base64"] ?: @"" options:0];
                dispatch_async(dispatch_get_main_queue(), ^{
                    if (iconData) gStatusItem.button.image = processImage(iconData);
                });
            } else if ([method isEqualToString:@"appendLog"]) {
                NSString *text = params[@"text"];
                if (text) appendLogText(text);
            } else if ([method isEqualToString:@"showWindow"]) {
                showLogWindow();
            } else if ([method isEqualToString:@"hideWindow"]) {
                hideLogWindow();
            } else if ([method isEqualToString:@"setTitle"]) {
                NSString *title = params[@"text"];
                if (title) {
                    dispatch_async(dispatch_get_main_queue(), ^{
                        [gLogWindow setTitle:title];
                    });
                }
            } else {
                dispatch_async(dispatch_get_main_queue(), ^{
                    if ([method isEqualToString:@"setMenu"]) {
                        [gMenu removeAllItems];
                        buildMenuItems(gMenu, params[@"items"]);
                    } else if ([method isEqualToString:@"setTooltip"]) {
                        gStatusItem.button.toolTip = params[@"text"];
                    }
                });
            }
        }
    }
    free(line);
    // stdin closed — parent process exited/crashed. Show the log window
    // with an exit message so the user can see what went wrong.
    gParentDead = YES;
    appendLogText(@"\n--- Process exited ---\n");
    showLogWindow();
}

// ---------------------------------------------------------------------------
// Entry Point
// ---------------------------------------------------------------------------

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        gOutputLock = [[NSObject alloc] init];
        gTarget = [[TrayMenuTarget alloc] init];

        // Simple arg parser
        for (int i = 1; i < argc; i++) {
            NSString *arg = [NSString stringWithUTF8String:argv[i]];
            if ([arg isEqualToString:@"--icon"] && i + 1 < argc) gInitialIcon = [NSString stringWithUTF8String:argv[++i]];
            if ([arg isEqualToString:@"--tooltip"] && i + 1 < argc) gInitialTooltip = [NSString stringWithUTF8String:argv[++i]];
        }

        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyAccessory];

        gStatusItem = [[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength];
        gMenu = [[NSMenu alloc] init];
        gMenu.delegate = gTarget;
        gStatusItem.menu = gMenu;

        @autoreleasepool {
            NSImage *img = gInitialIcon ? [[NSImage alloc] initWithContentsOfFile:gInitialIcon] : nil;
            gStatusItem.button.image = img ? processImage([img TIFFRepresentation]) : createDefaultIcon();
            gStatusItem.button.toolTip = gInitialTooltip ?: @"Tray";
        }

        // Create log window
        createLogWindow();

        emit(@"ready", nil);
        [NSThread detachNewThreadWithBlock:^{ stdinReaderThread(); }];
        [app run];
    }
    return 0;
}
