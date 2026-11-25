import { Clapperboard } from "lucide-react";
import { SpookyBrew, SpookyEmptyArt, useHalloween } from "../../shared/halloween";
import { CafeEmptyArt, CafeNap, useMaidCafe } from "../../shared/maid-cafe";
import { GothEmptyArt, GothLoading, useGothCafe } from "../../shared/goth-cafe";

export function LibraryEmptyState({
  title,
  copy,
  detail,
  actionLabel,
  imported,
  onAction
}: {
  title: string;
  copy: string;
  detail: string;
  actionLabel: string;
  imported: boolean;
  onAction: () => void | Promise<void>;
}) {
  const cafe = useMaidCafe();
  const spooky = useHalloween();
  const goth = useGothCafe();
  return (
    <div className="library-empty-state">
      {cafe ? <CafeEmptyArt imported={imported} /> : spooky ? <SpookyEmptyArt imported={imported} /> : goth ? <GothEmptyArt imported={imported} /> : (
        <div className="library-empty-art" aria-hidden="true">
          <Clapperboard size={74} />
        </div>
      )}
      <h2>{title}</h2>
      <p>{copy}</p>
      <p>{detail}</p>
      <button className="secondary-button library-empty-action" type="button" onClick={() => void onAction()}>
        <Clapperboard size={20} /> {actionLabel}
      </button>
    </div>
  );
}

// Same layout classes as the clip preview, so real content lands in place.
export function LibrarySkeleton() {
  return (
    <section className="player panel library-player library-player-preview library-skeleton" aria-busy="true" aria-label="Loading clips">
      <div className="library-player-main library-preview-main">
        <span className="thumbnail-skeleton" aria-hidden="true" />
        <CafeNap />
        <SpookyBrew />
        <GothLoading />
      </div>
      <div className="library-player-details" aria-hidden="true">
        <div className="library-player-side-header">
          <span className="skeleton skeleton-text medium" style={{ height: 22 }} />
          <span className="skeleton skeleton-text short" />
        </div>
      </div>
      <aside className="library-player-sidebar" aria-hidden="true">
        <div className="clip-rail-heading"><span className="skeleton skeleton-text short" /></div>
        <div className="clip-rail">
          {[0, 1, 2, 3].map((row) => (
            <div className="clip-rail-item" key={row}>
              <span className="clip-rail-open-button">
                <span className="clip-rail-thumbnail"><span className="thumbnail-skeleton" /></span>
                <span className="clip-rail-copy">
                  <span className="skeleton skeleton-text medium" />
                  <span className="skeleton skeleton-text short" />
                </span>
              </span>
            </div>
          ))}
        </div>
      </aside>
    </section>
  );
}
