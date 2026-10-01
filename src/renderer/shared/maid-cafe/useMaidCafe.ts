import { useSyncExternalStore } from 'react';

const themeId = 'maid-cafe';

function subscribe(onChange: () => void) {
  const observer = new MutationObserver(onChange);
  observer.observe(document.documentElement, { attributes: true, attributeFilter: ['data-theme'] });
  return () => observer.disconnect();
}

/** Reads the applied theme from the document, so previews and saved settings
 * both count, and no other theme ever mounts a mascot. */
export function useMaidCafe(): boolean {
  return useSyncExternalStore(subscribe, () => document.documentElement.dataset.theme === themeId);
}
