import { listen, type Event, type UnlistenFn } from '@tauri-apps/api/event';

type EventHandler<T> = (payload: T) => void;

/**
 * Tauri registration is asynchronous, while Clipture's compatibility contract
 * requires an unsubscribe function synchronously. An immediate dispose is
 * remembered and applied as soon as registration resolves.
 */
export function subscribeToTauriEvent<T>(eventName: string, handler: EventHandler<T>): () => void {
  let disposed = false;
  let unlisten: UnlistenFn | undefined;

  void listen<T>(eventName, (event: Event<T>) => {
    if (!disposed) handler(event.payload);
  }).then((registeredUnlisten) => {
    if (disposed) {
      registeredUnlisten();
      return;
    }
    unlisten = registeredUnlisten;
  }).catch((error) => {
    if (!disposed) console.error('Failed to subscribe to Tauri event ' + eventName + ':', error);
  });

  return () => {
    if (disposed) return;
    disposed = true;
    const registeredUnlisten = unlisten;
    unlisten = undefined;
    registeredUnlisten?.();
  };
}
