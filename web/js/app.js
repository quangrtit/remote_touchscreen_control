import { TouchController } from "./input/touch-controller.js";
import { ViewportZoomController } from "./input/viewport-zoom-controller.js";
import { WebSocketTransport } from "./network/websocket-transport.js";
import { ScreenStream } from "./video/screen-stream.js";
import { clientLog, describeError } from "./diagnostics.js";

const CLIENT_VERSION = "2026.10.04-low-latency.4";

const status = document.querySelector("#connectionStatus");
const statusLabel = document.querySelector("#connectionLabel");
const surface = document.querySelector("#touchSurface");
const canvas = document.querySelector("#screenCanvas");
const video = document.querySelector("#screenVideo");
const screenFrame = document.querySelector("#screenFrame");
const placeholder = document.querySelector("#streamPlaceholder");
const placeholderTitle = document.querySelector("#streamPlaceholderTitle");
const placeholderHint = document.querySelector("#streamPlaceholderHint");
const markers = document.querySelector("#touchMarkers");
const fitButton = document.querySelector("#fitButton");
const zoomButton = document.querySelector("#zoomButton");

const connection = { input: "connecting", video: "connecting" };
let controller;
let zoomController;
let zoomMode = false;
let lastErrorMessage = "";

clientLog("client-start", {
  version: CLIENT_VERSION,
  userAgent: navigator.userAgent,
  platform: navigator.platform,
  secureContext: isSecureContext,
  videoDecoder: "VideoDecoder" in window,
  mediaSource: "MediaSource" in window,
  managedMediaSource: "ManagedMediaSource" in window,
});
window.addEventListener("error", (event) => {
  clientLog("window-error", {
    message: event.message,
    source: event.filename,
    line: event.lineno,
    column: event.colno,
  });
});
window.addEventListener("unhandledrejection", (event) => {
  clientLog("unhandled-rejection", { error: describeError(event.reason) });
});

function updateStatus(message) {
  if (message) lastErrorMessage = message;
  if (connection.input === "connected" && connection.video === "connected") {
    status.dataset.state = "connected";
    statusLabel.textContent = "Đã kết nối";
    lastErrorMessage = "";
  } else if (connection.input === "error" || connection.video === "error") {
    status.dataset.state = "error";
    statusLabel.textContent = lastErrorMessage || "Không thể kết nối";
    placeholderTitle.textContent = "Không nhận được màn hình";
    placeholderHint.textContent = lastErrorMessage || "Hãy mở lại URL mới do host cung cấp";
  } else if (connection.input === "disconnected" || connection.video === "disconnected") {
    status.dataset.state = "disconnected";
    statusLabel.textContent = "Đang kết nối lại";
  } else {
    status.dataset.state = "connecting";
    statusLabel.textContent = "Đang kết nối";
  }

  if (connection.video !== "error") {
    placeholderTitle.textContent = "Đang nhận màn hình Windows";
    placeholderHint.textContent = "Xoay ngang thiết bị để có vùng điều khiển lớn nhất";
  }
}

const inputTransport = new WebSocketTransport((state) => {
  connection.input = state;
  updateStatus();
  if (state !== "connected") controller?.reset(false);
});

const screenStream = new ScreenStream(
  canvas,
  video,
  screenFrame,
  surface,
  placeholder,
  (state, message) => {
    connection.video = state;
    updateStatus(message);
  },
);

controller = new TouchController(surface, markers, inputTransport, screenStream.coordinateElement);
zoomController = new ViewportZoomController(
  surface,
  screenFrame,
  (x, y) => controller.sendTapAt(x, y),
  (scale) => updateZoomButton(scale),
);
inputTransport.connect();
screenStream.connect();

let fitMode = "contain";
fitButton.addEventListener("click", () => {
  fitMode = fitMode === "contain" ? "cover" : "contain";
  screenStream.setFitMode(fitMode);
  zoomController.reset();
  fitButton.textContent = fitMode === "contain" ? "Vừa màn hình" : "Lấp đầy";
});

zoomButton.addEventListener("click", () => {
  zoomMode = !zoomMode;
  if (zoomMode) {
    controller.setEnabled(false);
    zoomController.setEnabled(true);
  } else {
    zoomController.setEnabled(false);
    controller.setEnabled(true);
  }
  zoomButton.setAttribute("aria-pressed", String(zoomMode));
  updateZoomButton(zoomController.scale);
});

function updateZoomButton(scale) {
  const percent = Math.round(scale * 100);
  zoomButton.textContent = zoomMode
    ? `Xong · ${percent}%`
    : scale > 1.005 ? `Thu phóng · ${percent}%` : "Thu phóng";
}

surface.addEventListener("contextmenu", (event) => event.preventDefault());
window.addEventListener("pagehide", () => {
  controller.dispose();
  zoomController.dispose();
  inputTransport.dispose();
  screenStream.dispose();
});

if (location.search) {
  history.replaceState(null, "", `${location.pathname}${location.hash}`);
}
