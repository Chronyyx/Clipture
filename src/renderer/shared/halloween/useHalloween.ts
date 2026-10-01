import { useSyncExternalStore } from 'react';

function subscribe(onChange: () => void) {
  const observer = new MutationObserver(onChange);
  observer.observe(document.documentElement, { attributes: true, attributeFilter: ['data-theme'] });
  return () => observer.disconnect();
}

/** True while the Halloween theme is applied (saved or previewed); nothing
 * spooky mounts under any other theme. */
export function useHalloween(): boolean {
  return useSyncExternalStore(subscribe, () => document.documentElement.dataset.theme === 'halloween');
}
