import { Check, LibraryBig, Play, RotateCcw, Trash2, WifiOff, X } from "lucide-react";
import { useEffect, useState } from "react";
import type { SharedClip, SharingDownload, StreamUrls } from "../../../shared/sharing";
import { clipture } from "../../platform";
import { MediaPlayer } from "../player";
import { formatDuration } from "../../shared/clips/clipMetadata";
import { formatBytes, possessive } from "./sharingFormat";

/** Where the playhead is, as a fraction of the clip. */
function usePlayhead(video: HTMLVideoElement | null) {
  const [playhead, setPlayhead] = useState(0);
  useEffect(() => {
    if (!video) return;
    const update = () => {
      const duration = video.duration;
      if (Number.isFinite(duration) && duration > 0) setPlayhead(video.currentTime / duration);
    };
    const events = ["timeupdate", "durationchange", "loadedmetadata", "seeked"];
    events.forEach((name) => video.addEventListener(name, update));
    update();
    return () => events.forEach((name) => video.removeEventListener(name, update));
  }, [video]);
  return playhead;
}

/** What has arrived from the friend's PC, drawn from the host's own record
 * (not the browser's estimate), so it fills in order from the playhead. */
function WireBar({ clip, video }: { clip: SharedClip; video: HTMLVideoElement | null }) {
  const playhead = usePlayhead(video);
  const size = Math.max(1, clip.size);
  const arrivedBytes = clip.streamed.reduce((total, [start, end]) => total + end - start, 0);
  const arrived = Math.min(100, Math.round((arrivedBytes / size) * 100));
  const friendName = clip.friendName;
  return (
    <div className="share-wire">
      <div className="share-wire-track" role="img" aria-label={`${arrived}% of the clip has arrived from ${possessive(friendName)} PC`}>
        {clip.streamed.map(([start, end]) => (
          <span key={start} className="share-wire-span"
            style={{ left: `${(start / size) * 100}%`, width: `${((end - start) / size) * 100}%` }} />
        ))}
        <span className="share-wire-head" style={{ left: `${playhead * 100}%` }} />
      </div>
      <p>
        <strong>Streaming from {possessive(friendName)} PC.</strong>{" "}
        Nothing is saved on this PC until you add it to your library.
      </p>
    </div>
  );
}

function KeepButton({ clip, download, onSave, onCancel }: {
  clip: SharedClip;
  download?: SharingDownload;
  onSave: () => void;
  onCancel: () => void;
}) {
  if (clip.saved) {
    return <button className="secondary-button share-keep" type="button" disabled><Check size={17} /> In your library</button>;
  }
  if (download?.phase === "running") {
    const percent = download.totalBytes > 0 ? Math.floor((download.receivedBytes / download.totalBytes) * 100) : 0;
    return (
      <>
        <button className="primary share-keep share-keep-progress" type="button" disabled
          style={{ ["--progress" as string]: `${percent}%` }} aria-live="polite">
          <LibraryBig size={17} /> Adding to library {percent}%
        </button>
        {download.reconnecting ? (
          <span className="share-keep-rate" role="status">Reconnecting to {clip.friendName}…</span>
        ) : download.bytesPerSecond > 0 && (
          <span className="share-keep-rate" title={download.relayed
            ? "Your PCs couldn't connect directly, so this goes through a relay and is slower."
            : "Sending directly between your PCs."}>
            {formatBytes(download.bytesPerSecond)}/s{download.relayed ? ", via relay" : ""}
          </span>
        )}
        <button className="secondary-button share-keep-cancel" type="button" onClick={onCancel}
          aria-label={`Cancel adding ${clip.title} to your library`}>
          <X size={16} /> Cancel
        </button>
      </>
    );
  }
  const resumeAt = download?.phase === "failed" && download.totalBytes > 0
    ? Math.floor((download.receivedBytes / download.totalBytes) * 100)
    : 0;
  return (
    <button className="primary share-keep" type="button" onClick={onSave}>
      <LibraryBig size={17} />{" "}
      {resumeAt > 0 ? `Resume from ${resumeAt}%` : download?.phase === "failed" ? "Try adding again" : "Add to library"}
    </button>
  );
}

export function StreamPlayer({
  clip,
  download,
  streamUrl,
  onSave,
  onCancel,
  onDismiss
}: {
  clip: SharedClip;
  download?: SharingDownload;
  streamUrl: (shareId: string) => Promise<StreamUrls>;
  onSave: () => void;
  onCancel: () => void;
  onDismiss: () => void;
}) {
  const [urls, setUrls] = useState<StreamUrls>();
  const [failure, setFailure] = useState<string>();
  const [ready, setReady] = useState(false);
  const [buffering, setBuffering] = useState(false);
  const [attempt, setAttempt] = useState(0);
  const [video, setVideo] = useState<HTMLVideoElement | null>(null);
  // Nothing connects to the friend's PC until the viewer presses play;
  // selecting a clip (or opening Friends) only shows it.
  const [startedFor, setStartedFor] = useState<string>();
  const started = startedFor === clip.shareId;

  useEffect(() => {
    let current = true;
    setUrls(undefined);
    setFailure(undefined);
    setReady(false);
    setBuffering(false);
    if (!started) return;
    streamUrl(clip.shareId)
      .then((next) => { if (current) setUrls(next); })
      .catch((error: unknown) => { if (current) setFailure(error instanceof Error ? error.message : String(error)); });
    return () => { current = false; };
  }, [clip.shareId, streamUrl, attempt, started]);

  const unreachable = `${possessive(clip.friendName)} PC isn't reachable right now. Shared clips stream while their Clipture is running.`;

  useEffect(() => {
    if (!video) return;
    const loaded = () => setReady(true);
    const waiting = () => setBuffering(true);
    const playing = () => setBuffering(false);
    const failed = () => setFailure(unreachable);
    const events: [string, () => void][] = [
      ["loadeddata", loaded], ["waiting", waiting], ["playing", playing], ["canplay", playing], ["error", failed]
    ];
    events.forEach(([name, handler]) => video.addEventListener(name, handler));
    if (video.readyState >= 2) setReady(true);
    return () => events.forEach(([name, handler]) => video.removeEventListener(name, handler));
  }, [video, unreachable]);

  // Stop streaming (and free the host's session) when the viewer moves to
  // another clip or leaves.
  useEffect(() => {
    if (!started) return;
    return () => { void clipture.releasePlaybackCache().catch(() => undefined); };
  }, [started]);

  const connecting = started && urls === undefined && !failure;
  const noMediaHost = urls?.video === "";
  const [aspectWidth, aspectHeight] = (clip.resolution || "").split("x").map((part) => Number.parseInt(part));

  return (
    <section className="share-stage-player" aria-label={`${clip.title} from ${clip.friendName}`}>
      <div className="share-screen">
        {!started ? (
          <div className="share-screen-message share-screen-idle">
            <button className="share-play" type="button" onClick={() => setStartedFor(clip.shareId)}
              aria-label={`Play ${clip.title} from ${possessive(clip.friendName)} PC`}>
              <Play size={26} aria-hidden="true" />
            </button>
            <p>Plays from {possessive(clip.friendName)} PC when you press play.</p>
          </div>
        ) : failure ? (
          <div className="share-screen-message">
            <WifiOff size={30} aria-hidden="true" />
            <p>{failure === "Your friend is offline or unreachable right now." ? unreachable : failure}</p>
            <button className="secondary-button" type="button" onClick={() => setAttempt((value) => value + 1)}>
              <RotateCcw size={16} /> Try again
            </button>
          </div>
        ) : noMediaHost ? (
          <div className="share-screen-message">
            <p>Streams play in the Clipture desktop app.</p>
          </div>
        ) : (
          <>
            {urls?.video && (
              <MediaPlayer
                sourceUrl={urls.video}
                audioChunkUrl={urls.audio}
                mixedAudio={clip.allAudioReady && clip.audioTracks.length > 1}
                durationHint={clip.durationSeconds}
                aspectWidth={aspectWidth || 16}
                aspectHeight={aspectHeight || 9}
                onVideo={setVideo}
              />
            )}
            {(connecting || !ready) && (
              <div className="share-screen-connecting thumbnail-skeleton" aria-busy="true">
                <span>Connecting to {possessive(clip.friendName)} PC</span>
              </div>
            )}
            {ready && buffering && <span className="share-buffering" role="status">Waiting for {clip.friendName}</span>}
          </>
        )}
      </div>
      <div className="share-stage-bar">
        <div className="share-stage-title">
          <h2>{clip.title}</h2>
          <p>
            From {clip.friendName}, {clip.gameOrApp || "a clip"}, {formatDuration(clip.durationSeconds)}, {formatBytes(clip.size)}
          </p>
        </div>
        <div className="share-stage-actions">
          <KeepButton clip={clip} download={download} onSave={onSave} onCancel={onCancel} />
          <button className="icon-button share-dismiss" type="button" onClick={onDismiss}
            title="Remove from this list" aria-label={`Remove ${clip.title} from this list`}>
            <Trash2 size={17} />
          </button>
        </div>
      </div>
      {download?.phase === "failed" && download.message && (
        <p className="share-download-error" role="alert">{download.message}</p>
      )}
      {started && !failure && !noMediaHost && <WireBar clip={clip} video={video} />}
    </section>
  );
}
