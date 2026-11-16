#pragma once

namespace sle::server {

// Main-thread lifecycle only. Installs the ordinary LL 26.60.7 $onRequest hook
// and pins this Windows DLL before handing executable callbacks to the game.
// A successful install is process-lifetime: disable is supported; unload is not.
[[nodiscard]] bool installJoinInfoExtension();

// Stops publication of visuals, but retains a schema-1 enabled:false extension
// so clients discard stale visuals. The hook itself intentionally stays live.
void uninstallJoinInfoExtension();

// Build on the main thread after load/reload. HTTP threads only read immutable
// atomic shared_ptr snapshots, never the mutable ConfigManager.
void refreshJoinInfoSnapshot();
void setJoinInfoEnabled(bool enabled);
[[nodiscard]] bool isJoinInfoEnabled();

} // namespace sle::server
