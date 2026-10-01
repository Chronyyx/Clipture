// Shaped like the loaded Friends tab so content lands in place.
export function SharingSkeleton() {
  return (
    <div className="share-layout" aria-busy="true" aria-label="Loading friends">
      <section className="share-stage" aria-hidden="true">
        <div className="share-stage-main">
          <div className="share-stage-player">
            <span className="share-screen thumbnail-skeleton" />
            <div className="share-stage-bar">
              <div className="share-stage-title">
                <span className="skeleton skeleton-text medium" style={{ height: 20 }} />
                <span className="skeleton skeleton-text short" />
              </div>
              <span className="skeleton share-skeleton-button" />
            </div>
            <span className="skeleton share-skeleton-wire" />
          </div>
        </div>
        <aside className="share-rail">
          <div className="share-rail-tabs"><span className="skeleton skeleton-text medium" /></div>
          <ul className="share-rail-list">
            {[0, 1, 2, 3].map((row) => (
              <li key={row}>
                <span className="share-rail-item">
                  <span className="share-tile thumbnail-skeleton" />
                  <span className="share-rail-copy">
                    <span className="skeleton skeleton-text medium" />
                    <span className="skeleton skeleton-text short" />
                  </span>
                </span>
              </li>
            ))}
          </ul>
        </aside>
      </section>
      <div className="share-people" aria-hidden="true">
        <section className="share-code-card">
          <span className="skeleton skeleton-text medium" />
          <div className="share-code-loading">
            <span className="skeleton skeleton-text" />
            <span className="skeleton share-skeleton-button" style={{ width: "100%" }} />
          </div>
        </section>
        <section className="share-friends">
          {[0, 1, 2].map((row) => (
            <div className="share-friend-row" key={row}>
              <span className="share-avatar skeleton" />
              <span className="share-friend-copy">
                <span className="skeleton skeleton-text medium" />
                <span className="skeleton skeleton-text short" />
              </span>
            </div>
          ))}
        </section>
      </div>
    </div>
  );
}
