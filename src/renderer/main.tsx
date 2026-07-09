import React from "react";
import ReactDOM from "react-dom/client";
import { App } from "./App";
import "./styles.css";

import { NotificationOverlay } from "./NotificationOverlay";

const isNotification = window.location.hash === "#notification";

ReactDOM.createRoot(document.getElementById("root") as HTMLElement).render(
  <React.StrictMode>
    {isNotification ? <NotificationOverlay /> : <App />}
  </React.StrictMode>
);
