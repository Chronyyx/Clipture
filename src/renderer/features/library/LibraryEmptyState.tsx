import { Clapperboard } from "lucide-react";

export function LibraryEmptyState({
  title,
  copy,
  detail,
  actionLabel,
  onAction
}: {
  title: string;
  copy: string;
  detail: string;
  actionLabel: string;
  onAction: () => void | Promise<void>;
}) {
  return (
    <div className="library-empty-state">
      <div className="library-empty-art" aria-hidden="true">
        <Clapperboard size={74} />
      </div>
      <h2>{title}</h2>
      <p>{copy}</p>
      <p>{detail}</p>
      <button className="secondary-button library-empty-action" type="button" onClick={() => void onAction()}>
        <Clapperboard size={20} /> {actionLabel}
      </button>
    </div>
  );
}
