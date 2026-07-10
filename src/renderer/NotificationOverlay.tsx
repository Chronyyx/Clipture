import { useEffect, useState } from "react";
import { Film } from "lucide-react";

export function NotificationOverlay() {
  const [activeNotification, setActiveNotification] = useState<{ position: string; id: number; message: string } | null>(null);
  const [animatingOut, setAnimatingOut] = useState(false);

  useEffect(() => {
    document.body.style.background = 'transparent';
    document.documentElement.style.background = 'transparent';
    const root = document.getElementById('root');
    if (root) root.style.background = 'transparent';
    let timeoutId: number;

    const unsubscribe = window.clipture.onShowNotification((_filePath, position, message = "Clip saved!") => {
      setActiveNotification({ position, id: Date.now(), message });
      setAnimatingOut(false);
      
      // Clear any existing timeout
      if (timeoutId) window.clearTimeout(timeoutId);

      // Start animate out after 4 seconds
      timeoutId = window.setTimeout(() => {
        setAnimatingOut(true);
        window.setTimeout(() => {
          setActiveNotification(null);
          setAnimatingOut(false);
          window.clipture.hideNotification();
        }, 500);
      }, 4000);
    });

    return () => {
      document.body.style.background = '';
      document.documentElement.style.background = '';
      unsubscribe();
    };
  }, []);

  if (!activeNotification) return null;

  const { position, id, message } = activeNotification;

  return (
    <div className={`notification-container ${position}`}>
      <div key={id} className={`notification-card ${position} ${animatingOut ? "slide-out" : "slide-in"}`}>
        <div className="notification-thumbnail">
          <div style={{ width: '100%', height: '100%', display: 'flex', alignItems: 'center', justifyContent: 'center' }}>
            <Film size={32} color="#aaa" />
          </div>
        </div>
        <div className="notification-text">
          <span>{message}</span>
        </div>
      </div>
    </div>
  );
}
