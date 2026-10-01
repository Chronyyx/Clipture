import { Clapperboard } from "lucide-react";
import { SpookyEmptyArt, useHalloween } from "../../shared/halloween";
import { CafeEmptyArt, useMaidCafe } from "../../shared/maid-cafe";

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
  return (
    <div className="library-empty-state">
      {cafe ? <CafeEmptyArt imported={imported} /> : spooky ? <SpookyEmptyArt imported={imported} /> : (
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
