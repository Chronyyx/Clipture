import { Check, ChevronDown, Copy, Link, Pencil } from "lucide-react";
import { useEffect, useState } from "react";
import { FriendAvatar } from "./FriendAvatar";
import { codeGroups } from "./sharingFormat";

function useCopied() {
  const [copied, setCopied] = useState<"link" | "code">();
  useEffect(() => {
    if (!copied) return;
    const timer = window.setTimeout(() => setCopied(undefined), 1800);
    return () => window.clearTimeout(timer);
  }, [copied]);
  const copy = async (what: "link" | "code", text: string) => {
    try {
      await navigator.clipboard.writeText(text);
      setCopied(what);
    } catch {
      setCopied(undefined);
    }
  };
  return { copied, copy };
}

export function FriendCodeCard({
  code,
  inviteLink,
  displayName,
  onRename
}: {
  code?: string | null;
  inviteLink?: string | null;
  displayName: string;
  onRename: (name: string) => Promise<boolean>;
}) {
  const { copied, copy } = useCopied();
  const [editing, setEditing] = useState(false);
  const [showCode, setShowCode] = useState(false);
  const [draft, setDraft] = useState(displayName);

  useEffect(() => { if (!editing) setDraft(displayName); }, [displayName, editing]);

  const commit = async () => {
    const name = draft.trim();
    if (name && name !== displayName) await onRename(name);
    setEditing(false);
  };

  return (
    <section className="share-code-card" aria-labelledby="share-code-title">
      <div className="share-me">
        <FriendAvatar name={displayName} size={46} />
        <span className="share-me-copy">
          <h2 id="share-code-title">Your friends see you as</h2>
          {editing ? (
            <input
              className="share-name-input"
              value={draft}
              maxLength={40}
              autoFocus
              aria-label="Name your friends see"
              onChange={(event) => setDraft(event.target.value)}
              onBlur={() => void commit()}
              onKeyDown={(event) => {
                if (event.key === "Enter") event.currentTarget.blur();
                if (event.key === "Escape") { setDraft(displayName); setEditing(false); }
              }}
            />
          ) : (
            <button className="share-name-button" type="button" onClick={() => setEditing(true)}
              title="Change the name your friends see">
              <span>{displayName}</span> <Pencil size={13} aria-hidden="true" />
            </button>
          )}
        </span>
      </div>
      {code && inviteLink ? (
        <>
          <p className="share-hint">
            Send your link in any chat. When a friend clicks it, Clipture opens and asks them to add you.
          </p>
          <button className="primary share-copy-button" type="button" onClick={() => void copy("link", inviteLink)}>
            {copied === "link" ? <Check size={16} /> : <Link size={16} />}
            {copied === "link" ? "Link copied" : "Copy invite link"}
          </button>
          <button className="share-disclosure" type="button" aria-expanded={showCode}
            onClick={() => setShowCode((value) => !value)}>
            <ChevronDown size={14} aria-hidden="true" /> {showCode ? "Hide friend code" : "Show friend code"}
          </button>
          {showCode && (
            <div className="share-code-reveal">
              <p className="share-code" aria-label={`Friend code ${code}`}>
                {codeGroups(code).map((group, index) => <span key={index}>{group}</span>)}
              </p>
              <button className="secondary-button share-copy-button" type="button" onClick={() => void copy("code", code)}>
                {copied === "code" ? <Check size={16} /> : <Copy size={16} />}
                {copied === "code" ? "Code copied" : "Copy code"}
              </button>
            </div>
          )}
        </>
      ) : (
        <div className="share-code-loading" aria-busy="true" aria-label="Creating your invite link">
          <span className="skeleton skeleton-text" />
          <span className="skeleton share-skeleton-button" style={{ width: "100%" }} />
        </div>
      )}
    </section>
  );
}
