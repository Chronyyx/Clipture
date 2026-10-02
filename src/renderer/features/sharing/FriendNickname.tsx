import { useState } from "react";
import type { Friend } from "../../../shared/sharing";

/** In-place nickname editing. The nickname is yours alone: it replaces the
 * name they chose everywhere in your Clipture and is never sent to them.
 * Their own name is not shown here, so a name you renamed stays out of view. */
export function NicknameField({ friend, onSave, onDone }: {
  friend: Friend;
  onSave: (nickname: string) => Promise<boolean>;
  onDone: () => void;
}) {
  const [draft, setDraft] = useState(friend.name);
  const [busy, setBusy] = useState(false);

  const save = async (nickname: string) => {
    if (busy) return;
    // Unchanged: nothing to save (and no nickname that merely copies theirs).
    if (nickname.trim() === friend.name) return onDone();
    setBusy(true);
    await onSave(nickname.trim());
    setBusy(false);
    onDone();
  };

  return (
    <span className="share-nickname-edit">
      <input
        className="share-name-input"
        value={draft}
        maxLength={40}
        autoFocus
        disabled={busy}
        aria-label={`Nickname for ${friend.name}`}
        placeholder="Nickname"
        onFocus={(event) => event.currentTarget.select()}
        onChange={(event) => setDraft(event.target.value)}
        onBlur={() => void save(draft)}
        onKeyDown={(event) => {
          if (event.key === "Enter") event.currentTarget.blur();
          if (event.key === "Escape") { event.preventDefault(); onDone(); }
        }}
      />
      {friend.nickname !== null && (
        // Mouse down, not click: the input's blur would save first.
        <button className="share-nickname-reset" type="button" disabled={busy}
          onMouseDown={(event) => { event.preventDefault(); void save(""); }}>
          Use their name
        </button>
      )}
    </span>
  );
}

export function useNicknameEditing() {
  const [editing, setEditing] = useState<string>();
  return { editing, start: setEditing, stop: () => setEditing(undefined) };
}
