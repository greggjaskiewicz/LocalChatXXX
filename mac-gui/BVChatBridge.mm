//
//  BVChatBridge.mm  --  Objective-C++ implementation wrapping BVChatCore.
//
#import "BVChatBridge.h"

#include <memory>
#include <string>
#include <fcntl.h>    // open
#include <unistd.h>   // isatty, dup2, close
#include <util.h>     // openpty
#include "BVChatCore.hpp"

// We reuse the console client's logic, which renders its TUI to stdout
// (ClearScreen / PrintAll / RenderChat). In a windowed app that's just noise.
// Redirect file descriptor 1 itself to /dev/null (not just the stdout FILE*),
// so std::cout — whose streambuf writes straight to fd 1 — is silenced too.
// stderr is left intact for genuine errors; spdlog logs to ~/localchat_gui.log.
static void BVSilenceConsoleStdout(void)
{
    const int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0)
    {
        dup2(devnull, STDOUT_FILENO);
        if (devnull != STDOUT_FILENO)
        {
            close(devnull);
        }
    }
}

// The console-client base constructs a BVTerminal whose ctor throws unless stdin
// is a real terminal. A windowed app has no tty, so hand the process a harmless
// pseudo-tty on stdin before the core is built. The pty is never read (the GUI
// client overrides Run and does no stdin I/O); this just satisfies isatty().
static void BVEnsureStdinIsTTY(void)
{
    if (isatty(STDIN_FILENO))
    {
        return;
    }
    int master = -1, slave = -1;
    if (openpty(&master, &slave, NULL, NULL, NULL) == 0)
    {
        dup2(slave, STDIN_FILENO);
        // Leave master/slave open for the process lifetime; closing slave after
        // dup2 is fine too, but keeping them avoids any EOF surprises.
    }
}

static NSString *NS(const std::string &s)
{
    NSString *r = [NSString stringWithUTF8String:s.c_str()];
    return r ? r : @"";
}

@implementation BVMessageItem
@end

@implementation BVReceivedFileItem
@end

@implementation BVChatBridge
{
    std::unique_ptr<BVChatCore> _core;
}

- (instancetype)init
{
    if (self = [super init])
    {
        _core = std::make_unique<BVChatCore>();
    }
    return self;
}

- (void)dealloc
{
    if (_core)
    {
        _core->stop();
    }
}

- (BOOL)start
{
    BVEnsureStdinIsTTY();
    BVSilenceConsoleStdout();
    __weak BVChatBridge *weakSelf = self;
    const bool ok = _core->start([weakSelf](BVGuiEvent e)
    {
        // Invoked on the io / mailbox thread. Hop to the main thread before
        // touching the delegate (and therefore SwiftUI state).
        dispatch_async(dispatch_get_main_queue(), ^{
            BVChatBridge *strongSelf = weakSelf;
            id<BVChatBridgeDelegate> d = strongSelf.delegate;
            if (!d)
            {
                return;
            }
            switch (e)
            {
                case BVGuiEvent::ServicesChanged:
                case BVGuiEvent::SessionsChanged:
                    if ([d respondsToSelector:@selector(chatBridgePeersChanged)])
                        [d chatBridgePeersChanged];
                    break;
                case BVGuiEvent::MessagesChanged:
                    if ([d respondsToSelector:@selector(chatBridgeMessagesChanged)])
                        [d chatBridgeMessagesChanged];
                    break;
                case BVGuiEvent::FileProgress:
                    if ([d respondsToSelector:@selector(chatBridgeFileProgress)])
                        [d chatBridgeFileProgress];
                    break;
                case BVGuiEvent::FileReceived:
                    if ([d respondsToSelector:@selector(chatBridgeFileReceived)])
                        [d chatBridgeFileReceived];
                    break;
            }
        });
    });
    return ok ? YES : NO;
}

- (void)stop
{
    if (_core)
    {
        _core->stop();
    }
}

- (NSArray<NSString *> *)connectedPeers
{
    NSMutableArray<NSString *> *out = [NSMutableArray array];
    for (const auto &s : _core->sessions())
    {
        [out addObject:NS(s)];
    }
    return out;
}

- (NSArray<NSString *> *)discoveredPeers
{
    NSMutableArray<NSString *> *out = [NSMutableArray array];
    for (const auto &bi : _core->services())
    {
        [out addObject:NS(bi.serviceName)];
    }
    return out;
}

- (NSArray<BVMessageItem *> *)messagesWith:(NSString *)peer
{
    NSMutableArray<BVMessageItem *> *out = [NSMutableArray array];
    const std::string p = peer.UTF8String ? peer.UTF8String : "";
    for (const auto &m : _core->messages(p))
    {
        BVMessageItem *item = [BVMessageItem new];
        item.text      = NS(m.textData);
        item.sender    = NS(m.sender);
        item.timestamp = m.timestamp;
        [out addObject:item];
    }
    return out;
}

- (BOOL)sendText:(NSString *)text to:(NSString *)peer
{
    if (!text || !peer)
    {
        return NO;
    }
    return _core->sendText(peer.UTF8String, text.UTF8String) ? YES : NO;
}

- (BOOL)sendFile:(NSString *)filePath to:(NSString *)peer
{
    if (!filePath || !peer)
    {
        return NO;
    }
    return _core->sendFile(peer.UTF8String, filePath.UTF8String) ? YES : NO;
}

- (NSArray<BVReceivedFileItem *> *)takeReceivedFiles
{
    NSMutableArray<BVReceivedFileItem *> *out = [NSMutableArray array];
    for (const auto &f : _core->takeReceivedFiles())
    {
        BVReceivedFileItem *item = [BVReceivedFileItem new];
        item.fileName = NS(f.fileName);
        item.sender   = NS(f.serviceName);
        item.path     = NS(f.path);
        [out addObject:item];
    }
    return out;
}

- (BOOL)discardFileAtPath:(NSString *)path
{
    if (!path)
    {
        return NO;
    }
    NSError *err = nil;
    return [[NSFileManager defaultManager] removeItemAtPath:path error:&err];
}

- (NSString *)thisHostname
{
    return NS(_core->thisHostname());
}

@end
