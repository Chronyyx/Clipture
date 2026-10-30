import { Check, Keyboard } from 'lucide-react';
import { AnimatePresence, motion } from 'motion/react';
import { useEffect, useState } from 'react';
import { needsAdminGameHotkey, useAdminGameHotkey } from './adminGameHotkeyStore';

const enter = { type: 'spring', bounce: 0, visualDuration: 0.28 } as const;

// Asked here, out of the game: Windows' prompt takes over the screen, so it
// never appears while playing. Pressing Allow is the consent; the prompt
// follows directly. Not now holds for the rest of the session.
export function AdminGameCard() {
  const { state, phase, message, allow, dismiss } = useAdminGameHotkey();
  const [celebrating, setCelebrating] = useState(false);
  const game = state?.game ?? 'This game';

  useEffect(() => {
    if (phase !== 'allowed') return;
    setCelebrating(true);
    const timer = window.setTimeout(() => setCelebrating(false), 4000);
    return () => window.clearTimeout(timer);
  }, [phase]);

  const asking = needsAdminGameHotkey(state) && !state?.dismissed;
  const visible = celebrating || asking || (Boolean(state?.game) && (phase === 'waiting' || phase === 'declined' || phase === 'failed'));
  const outdated = state?.enabled && (state.relay === 'outdated' || state.relay === 'not-installed');

  return (
    <AnimatePresence initial={false}>
      {visible && (
        <motion.section key='admin-game-card' className={celebrating ? 'admin-game-card done' : 'admin-game-card'}
          aria-labelledby='admin-game-card-title' aria-live='polite'
          initial={{ opacity: 0, transform: 'translateY(14px)' }}
          animate={{ opacity: 1, transform: 'translateY(0px)' }}
          exit={{ opacity: 0, transform: 'translateY(14px)' }}
          transition={enter}>
          <span className='admin-game-card-icon' aria-hidden='true'>
            {celebrating ? <Check size={20} /> : <Keyboard size={20} />}
          </span>
          {celebrating ? (
            <div className='admin-game-card-copy'>
              <h2 id='admin-game-card-title'>Your save shortcut works in {game} now</h2>
              <p>It works by itself in every admin game. Turn it off anytime in Settings.</p>
            </div>
          ) : (
            <div className='admin-game-card-copy'>
              <h2 id='admin-game-card-title'>
                {outdated ? `Allow the shortcut again for ${game}` : `Make your save shortcut work in ${game}`}
              </h2>
              <p>
                {phase === 'declined' ? 'Not allowed. The Windows prompt was declined, and you can try again anytime.'
                  : phase === 'failed' ? `Not allowed. ${message || 'Windows did not finish setting it up.'}`
                  : outdated ? 'Clipture updated, so Windows needs to approve the new version once.'
                  : `${game} runs as administrator, so Windows blocks other apps' shortcuts in it, Discord's too.`}
              </p>
              <p className='admin-game-card-note'>Windows asks for permission once. Turn it off anytime in Settings.</p>
            </div>
          )}
          {!celebrating && (
            <div className='admin-game-card-actions'>
              <button type='button' className='secondary-button' onClick={() => void dismiss()} disabled={phase === 'waiting'}>
                Not now
              </button>
              <button type='button' className='primary' onClick={() => void allow()} disabled={phase === 'waiting'}>
                {phase === 'waiting' ? 'Waiting for Windows…' : phase === 'declined' || phase === 'failed' ? 'Try again' : 'Allow'}
              </button>
            </div>
          )}
        </motion.section>
      )}
    </AnimatePresence>
  );
}
