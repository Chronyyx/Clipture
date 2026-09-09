import { Activity, Download, Library, Save, SlidersHorizontal } from "lucide-react";
import { useCallback, useEffect, useState } from "react";
// @ts-ignore
import logoUrl from "../../../assets/clipture-logo-ui.png";
import type { ClipRecord, ClipSettings } from "../../shared/types";
import { useCaptureActions } from "../features/capture";
import { DiagnosticsView, useDiagnostics } from "../features/diagnostics";
import { LibraryView, useClipLibrary } from "../features/library";
import { SettingsView, useClipPreferences } from "../features/settings";
import { TitlebarUpdateControls, useUpdates } from "../features/updates";

type Tab = "library" | "settings" | "diagnostics";
type AppNotice = { message: string; tab?: Tab; durationMs: number };

export function App({ initialSettings }: { initialSettings?: ClipSettings }) {
  const [activeTab, setActiveTab] = useState<Tab>("library");
  const [query, setQuery] = useState("");
  const [notice, setNotice] = useState<AppNotice>();
  const [selectedClip, setSelectedClip] = useState<ClipRecord>();
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
  const { clips, addClip, importVideos: importFolders } = useClipLibrary(libraryNotice);
  const { settings, clipSounds, updateSettings, previewClipSound, importClipSound,
    revealSounds, refreshPreferences } = useClipPreferences(settingsNotice, initialSettings);
  const { diagnostics, diagnosticsError, hasDiagnostics, exportDiagnostics, isExportingDiagnostics } = useDiagnostics(diagnosticsNotice);
  const { saveClip, isSavingClip, saveIoAnalyzer, toggleSaveIoAnalyzer } =
    useCaptureActions(settings, addClip, saveNotice, diagnosticsNotice);
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
    <div className="app-shell">
      <div className="titlebar-drag-region" aria-hidden="true" />
      <aside className="sidebar">
        <div className="brand">
          <img src={logoUrl} alt="Clipture" className="mark" />
          <div>
            <strong>Clipture</strong>
          </div>
        </div>
        <button className={activeTab === "library" ? "nav active" : "nav"} onClick={() => setActiveTab("library")}>
          <Library size={18} /> Library
        </button>
        <button className={activeTab === "settings" ? "nav active" : "nav"} onClick={() => setActiveTab("settings")}>
          <SlidersHorizontal size={18} /> Settings
        </button>
        <button className={activeTab === "diagnostics" ? "nav active" : "nav"} onClick={() => setActiveTab("diagnostics")}>
          <Activity size={18} /> Diagnostics
        </button>
        <div className={`encoder${hasDiagnostics && diagnostics.degraded ? " degraded" : ""}${diagnosticsError ? " delayed" : ""}`} title={diagnosticsError}>
          <span>Encoder</span>
          <strong>{hasDiagnostics ? diagnostics.activeEncoder : diagnosticsError ? "Waiting for engine" : "Connecting…"}</strong>
          {hasDiagnostics && <><small>{diagnostics.encoderMode}</small><small>{diagnostics.gpu}</small></>}
          {diagnosticsError && <small className="encoder-refresh-status">{hasDiagnostics ? "Status delayed · last known details" : "Waiting for diagnostics"}</small>}
        </div>
      </aside>

      <main className="workspace">
        {activeTab !== "library" && (
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

        {notice && (!notice.tab || notice.tab === activeTab) && (
          <div className="notice" role="status">{notice.message}</div>
        )}
        {activeTab === "library" && (
          <LibraryView
            headerControls={updateControls}
            clips={clips}
            query={query}
            setQuery={setQuery}
            selectedClip={selectedClip}
            setSelectedClip={setSelectedClip}
            settings={settings}
            onSaveClip={saveClip}
            onImportVideos={importVideos}
            isSavingClip={isSavingClip}
            clipLengthSeconds={settings?.clipLengthSeconds ?? 30}
          />
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
      </main>
    </div>
  );
}
