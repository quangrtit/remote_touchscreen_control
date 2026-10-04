const MAX_MOVE_BACKLOG_BYTES = 32 * 1024;
const INITIAL_RECONNECT_DELAY_MS = 300;
const MAX_RECONNECT_DELAY_MS = 5000;

export class WebSocketTransport {
  #socket = null;
  #reconnectTimer = 0;
  #reconnectDelay = INITIAL_RECONNECT_DELAY_MS;
  #disposed = false;
  #onStateChange;

  constructor(onStateChange) {
    this.#onStateChange = onStateChange;
  }

  connect() {
    if (this.#disposed || this.#socket) {
      return;
    }

    this.#emitState("connecting");
    const scheme = location.protocol === "https:" ? "wss:" : "ws:";
    const socket = new WebSocket(`${scheme}//${location.host}/input`);
    socket.binaryType = "arraybuffer";
    this.#socket = socket;

    socket.addEventListener("open", () => {
      if (socket !== this.#socket) return;
      this.#reconnectDelay = INITIAL_RECONNECT_DELAY_MS;
      this.#emitState("connected");
    });

    socket.addEventListener("close", () => {
      if (socket !== this.#socket) return;
      this.#socket = null;
      this.#emitState("disconnected");
      this.#scheduleReconnect();
    });

    socket.addEventListener("error", () => {
      socket.close();
    });
  }

  send(packet, highPriority = false) {
    const socket = this.#socket;
    if (!socket || socket.readyState !== WebSocket.OPEN) {
      return false;
    }
    if (!highPriority && socket.bufferedAmount > MAX_MOVE_BACKLOG_BYTES) {
      return false;
    }
    socket.send(packet);
    return true;
  }

  dispose() {
    this.#disposed = true;
    clearTimeout(this.#reconnectTimer);
    this.#socket?.close();
    this.#socket = null;
  }

  #scheduleReconnect() {
    if (this.#disposed) return;
    clearTimeout(this.#reconnectTimer);
    this.#reconnectTimer = setTimeout(() => this.connect(), this.#reconnectDelay);
    this.#reconnectDelay = Math.min(
      this.#reconnectDelay * 2,
      MAX_RECONNECT_DELAY_MS,
    );
  }

  #emitState(state) {
    this.#onStateChange?.(state);
  }
}
