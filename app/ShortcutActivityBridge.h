//
//  ShortcutActivityBridge.h
//  iSH-AOK
//
//  Swift-visible entry point for the App Shortcuts "open destination" intent.
//

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

// Applies an app.ish.scene.* destination activity to an already-connected
// scene's window and returns YES, or returns NO when no scene has connected
// yet (cold start) so the caller can fall back to
// requestSceneSessionActivation. Needed because UIKit's
// requestSceneSessionActivation(nil, userActivity:...) only delivers the
// activity when it CREATES a scene (iPad); on a single-scene device it just
// foregrounds the existing scene and drops the activity. Main thread only.
BOOL ISHApplyShortcutActivityToConnectedScene(NSUserActivity *activity);

NS_ASSUME_NONNULL_END
