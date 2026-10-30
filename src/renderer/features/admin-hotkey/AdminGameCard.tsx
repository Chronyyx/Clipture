import { Check, Keyboard, X } from 'lucide-react';
import { AnimatePresence, motion } from 'motion/react';
import { useEffect, useRef, useState } from 'react';
import { needsAdminGameHotkey, useAdminGameHotkey } from './adminGameHotkeyStore';

const enter = { type: 'spring', bounce: 0, visualDuration: 0.28 } as const;
// The success card counts down visibly; hovering it pauses the countdown.
const DONE_MS = 3000;

// Asked here, out of the game: Windows' prompt takes over the screen, so it
// never appears while playing. Pressing Allow is the consent; the prompt
// follows directly. Not now holds for the rest of the session.
export function AdminGameCard() {
  const { state, phase, message, allow, dismiss } = useAdminGameHotkey();
  const [celebrating, setCelebrating] = useState(false);
  const [paused, setPaused] = useState(false);
  const remaining = useRef(DONE_MS);
  // The game's name while it is known; it is forgotten a minute after it closes.
  const game = state?.game ?? null;
  const where = game ?? 'admin games';

  useEffect(() => {
    if (phase !== 'allowed') return;
    remaining.current = DONE_MS;
    setPaused(false);
    setCelebrating(true);
  }, [phase]);

  // The bar only shows the countdown; this timer ends it, motion or not.
  useEffect(() => {
    if (!celebrating || paused) return;
    const started = performance.now();
    const timer = window.setTimeout(() => setCelebrating(false), remaining.current);
    return () => {
      window.clearTimeout(timer);
      remaining.current = Math.max(0, remaining.current - (performance.now() - started));
    };
  }, [celebrating, paused]);

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
          transition={enter}
          onMouseEnter={celebrating ? () => setPaused(true) : undefined}
          onMouseLeave={celebrating ? () => setPaused(false) : undefined}
          onClick={celebrating ? () => setCelebrating(false) : undefined}>
          {celebrating && (
            <>
              <button type='button' className='admin-game-card-close' aria-label='Close' onClick={() => setCelebrating(false)}>
                <X size={15} aria-hidden='true' />
              </button>
              <span className='admin-game-card-timer' data-paused={paused || undefined}
                style={{ animationDuration: `${DONE_MS}ms` }} aria-hidden='true' />
            </>
          )}
          <span className='admin-game-card-icon' aria-hidden='true'>
            {celebrating ? <Check size={20} /> : <Keyboard size={20} />}
          </span>
          {celebrating ? (
            <div className='admin-game-card-copy'>
              <h2 id='admin-game-card-title'>Your save shortcut works in {where} now</h2>
              <p>It works by itself in every admin game. Turn it off anytime in Settings.</p>
            </div>
          ) : (
            <div className='admin-game-card-copy'>
              <h2 id='admin-game-card-title'>
                {outdated ? `Allow the shortcut again for ${where}` : `Make your save shortcut work in ${where}`}
              </h2>
              <p>
                {phase === 'declined' ? 'Not allowed. The Windows prompt was declined, and you can try again anytime.'
                  : phase === 'failed' ? `Not allowed. ${message || 'Windows did not finish setting it up.'}`
                  : outdated ? 'Clipture updated, so Windows needs to approve the new version once.'
                  : game ? `${game} runs as administrator, so Windows blocks other apps' shortcuts in it, Discord's too.`
                  : "Some games run as administrator, so Windows blocks other apps' shortcuts in them, Discord's too."}
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
