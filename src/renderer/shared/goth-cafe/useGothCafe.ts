import { useSyncExternalStore } from 'react';

function subscribe(onChange: () => void) {
  const observer = new MutationObserver(onChange);
  observer.observe(document.documentElement, { attributes: true, attributeFilter: ['data-theme'] });
  return () => observer.disconnect();
}

/** True while the Goth café theme is applied (saved or previewed); none of
 * its cast mounts under any other theme. */
export function useGothCafe(): boolean {
  return useSyncExternalStore(subscribe, () => document.documentElement.dataset.theme === 'goth-cafe');
}
