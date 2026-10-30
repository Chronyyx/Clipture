import type { AdminGameHotkey } from '../../../shared/types';
import { useAdminGameHotkey, type AdminGameHotkeyPhase } from '../admin-hotkey';
import { SettingRow, Toggle } from './SettingsSection';

function describe(state: AdminGameHotkey | null, phase: AdminGameHotkeyPhase, message: string) {
  if (phase === 'waiting') return 'Confirm the Windows prompt to finish.';
  if (phase === 'declined') return 'Off. The Windows prompt was declined, and you can try again anytime.';
  if (phase === 'failed') return 'Off. ' + (message || 'Windows did not finish setting it up.');
  if (phase === 'removed') return 'Removed from this PC. Turning it on asks Windows again.';
  if (!state?.enabled) {
    return "Games like Marvel Rivals run as administrator, and Windows blocks other apps' shortcuts while they have focus. Turn this on to let Clipture's through.";
  }
  if (state.relay === 'rejected') return 'On, but your save shortcut is a typing key. Pick one with Ctrl, Alt or an F key to use it in admin games.';
  if (state.relay === 'outdated' || state.relay === 'not-installed') return 'Clipture updated. Turn this off and on again so Windows can approve the new version.';
  if (state.relay === 'active' && state.game) return `On. Your save shortcut works in ${state.game} right now.`;
  return 'On. Your save shortcut works in games that run as administrator.';
}

// Off needs no prompt and leaves the helper set up, so on again is instant.
// Removing it from the PC asks Windows, as setting it up did.
export function AdminGameHotkeySetting() {
  const { state, phase, message, allow, disable, remove } = useAdminGameHotkey();
  if (!state) return null;
  const waiting = phase === 'waiting';

  return (
    <>
      <SettingRow
        title='Shortcut in admin games'
        description={
          <>
            <span aria-live='polite'>{describe(state, phase, message)}</span>
            {!state.enabled && !state.installed && !waiting && (
              <span className='setting-row-note'>Windows asks for administrator permission once.</span>
            )}
          </>
        }
        control={(id) => (
          <Toggle id={id} checked={state.enabled} disabled={waiting}
            onChange={(on) => void (on ? allow() : disable())} />
        )}
      />
      {state.installed && !state.enabled && (
        <SettingRow
          title='Admin shortcut helper'
          description='Still set up on this PC, unused while the setting above is off.'
          control={(id) => (
            <button id={id} type='button' className='secondary-button' disabled={waiting} onClick={() => void remove()}>
              {waiting ? 'Waiting for Windows…' : 'Remove from this PC'}
            </button>
          )}
        />
      )}
    </>
  );
}
