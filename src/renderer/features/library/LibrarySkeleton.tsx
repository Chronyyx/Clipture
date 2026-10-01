import { SpookyBrew } from "../../shared/halloween";
import { CafeNap } from "../../shared/maid-cafe";

// Same layout classes as the clip preview, so real content lands in place.
export function LibrarySkeleton() {
  return (
    <section className="player panel library-player library-player-preview library-skeleton" aria-busy="true" aria-label="Loading clips">
      <div className="library-player-main library-preview-main">
        <span className="thumbnail-skeleton" aria-hidden="true" />
        <CafeNap />
        <SpookyBrew />
      </div>
      <aside className="library-player-sidebar" aria-hidden="true">
        <div className="library-player-side-header">
          <span className="skeleton skeleton-text medium" style={{ height: 22 }} />
          <span className="skeleton skeleton-text short" />
          <span className="skeleton skeleton-text medium" />
        </div>
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
