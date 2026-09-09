import React from "react";
import ReactDOM from "react-dom/client";
import { mountMainApp } from "./app/bootstrap";
import "./styles.css";

import { NotificationOverlay } from "./NotificationOverlay";

const isNotification = window.location.hash === "#notification";

if (isNotification) {
  ReactDOM.createRoot(document.getElementById("root") as HTMLElement).render(
    <React.StrictMode><NotificationOverlay /></React.StrictMode>
  );
} else {
  void mountMainApp();
}
