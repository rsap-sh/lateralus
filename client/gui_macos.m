/*
 * gui_macos.m — Native AppKit (Cocoa) frontend for Lateralus voice chat.
 *
 * Why AppKit instead of SDL/ImGui on macOS:
 *   - System font (San Francisco), Retina-crisp rendering, Dark Mode aware
 *   - App menu bar shows "Lateralus" (via embedded Info.plist at link time)
 *   - NSRunLoop is fully event-driven — idle CPU ≈ 0 %
 *   - NSUserDefaults persists last-used Server / Room / Name across launches
 *
 * Compiled as Objective-C (.m).  The audio engine (client.c) is a separate
 * translation unit; we call it through vc_api.h.
 */

#import <Cocoa/Cocoa.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include "../common/vc_api.h"

/* ── Engine thread ──────────────────────────────────────────────────────── */

typedef struct { char host[256], room[64], user[64], psk[256]; } EngArgs;
static EngArgs      s_args;
static pthread_t    s_thread;
static volatile int s_running = 0;

static void *eng_fn(void *p)
{
    EngArgs *a = (EngArgs *)p;
    vc_run(a->host, a->room, a->user, a->psk);
    s_running = 0;
    return NULL;
}

/* ── NSUserDefaults keys ─────────────────────────────────────────────────── */

static NSString *const kServer = @"vc_server";
static NSString *const kRoom   = @"vc_room";
static NSString *const kUser   = @"vc_user";

/* ── Application delegate ───────────────────────────────────────────────── */

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property (strong) NSWindow          *win;
/* Connect view */
@property (strong) NSView            *connectView;
@property (strong) NSTextField       *serverField, *roomField, *userField;
@property (strong) NSSecureTextField *pskField;
@property (strong) NSButton          *connectBtn, *updateBtn, *debugBtn;
@property (strong) NSTextField       *errorLabel;
/* Room view */
@property (strong) NSView            *roomView;
@property (strong) NSTextField       *roomLabel;
@property (strong) NSTextView        *peerView;
@property (strong) NSButton          *muteBtn, *disconnectBtn;
/* Refresh timer */
@property (strong) NSTimer           *timer;
@end

@implementation AppDelegate

/* ── Lifecycle ──────────────────────────────────────────────────────────── */

- (void)applicationDidFinishLaunching:(NSNotification *)n
{
    (void)n;
    /* Suppress stdout/stderr — GUI app; user can enable via "Debug logs" checkbox */
    freopen("/dev/null", "w", stdout);
    freopen("/dev/null", "w", stderr);
    [self buildMenu];
    [self buildWindow];
    [_win makeKeyAndOrderFront:nil];
    /* 10 Hz tick for peer-list updates — RunLoop sleeps the rest of the time */
    _timer = [NSTimer scheduledTimerWithTimeInterval:0.1
                      target:self selector:@selector(tick)
                      userInfo:nil repeats:YES];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)a
{
    (void)a; return YES;
}

- (void)applicationWillTerminate:(NSNotification *)n
{
    (void)n;
    if (s_running) { vc_quit(); pthread_join(s_thread, NULL); }
}

/* ── Menu bar ────────────────────────────────────────────────────────────── */

- (void)buildMenu
{
    NSMenu *bar = [[NSMenu alloc] init];
    [NSApp setMainMenu:bar];

    NSMenuItem *appItem = [[NSMenuItem alloc] init];
    [bar addItem:appItem];
    NSMenu *appMenu = [[NSMenu alloc] init];
    [appItem setSubmenu:appMenu];
    [appMenu addItemWithTitle:@"About Lateralus"
                       action:@selector(orderFrontStandardAboutPanel:)
                keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Quit Lateralus"
                       action:@selector(terminate:)
                keyEquivalent:@"q"];
}

/* ── Window ──────────────────────────────────────────────────────────────── */

- (void)buildWindow
{
    NSRect r = NSMakeRect(0, 0, 340, 430);
    _win = [[NSWindow alloc]
        initWithContentRect:r
        styleMask:(NSWindowStyleMaskTitled |
                   NSWindowStyleMaskClosable |
                   NSWindowStyleMaskMiniaturizable)
        backing:NSBackingStoreBuffered
        defer:NO];
    _win.title      = @"Lateralus";
    _win.delegate   = self;
    _win.restorable = NO;
    [_win center];

    [self buildConnectView:r];
    [self buildRoomView:r];
    _win.contentView = _connectView;
}

/* ── Connect view ─────────────────────────────────────────────────────────── */

- (void)buildConnectView:(NSRect)r
{
    CGFloat W = r.size.width;
    _connectView = [[NSView alloc] initWithFrame:r];

    /* Title */
    NSTextField *title = [NSTextField labelWithString:@"Lateralus"];
    title.font      = [NSFont boldSystemFontOfSize:22];
    title.alignment = NSTextAlignmentCenter;
    title.frame     = NSMakeRect(0, 370, W, 32);
    [_connectView addSubview:title];

    /* Input rows: {label, field ptr, secure, defaults-key, placeholder} */
    NSUserDefaults *ud = [NSUserDefaults standardUserDefaults];
    struct {
        NSString *lbl;
        NSTextField * __strong *fld;
        BOOL sec;
        NSString *key, *ph;
    } rows[] = {
        { @"Server", &_serverField,              NO,  kServer, @"hostname or IP"    },
        { @"Room",   &_roomField,                NO,  kRoom,   @"general"           },
        { @"Name",   &_userField,                NO,  kUser,   @""                  },
        { @"PSK",    (NSTextField **)&_pskField,  YES, nil,     @"optional passphrase" },
    };
    CGFloat rowY[] = { 318, 282, 246, 210 };

    for (int i = 0; i < 4; i++) {
        NSTextField *lbl = [NSTextField labelWithString:rows[i].lbl];
        lbl.font      = [NSFont systemFontOfSize:13];
        lbl.alignment = NSTextAlignmentRight;
        lbl.frame     = NSMakeRect(12, rowY[i], 56, 22);
        [_connectView addSubview:lbl];

        NSTextField *fld = rows[i].sec
            ? [[NSSecureTextField alloc] initWithFrame:NSMakeRect(76, rowY[i], W - 96, 22)]
            : [[NSTextField       alloc] initWithFrame:NSMakeRect(76, rowY[i], W - 96, 22)];
        fld.placeholderString = rows[i].ph;
        if (rows[i].key) {
            NSString *saved = [ud stringForKey:rows[i].key];
            if (saved.length) fld.stringValue = saved;
        }
        [_connectView addSubview:fld];
        *rows[i].fld = fld;
    }

    /* Default username from environment */
    if (!_userField.stringValue.length) {
        const char *u = getenv("USER") ?: getenv("USERNAME");
        _userField.stringValue = (u && u[0]) ? @(u) : @"user";
    }

    /* Connect button */
    _connectBtn = [[NSButton alloc] initWithFrame:NSMakeRect(W/2 - 75, 162, 150, 32)];
    _connectBtn.title        = @"Connect";
    _connectBtn.bezelStyle   = NSBezelStyleRounded;
    _connectBtn.keyEquivalent = @"\r";
    _connectBtn.target = self;
    _connectBtn.action = @selector(connectClicked);
    [_connectView addSubview:_connectBtn];

    /* Update button */
    _updateBtn = [[NSButton alloc] initWithFrame:NSMakeRect(W/2 - 75, 122, 150, 28)];
    _updateBtn.title      = @"Check for Updates";
    _updateBtn.bezelStyle = NSBezelStyleRecessed;
    _updateBtn.target = self;
    _updateBtn.action = @selector(updateClicked);
    [_connectView addSubview:_updateBtn];

    /* Error label */
    _errorLabel = [NSTextField labelWithString:@""];
    _errorLabel.textColor            = [NSColor labelColor];
    _errorLabel.alignment            = NSTextAlignmentCenter;
    _errorLabel.maximumNumberOfLines = 2;
    _errorLabel.frame = NSMakeRect(16, 82, W - 32, 32);
    [_connectView addSubview:_errorLabel];

    /* Debug log checkbox — bottom center */
    _debugBtn = [[NSButton alloc] initWithFrame:NSMakeRect(W/2 - 52, 14, 104, 20)];
    [_debugBtn setButtonType:NSButtonTypeSwitch];
    _debugBtn.title  = @"Debug logs";
    _debugBtn.state  = NSControlStateValueOff;
    _debugBtn.target = self;
    _debugBtn.action = @selector(debugClicked);
    [_connectView addSubview:_debugBtn];

    /* Version label — top left, quaternary (very faint, dark-mode aware) */
#ifndef VC_COMMIT_COUNT
#  define VC_COMMIT_COUNT "0"
#  define VC_COMMIT_HASH  "dev"
#endif
    NSTextField *ver = [NSTextField labelWithString:
        [NSString stringWithFormat:@"#" VC_COMMIT_COUNT "  " VC_COMMIT_HASH]];
    ver.font      = [NSFont systemFontOfSize:10];
    ver.textColor = [NSColor quaternaryLabelColor];
    ver.frame     = NSMakeRect(8, r.size.height - 18, 160, 14);
    [_connectView addSubview:ver];
}

/* ── Room view ──────────────────────────────────────────────────────────── */

- (void)buildRoomView:(NSRect)r
{
    CGFloat W = r.size.width, H = r.size.height;
    _roomView = [[NSView alloc] initWithFrame:r];

    _roomLabel = [NSTextField labelWithString:@""];
    _roomLabel.font  = [NSFont boldSystemFontOfSize:14];
    _roomLabel.frame = NSMakeRect(12, H - 38, W - 24, 24);
    [_roomView addSubview:_roomLabel];

    NSScrollView *scroll = [[NSScrollView alloc]
        initWithFrame:NSMakeRect(12, 54, W - 24, H - 106)];
    scroll.hasVerticalScroller = YES;
    scroll.autohidesScrollers  = YES;
    scroll.borderType          = NSBezelBorder;

    _peerView = [[NSTextView alloc] initWithFrame:scroll.contentView.bounds];
    _peerView.editable            = NO;
    _peerView.selectable          = NO;
    _peerView.font                = [NSFont monospacedSystemFontOfSize:12
                                                               weight:NSFontWeightRegular];
    _peerView.textContainerInset  = NSMakeSize(4, 6);
    scroll.documentView = _peerView;
    [_roomView addSubview:scroll];

    CGFloat bw = (W - 36) / 2;
    _muteBtn = [[NSButton alloc] initWithFrame:NSMakeRect(12, 14, bw, 28)];
    _muteBtn.title      = @"Mute [M]";
    _muteBtn.bezelStyle = NSBezelStyleRounded;
    _muteBtn.target = self; _muteBtn.action = @selector(muteClicked);
    [_roomView addSubview:_muteBtn];

    _disconnectBtn = [[NSButton alloc] initWithFrame:NSMakeRect(24 + bw, 14, bw, 28)];
    _disconnectBtn.title      = @"Disconnect";
    _disconnectBtn.bezelStyle = NSBezelStyleRounded;
    _disconnectBtn.target = self; _disconnectBtn.action = @selector(disconnectClicked);
    [_roomView addSubview:_disconnectBtn];
}

/* ── Actions ────────────────────────────────────────────────────────────── */

- (void)connectClicked
{
    NSString *srv  = _serverField.stringValue;
    NSString *room = _roomField.stringValue.length ? _roomField.stringValue : @"general";
    NSString *user = _userField.stringValue;

    if (!srv.length || !user.length) {
        _errorLabel.stringValue = @"Server and Name are required.";
        return;
    }

    NSUserDefaults *ud = [NSUserDefaults standardUserDefaults];
    [ud setObject:srv  forKey:kServer];
    [ud setObject:room forKey:kRoom];
    [ud setObject:user forKey:kUser];

    strlcpy(s_args.host, srv.UTF8String,                   sizeof s_args.host);
    strlcpy(s_args.room, room.UTF8String,                  sizeof s_args.room);
    strlcpy(s_args.user, user.UTF8String,                  sizeof s_args.user);
    strlcpy(s_args.psk,  _pskField.stringValue.UTF8String, sizeof s_args.psk);

    s_running = 1;
    pthread_create(&s_thread, NULL, eng_fn, &s_args);

    _roomLabel.stringValue = [NSString stringWithFormat:@"# %@", room];
    _win.contentView = _roomView;
}

- (void)disconnectClicked
{
    if (s_running) {
        vc_quit();
        pthread_join(s_thread, NULL);
        s_running = 0;
    }
    const char *err = vc_last_error();
    _errorLabel.stringValue = (err && err[0]) ? @(err) : @"";
    _win.contentView = _connectView;
}

- (void)muteClicked { vc_set_muted(!vc_get_muted()); }

- (void)debugClicked
{
    vc_set_debug(_debugBtn.state == NSControlStateValueOn ? 1 : 0);
    if (vc_get_debug()) {
        freopen("/tmp/lateralus.log", "w", stdout);
        freopen("/tmp/lateralus.log", "w", stderr);
    } else {
        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);
    }
}

- (void)updateClicked
{
    _updateBtn.enabled = NO;
    _updateBtn.title   = @"Updating…";
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        int rc = do_update();
        dispatch_async(dispatch_get_main_queue(), ^{
            self->_updateBtn.enabled = YES;
            if (rc == 0) {
                self->_updateBtn.title = @"✓ Updated — restart to apply";
            } else {
                self->_updateBtn.title = @"Update failed";
                dispatch_after(
                    dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC),
                    dispatch_get_main_queue(),
                    ^{ self->_updateBtn.title = @"Check for Updates"; });
            }
        });
    });
}

/* ── Peer list refresh (10 Hz timer) ────────────────────────────────────── */

- (void)tick
{
    /* Switch back to connect view if engine stopped unexpectedly */
    if (!s_running && _win.contentView == _roomView) {
        [self disconnectClicked];
        return;
    }
    if (!s_running) return;

    BOOL connected = vc_is_connected() != 0;
    BOOL muted     = vc_get_muted()    != 0;

    _muteBtn.title = muted ? @"Unmute [M]" : @"Mute [M]";

    if (!connected) {
        _roomLabel.stringValue =
            [NSString stringWithFormat:@"Connecting to %s…", s_args.host];
        _peerView.string = @"Connecting…";
        return;
    }

    _roomLabel.stringValue =
        [NSString stringWithFormat:@"# %s", vc_room_name()];

    NSMutableString *txt = [NSMutableString string];
    [txt appendFormat:(muted ? @"\xe2\x97\x8b %s  (you, muted)\n" : @"\xe2\x97\x8f %s  (you)\n"),
                       vc_username()];

    vc_peer_snapshot_t peers[32];
    int n = vc_snapshot_peers(peers, 32);
    for (int i = 0; i < n; i++) {
        vc_peer_snapshot_t *p = &peers[i];
        const char *path = p->direct_ok    ? "P2P"  :
                           p->direct_known ? "~P2P" : "relay";
        [txt appendFormat:(p->speaking
                ? @"\xe2\x97\x8f %s  [%s]  jb=%d/%dms\n"
                : @"\xe2\x97\x8b %s  [%s]  jb=%d/%dms\n"),
            p->name, path, p->jb_ms, p->jb_target_ms];
    }
    if (n == 0)
        [txt appendString:@"\nWaiting for others to join…"];

    _peerView.string = txt;
}

/* ── Window delegate ────────────────────────────────────────────────────── */

- (BOOL)windowShouldClose:(id)sender
{
    (void)sender;
    if (s_running) { vc_quit(); pthread_join(s_thread, NULL); s_running = 0; }
    return YES;
}

@end

/* ── Entry point ────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    /* Handle CLI flags before starting the GUI */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--update") == 0)
            return do_update();
        if (strcmp(argv[i], "--token") == 0 && i + 1 < argc)
            return save_token(argv[i + 1]);
    }

    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        AppDelegate *del = [[AppDelegate alloc] init];
        app.delegate = del;
        [app activateIgnoringOtherApps:YES];
        [app run];
    }
    return 0;
}
