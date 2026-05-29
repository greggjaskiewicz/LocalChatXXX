//
//  BVChatBridge.h  --  Swift-facing Objective-C interface over the C++ core.
//
//  This is the only surface SwiftUI talks to. It exposes plain Foundation
//  types (NSString, NSArray, small model objects) and a delegate for change
//  notifications, hiding all of the C++ / STL / threading behind it.
//
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

/// One chat message as seen by the UI.
@interface BVMessageItem : NSObject
@property (nonatomic, copy)   NSString *text;
@property (nonatomic, copy)   NSString *sender;
@property (nonatomic, assign) uint64_t  timestamp;
@end

/// An incoming file offer, surfaced as the transfer begins (Accept/Reject).
@interface BVFileOfferItem : NSObject
@property (nonatomic, assign) uint32_t correlationKey;
@property (nonatomic, copy)   NSString *fileName;
@property (nonatomic, copy)   NSString *sender;
@property (nonatomic, assign) uint64_t  size;   // bytes
@end

/// Coarse change notifications. The UI re-pulls state in response. All calls
/// are delivered on the main thread.
@protocol BVChatBridgeDelegate <NSObject>
@optional
- (void)chatBridgePeersChanged;      ///< discovered/connected peers changed
- (void)chatBridgeMessagesChanged;   ///< a message was received or sent
- (void)chatBridgeFileProgress;      ///< a file transfer made progress
- (void)chatBridgeFileOffered;       ///< a file offer arrived (drain via -takeFileOffers)
@end

@interface BVChatBridge : NSObject

@property (nonatomic, weak, nullable) id<BVChatBridgeDelegate> delegate;

/// Registers the Bonjour service and starts discovery + networking.
/// Returns NO if setup failed (e.g. mDNS registration).
- (BOOL)start;
- (void)stop;

/// Peers we have a live session with (ready to chat / send files).
- (NSArray<NSString *> *)connectedPeers;
/// Peers discovered on the network (may still be connecting).
- (NSArray<NSString *> *)discoveredPeers;

- (NSArray<BVMessageItem *> *)messagesWith:(NSString *)peer;

- (BOOL)sendText:(NSString *)text to:(NSString *)peer;
- (BOOL)sendFile:(NSString *)filePath to:(NSString *)peer;

/// File offers that arrived since the last call (each awaiting Accept/Reject).
- (NSArray<BVFileOfferItem *> *)takeFileOffers;
/// Accept an offered file (it is kept). Reject deletes it (now or on arrival).
- (void)acceptFile:(uint32_t)correlationKey;
- (void)rejectFile:(uint32_t)correlationKey;

/// Latest file-transfer status line, e.g. "Receiving x.png… 45%" / "Sent y.png".
- (NSString *)transferStatus;

- (NSString *)thisHostname;

@end

NS_ASSUME_NONNULL_END
