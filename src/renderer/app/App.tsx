import { Activity, Download, Library, Save, SlidersHorizontal, Users, type LucideIcon } from "lucide-react";
import { AnimatePresence, MotionConfig, motion } from "motion/react";
import { useCallback, useEffect, useState } from "react";
// @ts-ignore
import logoUrl from "../../../assets/clipture-logo-ui.png";
import type { ClipRecord, ClipSettings } from "../../shared/types";
import { useCaptureActions } from "../features/capture";
import { DiagnosticsView, RecorderStatus, useDiagnostics } from "../features/diagnostics";
import { LibraryView, useClipLibrary } from "../features/library";
import { SettingsView, useClipPreferences } from "../features/settings";
import { FriendsSidebar, FriendsView, InviteDialog, ShareClipDialog, useSharing } from "../features/sharing";
import { TitlebarUpdateControls, useUpdates } from "../features/updates";
import { HalloweenAmbience, SpookyNoticePet, SpookySaveCheer } from "../shared/halloween";
import { CafeNoticePet, CafeSaveCheer } from "../shared/maid-cafe";

type Tab = "library" | "friends" | "settings" | "diagnostics";
type AppNotice = { message: string; tab?: Tab; durationMs: number };

const tabs: Array<{ id: Tab; label: string; Icon: LucideIcon }> = [
  { id: "library", label: "Library", Icon: Library },
  { id: "friends", label: "Friends", Icon: Users },
  { id: "settings", label: "Settings", Icon: SlidersHorizontal },
  { id: "diagnostics", label: "Diagnostics", Icon: Activity }
];

// Short, interruptible responses to navigation; nothing animates on its own.
const viewTransition = { duration: 0.16, ease: "easeOut" } as const;

export function App({ initialSettings }: { initialSettings?: ClipSettings }) {
  const [activeTab, setActiveTab] = useState<Tab>("library");
  const [query, setQuery] = useState("");
  const [notice, setNotice] = useState<AppNotice>();
  const [selectedClip, setSelectedClip] = useState<ClipRecord>();
  const [clipToShare, setClipToShare] = useState<ClipRecord>();
  const [focusFriendId, setFocusFriendId] = useState<string>();
  const libraryNotice = useCallback((message: string, durationMs = 4000) =>
    setNotice({ message, durationMs, tab: "library" }), []);
  const settingsNotice = useCallback((message: string, durationMs = 4000) =>
    setNotice({ message, durationMs, tab: "settings" }), []);
  const diagnosticsNotice = useCallback((message: string, durationMs = 4000) =>
    setNotice({ message, durationMs, tab: "diagnostics" }), []);
  const saveNotice = useCallback((message: string, durationMs = 4000) =>
    setNotice({ message, durationMs, tab: activeTab }), [activeTab]);
  const globalNotice = useCallback((message: string, durationMs = 4000) =>
    setNotice({ message, durationMs }), []);
  const { clips, loaded: libraryLoaded, addClip, importVideos: importFolders } = useClipLibrary(libraryNotice);
  const { settings, clipSounds, updateSettings, previewClipSound, importClipSound,
    revealSounds, refreshPreferences } = useClipPreferences(settingsNotice, initialSettings);
  const { diagnostics, diagnosticsError, hasDiagnostics, exportDiagnostics, isExportingDiagnostics } = useDiagnostics(diagnosticsNotice);
  const { saveClip, isSavingClip, saveIoAnalyzer, toggleSaveIoAnalyzer } =
    useCaptureActions(settings, addClip, saveNotice, diagnosticsNotice);
  const sharing = useSharing(globalNotice);
  const friendRequests = sharing.snapshot?.friends.filter((friend) => friend.status === "incoming").length ?? 0;
  const openFriends = (friendId?: string) => {
    setFocusFriendId(friendId);
    setActiveTab("friends");
  };
  const { updateState, checkForUpdatesNow, downloadUpdate, installUpdate } = useUpdates(globalNotice);
  const updateControls = <TitlebarUpdateControls
    updateState={updateState}
    onCheck={() => void checkForUpdatesNow()}
    onDownload={() => void downloadUpdate()}
    onInstall={installUpdate}
  />;

  useEffect(() => {
    if (!notice) return;
    const timer = window.setTimeout(() => setNotice(undefined), notice.durationMs);
    return () => window.clearTimeout(timer);
  }, [notice]);

  async function importVideos() {
    if (await importFolders()) {
      try { await refreshPreferences(); }
      catch (error) { globalNotice(error instanceof Error ? error.message : "Could not refresh preferences.", 6000); }
    }
  }

  return (
    <MotionConfig reducedMotion="user">
    <div className="app-shell">
      <div className="titlebar-drag-region" aria-hidden="true" />
      <HalloweenAmbience />
      <aside className="sidebar">
        <div className="brand">
          <img src={logoUrl} alt="Clipture" className="mark" />
          <div>
            <strong>Clipture</strong>
          </div>
        </div>
        <nav className="nav-list" aria-label="Main">
          {tabs.map(({ id, label, Icon }) => (
            <button key={id} className={activeTab === id ? "nav active" : "nav"} aria-current={activeTab === id ? "page" : undefined}
              onClick={() => (id === "friends" ? openFriends() : setActiveTab(id))}>
              {activeTab === id && (
                <motion.span layoutId="nav-active" className="nav-indicator"
                  transition={{ type: "spring", bounce: 0, visualDuration: 0.25 }} />
              )}
              <Icon size={18} aria-hidden="true" /> {label}
              {id === "friends" && friendRequests > 0 && (
                <span className="nav-badge" aria-label={`${friendRequests} friend ${friendRequests === 1 ? "request" : "requests"}`}>{friendRequests}</span>
              )}
            </button>
          ))}
        </nav>
        <RecorderStatus
          diagnostics={diagnostics}
          hasDiagnostics={hasDiagnostics}
          diagnosticsError={diagnosticsError}
          clipLengthSeconds={settings?.clipLengthSeconds ?? 30}
        />
        <FriendsSidebar controller={sharing} onOpenFriend={openFriends} onOpenFriends={() => openFriends()} />
      </aside>

      <main className="workspace">
        {activeTab !== "library" && activeTab !== "friends" && (
          <header className="topbar">
            <div>
              <h1>{activeTab === "settings" ? "Settings" : "Diagnostics"}</h1>
              {activeTab === "diagnostics" && <p>{diagnostics.status}</p>}
            </div>
            <div className="topbar-actions">
              <div className="save-actions">
                {updateControls}
                <button className="primary" onClick={saveClip} disabled={isSavingClip}>
                  <Save size={18} /> {isSavingClip ? "Saving..." : `Save last ${settings?.clipLengthSeconds ?? 30}s`}
                  <CafeSaveCheer saving={isSavingClip} />
                  <SpookySaveCheer saving={isSavingClip} />
                </button>
              </div>
              {activeTab === "diagnostics" && (
                <button
                  className="secondary-button"
                  onClick={() => void exportDiagnostics()}
                  disabled={isExportingDiagnostics}
                >
                  <Download size={18} /> {isExportingDiagnostics ? "Exporting..." : "Export diagnostics"}
                </button>
              )}
              {activeTab === "diagnostics" && saveIoAnalyzer.available && (
                <button
                  className="secondary-button"
                  onClick={() => void toggleSaveIoAnalyzer()}
                  disabled={isSavingClip}
                >
                  <Activity size={18} /> {saveIoAnalyzer.armed ? "Cancel I/O trace" : "Analyze next save"}
                </button>
              )}
            </div>
          </header>
        )}

        <AnimatePresence>
          {notice && (!notice.tab || notice.tab === activeTab) && (
            <motion.div key={notice.message} className="notice" role="status"
              initial={{ opacity: 0, transform: "translateY(-6px)" }}
              animate={{ opacity: 1, transform: "translateY(0px)" }}
              exit={{ opacity: 0, transform: "translateY(-6px)" }}
              transition={viewTransition}>
              <CafeNoticePet />
              <SpookyNoticePet />
              {notice.message}
            </motion.div>
          )}
        </AnimatePresence>
        <AnimatePresence mode="wait" initial={false}>
        <motion.div key={activeTab} className="view"
          initial={{ opacity: 0, transform: "translateY(4px)" }}
          // A lingering transform would contain position:fixed dialogs.
          animate={{ opacity: 1, transform: "translateY(0px)", transitionEnd: { transform: "none" } }}
          exit={{ opacity: 0 }}
          transition={viewTransition}>
        {activeTab === "library" && (
          <LibraryView
            headerControls={updateControls}
            clips={clips}
            loading={!libraryLoaded}
            query={query}
            setQuery={setQuery}
            selectedClip={selectedClip}
            setSelectedClip={setSelectedClip}
            settings={settings}
            onSaveClip={saveClip}
            onImportVideos={importVideos}
            isSavingClip={isSavingClip}
            clipLengthSeconds={settings?.clipLengthSeconds ?? 30}
            onShareClip={setClipToShare}
          />
        )}
        {activeTab === "friends" && (
          <FriendsView controller={sharing} headerControls={updateControls}
            focusFriendId={focusFriendId} onClearFocus={() => setFocusFriendId(undefined)} />
        )}
        {activeTab === "settings" && settings && (
          <SettingsView
            settings={settings}
            clipSounds={clipSounds}
            onChange={updateSettings}
            onPreviewSound={previewClipSound}
            onImportSound={importClipSound}
            onRevealSounds={revealSounds}
          />
        )}
        {activeTab === "diagnostics" && <>
          {diagnosticsError && <div className="notice" role="status">Diagnostics refresh delayed. {hasDiagnostics ? "Values below are the last received snapshot, not live readings." : "No snapshot received yet."} {diagnosticsError}</div>}
          <DiagnosticsView diagnostics={diagnostics} />
        </>}
        </motion.div>
        </AnimatePresence>
      </main>
      {sharing.snapshot?.pendingInvite && (
        <InviteDialog
          key={sharing.snapshot.pendingInvite.code}
          invite={sharing.snapshot.pendingInvite}
          sharingEnabled={sharing.snapshot.enabled}
          onAccept={() => sharing.acceptInvite(sharing.snapshot?.pendingInvite?.name ?? "")}
          onDismiss={() => void sharing.dismissInvite()}
        />
      )}
      {clipToShare && (
        <ShareClipDialog
          clip={clipToShare}
          snapshot={sharing.snapshot}
          onShare={(friendId, friendName) => sharing.shareClip(friendId, clipToShare.filePath, friendName)}
          onClose={() => setClipToShare(undefined)}
          onOpenFriends={() => openFriends()}
        />
      )}
    </div>
    </MotionConfig>
  );
}
