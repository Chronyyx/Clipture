import { useEffect, useState } from "react";

/** The current time, refreshed often enough for minute countdowns. */
export function useNow(intervalMs = 15_000) {
  const [now, setNow] = useState(() => Date.now());
  useEffect(() => {
    const timer = window.setInterval(() => setNow(Date.now()), intervalMs);
    return () => window.clearInterval(timer);
  }, [intervalMs]);
  return now;
}

/** "11 more minutes", rounded up so it never says zero while still open. */
export function minutesLeft(untilMs: number, now: number) {
  const minutes = Math.max(1, Math.ceil((untilMs - now) / 60_000));
  return `${minutes} more ${minutes === 1 ? "minute" : "minutes"}`;
}

/** The friend's window has closed and nothing that may outlive it is running. */
export function windowClosed(untilMs: number | null | undefined, now: number) {
  return untilMs != null && now >= untilMs;
}
