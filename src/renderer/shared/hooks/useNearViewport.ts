import { useEffect, useRef, useState } from "react";
import type { MutableRefObject } from "react";

export function useNearViewport<T extends Element>(delayMs = 75): [MutableRefObject<T | null>, boolean] {
  const elementRef = useRef<T | null>(null);
  const [intersecting, setIntersecting] = useState(false);
  const [ready, setReady] = useState(false);

  useEffect(() => {
    const element = elementRef.current;
    if (!element || typeof IntersectionObserver === "undefined") {
      setIntersecting(true);
      return;
    }
    const observer = new IntersectionObserver(
      (entries) => setIntersecting(entries.some((entry) => entry.isIntersecting)),
      { rootMargin: "700px 0px" }
    );
    observer.observe(element);
    return () => observer.disconnect();
  }, []);

  useEffect(() => {
    if (!intersecting) {
      setReady(false);
      return;
    }
    const timer = window.setTimeout(() => setReady(true), delayMs);
    return () => window.clearTimeout(timer);
  }, [delayMs, intersecting]);

  return [elementRef, ready];
}
