import { FolderOpen, Play, Send, Trash2 } from "lucide-react";
import { useEffect, useLayoutEffect, useRef, useState, type KeyboardEvent, type MouseEvent, type ReactNode } from "react";
import { createPortal } from "react-dom";
import type { ClipRecord } from "../../../shared/types";

export interface ClipMenuRequest {
  clip: ClipRecord;
  x: number;
  y: number;
}

interface MenuItem {
  label: string;
  icon: ReactNode;
  danger?: boolean;
  run: () => void;
}

/** A clip's right-click menu, at the pointer (or the clip, from the keyboard).
 * Closes on Escape, a click elsewhere, scrolling or leaving the window. */
export function ClipContextMenu({
  request,
  onClose,
  onPlay,
  onReveal,
  onShare,
  onDelete
}: {
  request: ClipMenuRequest;
  onClose: () => void;
  onPlay: (clip: ClipRecord) => void;
  onReveal: (clip: ClipRecord) => void;
  onShare?: (clip: ClipRecord) => void;
  onDelete: (clip: ClipRecord) => void;
}) {
  const menu = useRef<HTMLDivElement>(null);
  const close = useRef(onClose);
  close.current = onClose;
  const [position, setPosition] = useState({ left: request.x, top: request.y });
  const { clip } = request;
  const items: MenuItem[] = [
    { label: "Play", icon: <Play size={16} />, run: () => onPlay(clip) },
    { label: "Show in folder", icon: <FolderOpen size={16} />, run: () => onReveal(clip) },
    ...(onShare ? [{ label: "Send to a friend", icon: <Send size={16} />, run: () => onShare(clip) }] : []),
    { label: "Delete…", icon: <Trash2 size={16} />, danger: true, run: () => onDelete(clip) }
  ];

  // Keep the whole menu on screen: open up or left of the pointer near an edge.
  useLayoutEffect(() => {
    const element = menu.current;
    if (!element) return;
    const { width, height } = element.getBoundingClientRect();
    const margin = 8;
    setPosition({
      left: Math.max(margin, Math.min(request.x, window.innerWidth - width - margin)),
      top: request.y + height + margin > window.innerHeight ? Math.max(margin, request.y - height) : request.y
    });
    element.querySelector<HTMLButtonElement>("button")?.focus();
  }, [request.x, request.y]);

  useEffect(() => {
    const previous = document.activeElement;
    const closeOutside = (event: PointerEvent) => {
      if (!menu.current?.contains(event.target as Node)) close.current();
    };
    const dismiss = () => close.current();
    window.addEventListener("pointerdown", closeOutside, true);
    window.addEventListener("scroll", dismiss, true);
    window.addEventListener("resize", dismiss);
    window.addEventListener("blur", dismiss);
    return () => {
      window.removeEventListener("pointerdown", closeOutside, true);
      window.removeEventListener("scroll", dismiss, true);
      window.removeEventListener("resize", dismiss);
      window.removeEventListener("blur", dismiss);
      if (previous instanceof HTMLElement && previous.isConnected) previous.focus();
    };
  }, []);

  const onKeyDown = (event: KeyboardEvent) => {
    const buttons = [...(menu.current?.querySelectorAll<HTMLButtonElement>("button") ?? [])];
    const index = buttons.indexOf(document.activeElement as HTMLButtonElement);
    if (event.key === "Escape" || event.key === "Tab") {
      event.preventDefault();
      onClose();
    } else if (event.key === "ArrowDown" || event.key === "ArrowUp") {
      event.preventDefault();
      const step = event.key === "ArrowDown" ? 1 : -1;
      buttons[(index + step + buttons.length) % buttons.length]?.focus();
    } else if (event.key === "Home" || event.key === "End") {
      event.preventDefault();
      buttons[event.key === "Home" ? 0 : buttons.length - 1]?.focus();
    }
  };

  return createPortal(
    <div
      ref={menu}
      className="clip-context-menu"
      role="menu"
      aria-label={`Actions for ${clip.title}`}
      style={position}
      onKeyDown={onKeyDown}
      onContextMenu={(event) => event.preventDefault()}
    >
      {items.map((item) => (
        <button
          key={item.label}
          type="button"
          role="menuitem"
          className={item.danger ? "danger" : undefined}
          onClick={() => {
            onClose();
            item.run();
          }}
        >
          {item.icon}
          <span>{item.label}</span>
        </button>
      ))}
    </div>,
    document.body
  );
}

/** Where a menu opened from the keyboard (menu key, Shift+F10) appears. */
export function menuPointFor(event: MouseEvent<HTMLElement>): { x: number; y: number } {
  if (event.clientX !== 0 || event.clientY !== 0) return { x: event.clientX, y: event.clientY };
  const box = event.currentTarget.getBoundingClientRect();
  return { x: box.left + 16, y: box.top + 16 };
}
