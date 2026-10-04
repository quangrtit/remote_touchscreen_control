import { Fmp4Muxer, codecFromSps, parseParameterSets } from "./fmp4-muxer.js";
import { clientLog, describeError } from "../diagnostics.js";

const MIN_HEADER_SIZE = 24;
const EXTENDED_HEADER_SIZE = 28;
const MAGIC = [0x52, 0x54, 0x56, 0x31];
const INITIAL_RECONNECT_DELAY_MS = 300;
const MAX_RECONNECT_DELAY_MS = 5000;
const FIRST_FRAME_TIMEOUT_MS = 5000;
const DEFAULT_FRAME_DURATION_US = 33_333;
const MAX_MSE_QUEUE_SEGMENTS = 12;
const MSE_MAX_LIVE_LATENCY_SECONDS = 0.12;
const MSE_TARGET_LIVE_LATENCY_SECONDS = 0.03;

export class ScreenStream {
  #canvas;
  #video;
  #frameElement;
  #viewport;
  #placeholder;
  #context;
  #decoder = null;
  #socket = null;
  #configuredCodec = "";
  #waitingForKeyFrame = true;
  #reconnectTimer = 0;
  #reconnectDelay = INITIAL_RECONNECT_DELAY_MS;
  #failedConnections = 0;
  #firstFrameTimer = 0;
  #disposed = false;
  #sourceWidth = 16;
  #sourceHeight = 9;
  #onStateChange;
  #resizeObserver;
  #useWebCodecs;
  #mediaSource = null;
  #sourceBuffer = null;
  #mediaUrl = "";
  #appendQueue = [];
  #muxer = null;
  #mseNeedsResync = false;
  #packetCount = 0;
  #metricsPacketCount = 0;
  #renderedFrameCount = 0;
  #lastMetricsAt = 0;
  #webCodecsFailures = [];
  #loggedEvents = new Set();

  constructor(canvas, video, frameElement, viewport, placeholder, onStateChange) {
    this.#canvas = canvas;
    this.#video = video;
    this.#video.disableRemotePlayback = true;
    this.#frameElement = frameElement;
    this.#viewport = viewport;
    this.#placeholder = placeholder;
    this.#onStateChange = onStateChange;
    this.#context = canvas.getContext("2d", { alpha: false, desynchronized: true });
    const forceMse = new URLSearchParams(location.search).get("decoder") === "mse";
    const webCodecsAvailable = "VideoDecoder" in window && "EncodedVideoChunk" in window;
    // Safari exposes H.264 WebCodecs on LAN HTTP pages. Requiring a secure
    // context here unnecessarily forced iPhone/iPad through buffered MSE.
    this.#useWebCodecs = !forceMse && webCodecsAvailable;
    clientLog("video-mode", {
      forceMse,
      selected: this.#useWebCodecs ? "webcodecs" : "mse",
      secureContext: isSecureContext,
      webCodecsAvailable,
      mediaSource: "MediaSource" in window,
      managedMediaSource: "ManagedMediaSource" in window,
    });
    for (const eventName of ["loadedmetadata", "canplay", "playing", "waiting", "stalled"]) {
      video.addEventListener(eventName, () => {
        this.#logOnce(`video-${eventName}`, `video-${eventName}`, this.#videoState());
      });
    }
    video.addEventListener("error", () => {
      clientLog("video-error", {
        ...this.#videoState(),
        code: video.error?.code ?? 0,
        message: video.error?.message ?? "unknown",
      });
    });
    this.#resizeObserver = new ResizeObserver(() => this.#layoutFrame());
    this.#resizeObserver.observe(viewport);
  }

  get coordinateElement() {
    return this.#frameElement;
  }

  connect() {
    if (this.#disposed || this.#socket) return;
    if (!this.#useWebCodecs && !("MediaSource" in window) && !("ManagedMediaSource" in window)) {
      clientLog("video-unsupported");
      this.#emitState("error", "Trình duyệt không hỗ trợ giải mã H.264");
      return;
    }

    const connectionMessage = "Sai token hoặc không tới được host; hãy mở lại URL mới";
    this.#emitState(
      this.#failedConnections >= 3 ? "error" : "connecting",
      this.#failedConnections >= 3 ? connectionMessage : undefined,
    );
    const scheme = location.protocol === "https:" ? "wss:" : "ws:";
    const socket = new WebSocket(`${scheme}//${location.host}/screen`);
    socket.binaryType = "arraybuffer";
    this.#socket = socket;
    let opened = false;
    socket.addEventListener("open", () => {
      if (socket !== this.#socket) return;
      opened = true;
      this.#packetCount = 0;
      this.#metricsPacketCount = 0;
      this.#renderedFrameCount = 0;
      this.#lastMetricsAt = performance.now();
      this.#loggedEvents.clear();
      clientLog("screen-ws-open");
      this.#failedConnections = 0;
      this.#reconnectDelay = INITIAL_RECONNECT_DELAY_MS;
      this.#emitState("connected");
      clearTimeout(this.#firstFrameTimer);
      this.#firstFrameTimer = setTimeout(() => {
        if (socket !== this.#socket) return;
        this.#failedConnections = 3;
        this.#emitState("error", "Đã kết nối nhưng host chưa gửi được video");
        socket.close();
      }, FIRST_FRAME_TIMEOUT_MS);
    });
    socket.addEventListener("message", (event) => this.#onPacket(event.data));
    socket.addEventListener("close", (event) => {
      if (socket !== this.#socket) return;
      clientLog("screen-ws-close", {
        code: event.code,
        reason: event.reason,
        clean: event.wasClean,
        packets: this.#packetCount,
      });
      clearTimeout(this.#firstFrameTimer);
      if (!opened) this.#failedConnections += 1;
      this.#socket = null;
      this.#resetDecoder();
      this.#emitState(
        this.#failedConnections >= 3 ? "error" : "disconnected",
        this.#failedConnections >= 3 ? connectionMessage : undefined,
      );
      this.#scheduleReconnect();
    });
    socket.addEventListener("error", () => {
      clientLog("screen-ws-error", { packets: this.#packetCount });
      socket.close();
    });
  }

  dispose() {
    this.#disposed = true;
    clearTimeout(this.#reconnectTimer);
    clearTimeout(this.#firstFrameTimer);
    this.#resizeObserver.disconnect();
    this.#socket?.close();
    this.#socket = null;
    this.#resetDecoder();
  }

  #onPacket(buffer) {
    if (!(buffer instanceof ArrayBuffer) || buffer.byteLength <= MIN_HEADER_SIZE) {
      this.#logOnce("invalid-packet-type", "screen-invalid-packet", {
        type: Object.prototype.toString.call(buffer),
        bytes: buffer?.byteLength ?? buffer?.size ?? 0,
      });
      return;
    }
    const bytes = new Uint8Array(buffer);
    if (!MAGIC.every((value, index) => bytes[index] === value)) {
      this.#logOnce("invalid-magic", "screen-invalid-magic");
      return;
    }
    const view = new DataView(buffer);
    const headerSize = view.getUint16(6, true);
    if (headerSize < MIN_HEADER_SIZE || headerSize >= buffer.byteLength) {
      this.#logOnce("invalid-header", "screen-invalid-header", {
        headerSize,
        bytes: buffer.byteLength,
      });
      return;
    }
    const keyFrame = (view.getUint8(5) & 1) !== 0;
    const width = view.getUint32(8, true);
    const height = view.getUint32(12, true);
    const timestamp = Number(view.getBigUint64(16, true));
    const frameDuration = headerSize >= EXTENDED_HEADER_SIZE
      ? view.getUint32(24, true) : DEFAULT_FRAME_DURATION_US;
    const payload = new Uint8Array(buffer, headerSize);
    this.#packetCount += 1;
    this.#metricsPacketCount += 1;
    if (this.#packetCount === 1) {
      clientLog("screen-first-packet", {
        bytes: buffer.byteLength,
        payloadBytes: payload.byteLength,
        keyFrame,
        width,
        height,
        frameDuration,
      });
    }
    clearTimeout(this.#firstFrameTimer);

    if (width !== this.#sourceWidth || height !== this.#sourceHeight) {
      this.#sourceWidth = width;
      this.#sourceHeight = height;
      this.#layoutFrame();
    }
    if (this.#useWebCodecs) {
      this.#decodeWithWebCodecs(
        payload, timestamp, frameDuration, keyFrame, width, height,
      );
    } else {
      this.#decodeWithMediaSource(
        payload, timestamp, frameDuration, keyFrame, width, height,
      );
    }
  }

  #decodeWithWebCodecs(payload, timestamp, frameDuration, keyFrame, width, height) {
    if (keyFrame) {
      const { sps } = parseParameterSets(payload);
      const codec = codecFromSps(sps);
      this.#logOnce("webcodecs-codec", "webcodecs-codec", {
        codec,
        hasSps: Boolean(sps),
      });
      if ((!this.#decoder || codec !== this.#configuredCodec) &&
          !this.#configureDecoder(codec, width, height)) {
        this.#fallbackToMediaSource("configure-failed", { codec });
        this.#decodeWithMediaSource(
          payload, timestamp, frameDuration, keyFrame, width, height,
        );
        return;
      }
      this.#waitingForKeyFrame = false;
    }
    if (!this.#decoder || this.#waitingForKeyFrame) return;
    // Allow a short startup burst, but never let a sustained decode queue turn
    // into seconds of stale video.
    if (this.#decoder.decodeQueueSize > 8) {
      clientLog("webcodecs-backlog", { queueSize: this.#decoder.decodeQueueSize });
      this.#recoverWebCodecs("decode-backlog");
      return;
    }
    try {
      this.#decoder.decode(new EncodedVideoChunk({
        type: keyFrame ? "key" : "delta", timestamp, data: payload,
      }));
    } catch (error) {
      console.error("H.264 decode failed", error);
      clientLog("webcodecs-decode-error", { error: describeError(error) });
      this.#recoverWebCodecs("decode-failed", { error: describeError(error) });
    }
  }

  #decodeWithMediaSource(payload, timestamp, frameDuration, keyFrame, width, height) {
    if (this.#appendQueue.length >= MAX_MSE_QUEUE_SEGMENTS) {
      this.#logOnce("mse-backlog", "mse-backlog", {
        queuedSegments: this.#appendQueue.length,
      });
      this.#mseNeedsResync = true;
    }
    if (this.#mseNeedsResync) {
      if (!keyFrame) return;
      this.#resetMediaSource();
    }
    if (!this.#muxer) {
      if (!keyFrame) return;
      const { sps, pps } = parseParameterSets(payload);
      if (!sps || !pps) {
        this.#logOnce("mse-missing-parameters", "mse-missing-parameters", {
          hasSps: Boolean(sps),
          hasPps: Boolean(pps),
          payloadBytes: payload.byteLength,
        });
        return;
      }
      const codec = codecFromSps(sps);
      this.#logOnce("mse-codec", "mse-codec", {
        codec,
        width,
        height,
        frameDuration,
        spsBytes: sps.byteLength,
        ppsBytes: pps.byteLength,
      });
      this.#muxer = new Fmp4Muxer(width, height, frameDuration);
      this.#initializeMediaSource(codec);
      this.#appendQueue.push(this.#muxer.initializationSegment(sps, pps));
      this.#waitingForKeyFrame = false;
      this.#mseNeedsResync = false;
    }
    this.#appendQueue.push(this.#muxer.mediaSegment(payload, timestamp, keyFrame));
    this.#drainAppendQueue();
  }

  #initializeMediaSource(codec) {
    const MediaSourceClass = window.ManagedMediaSource ?? window.MediaSource;
    const mimeType = `video/mp4; codecs="${codec}"`;
    const supported = MediaSourceClass.isTypeSupported(mimeType);
    clientLog("mse-initialize", {
      implementation: MediaSourceClass.name,
      mimeType,
      supported,
    });
    if (!supported) {
      this.#emitState("error", `H.264 ${codec} không được hỗ trợ`);
      return;
    }
    const mediaSource = new MediaSourceClass();
    this.#mediaSource = mediaSource;
    this.#mediaUrl = URL.createObjectURL(mediaSource);
    this.#video.disableRemotePlayback = true;
    this.#video.src = this.#mediaUrl;
    this.#video.load();
    this.#video.hidden = false;
    this.#canvas.hidden = true;
    void this.#video.play().catch((error) => {
      clientLog("mse-initial-play-error", { error: describeError(error) });
    });
    if ("onstartstreaming" in mediaSource) {
      mediaSource.addEventListener("startstreaming", () => {
        clientLog("mse-startstreaming");
        this.#drainAppendQueue();
      });
      mediaSource.addEventListener("endstreaming", () => {
        clientLog("mse-endstreaming");
      });
    }
    mediaSource.addEventListener("sourceclose", () => {
      clientLog("mse-sourceclose", { readyState: mediaSource.readyState });
    });
    mediaSource.addEventListener("sourceended", () => {
      clientLog("mse-sourceended", { readyState: mediaSource.readyState });
    });
    mediaSource.addEventListener("sourceopen", () => {
      if (this.#mediaSource !== mediaSource || this.#sourceBuffer) return;
      clientLog("mse-sourceopen", { readyState: mediaSource.readyState });
      let sourceBuffer;
      try {
        sourceBuffer = mediaSource.addSourceBuffer(mimeType);
      } catch (error) {
        clientLog("mse-add-source-buffer-error", { error: describeError(error), mimeType });
        this.#emitState("error", "Không khởi tạo được bộ giải mã H.264");
        return;
      }
      this.#sourceBuffer = sourceBuffer;
      this.#sourceBuffer.mode = "segments";
      sourceBuffer.addEventListener("error", () => {
        clientLog("mse-source-buffer-error", this.#videoState());
      });
      sourceBuffer.addEventListener("abort", () => {
        clientLog("mse-source-buffer-abort", this.#videoState());
      });
      sourceBuffer.addEventListener("updateend", () => {
        if (this.#sourceBuffer !== sourceBuffer) return;
        this.#logOnce("mse-updateend", "mse-updateend", this.#videoState());
        this.#keepPlaybackLive();
        this.#drainAppendQueue();
      });
      this.#drainAppendQueue();
      void this.#video.play().catch((error) => {
        clientLog("mse-play-error", { error: describeError(error) });
      });
    }, { once: true });
    setTimeout(() => {
      if (this.#mediaSource === mediaSource && mediaSource.readyState === "closed") {
        clientLog("mse-sourceopen-timeout", this.#videoState());
      }
    }, 2000);
  }

  #drainAppendQueue() {
    if (!this.#sourceBuffer || this.#sourceBuffer.updating || !this.#appendQueue.length) return;
    try {
      const segments = this.#appendQueue.splice(0);
      const data = concatenate(segments);
      this.#logOnce("mse-first-append", "mse-append", {
        segments: segments.length,
        bytes: data.byteLength,
      });
      this.#sourceBuffer.appendBuffer(data);
    } catch (error) {
      console.error("MSE append failed", error);
      clientLog("mse-append-error", { error: describeError(error) });
      this.#resetMediaSource();
      this.#waitingForKeyFrame = true;
    }
  }

  #keepPlaybackLive() {
    const ranges = this.#video.buffered;
    if (!ranges.length) return;
    const end = ranges.end(ranges.length - 1);
    if (!Number.isFinite(this.#video.currentTime) ||
        end - this.#video.currentTime > MSE_MAX_LIVE_LATENCY_SECONDS) {
      this.#video.currentTime = Math.max(
        ranges.start(0), end - MSE_TARGET_LIVE_LATENCY_SECONDS,
      );
    }
    this.#logOnce("mse-rendered", "mse-rendered", {
      ...this.#videoState(),
      bufferStart: ranges.start(0),
      bufferEnd: end,
    });
    void this.#video.play().catch((error) => {
      clientLog("mse-play-error", { error: describeError(error) });
    });
    this.#placeholder.hidden = true;
    if (end - ranges.start(0) > 5 && this.#sourceBuffer && !this.#sourceBuffer.updating) {
      this.#sourceBuffer.remove(ranges.start(0), end - 2);
    }
  }

  #configureDecoder(codec, width, height) {
    this.#resetWebCodecs();
    this.#decoder = new VideoDecoder({
      output: (frame) => this.#renderFrame(frame),
      error: (error) => {
        console.error("VideoDecoder error", error);
        clientLog("webcodecs-decoder-error", { error: describeError(error) });
        this.#recoverWebCodecs("decoder-error", {
          error: describeError(error),
        });
      },
    });
    const config = {
      codec,
      codedWidth: width,
      codedHeight: height,
      optimizeForLatency: true,
      // Let the browser choose its native H.264 path. Forcing hardware can
      // make an otherwise supported configuration fail on some devices.
      hardwareAcceleration: "no-preference",
    };
    void VideoDecoder.isConfigSupported(config).then((result) => {
      clientLog("webcodecs-support", { codec, supported: result.supported });
    }).catch((error) => {
      clientLog("webcodecs-support-error", { error: describeError(error) });
    });
    try {
      this.#decoder.configure(config);
    } catch (error) {
      clientLog("webcodecs-configure-error", { codec, error: describeError(error) });
      this.#resetWebCodecs();
      return false;
    }
    this.#configuredCodec = codec;
    this.#canvas.hidden = false;
    this.#video.hidden = true;
    return true;
  }

  #renderFrame(frame) {
    this.#logOnce("webcodecs-rendered", "webcodecs-rendered", {
      width: frame.displayWidth,
      height: frame.displayHeight,
      timestamp: frame.timestamp,
    });
    if (this.#canvas.width !== frame.displayWidth || this.#canvas.height !== frame.displayHeight) {
      this.#canvas.width = frame.displayWidth;
      this.#canvas.height = frame.displayHeight;
    }
    this.#context.drawImage(frame, 0, 0, this.#canvas.width, this.#canvas.height);
    frame.close();
    this.#renderedFrameCount += 1;
    const now = performance.now();
    if (now - this.#lastMetricsAt >= 2000) {
      clientLog("webcodecs-metrics", {
        renderedFrames: this.#renderedFrameCount,
        packets: this.#metricsPacketCount,
        decodeQueueSize: this.#decoder?.decodeQueueSize ?? 0,
        intervalMs: Math.round(now - this.#lastMetricsAt),
      });
      this.#renderedFrameCount = 0;
      this.#metricsPacketCount = 0;
      this.#lastMetricsAt = now;
    }
    this.#placeholder.hidden = true;
  }

  #fallbackToMediaSource(reason, details = {}) {
    if (!this.#useWebCodecs) return;
    clientLog("video-fallback", {
      from: "webcodecs", to: "mse", reason, ...details,
    });
    this.#useWebCodecs = false;
    this.#resetWebCodecs();
    this.#waitingForKeyFrame = true;
  }

  #recoverWebCodecs(reason, details = {}) {
    const now = performance.now();
    this.#webCodecsFailures = this.#webCodecsFailures.filter(
      (failedAt) => now - failedAt < 60_000,
    );
    this.#webCodecsFailures.push(now);
    if (this.#webCodecsFailures.length >= 3) {
      this.#fallbackToMediaSource(reason, {
        ...details, failuresIn60s: this.#webCodecsFailures.length,
      });
      this.#socket?.close(4001, "decoder fallback");
      return;
    }
    clientLog("webcodecs-restart", {
      reason,
      attempt: this.#webCodecsFailures.length,
      ...details,
    });
    this.#resetWebCodecs();
    this.#waitingForKeyFrame = true;
    // A new WebSocket session makes the host send a new IDR immediately.
    this.#socket?.close(4002, "decoder restart");
  }

  #layoutFrame() {
    const viewportWidth = this.#viewport.clientWidth;
    const viewportHeight = this.#viewport.clientHeight;
    const size = calculateContainSize(
      this.#sourceWidth,
      this.#sourceHeight,
      viewportWidth,
      viewportHeight,
    );
    if (!size) return;
    this.#frameElement.style.width = `${size.width}px`;
    this.#frameElement.style.height = `${size.height}px`;
  }

  #resetWebCodecs() {
    if (this.#decoder) {
      try { this.#decoder.close(); } catch {}
    }
    this.#decoder = null;
    this.#configuredCodec = "";
  }

  #resetDecoder() {
    this.#resetWebCodecs();
    this.#resetMediaSource();
    this.#waitingForKeyFrame = true;
  }

  #resetMediaSource() {
    if (this.#sourceBuffer?.updating) {
      try { this.#sourceBuffer.abort(); } catch {}
    }
    this.#sourceBuffer = null;
    this.#mediaSource = null;
    this.#appendQueue.length = 0;
    this.#muxer = null;
    this.#mseNeedsResync = false;
    this.#video.removeAttribute("src");
    this.#video.load();
    if (this.#mediaUrl) URL.revokeObjectURL(this.#mediaUrl);
    this.#mediaUrl = "";
  }

  #scheduleReconnect() {
    if (this.#disposed) return;
    clearTimeout(this.#reconnectTimer);
    this.#reconnectTimer = setTimeout(() => this.connect(), this.#reconnectDelay);
    this.#reconnectDelay = Math.min(this.#reconnectDelay * 2, MAX_RECONNECT_DELAY_MS);
  }

  #videoState() {
    const ranges = [];
    try {
      for (let index = 0; index < this.#video.buffered.length; index += 1) {
        ranges.push([
          Number(this.#video.buffered.start(index).toFixed(3)),
          Number(this.#video.buffered.end(index).toFixed(3)),
        ]);
      }
    } catch {}
    return {
      readyState: this.#video.readyState,
      networkState: this.#video.networkState,
      paused: this.#video.paused,
      currentTime: Number.isFinite(this.#video.currentTime)
        ? Number(this.#video.currentTime.toFixed(3)) : null,
      buffered: ranges,
    };
  }

  #logOnce(key, event, details = {}) {
    if (this.#loggedEvents.has(key)) return;
    this.#loggedEvents.add(key);
    clientLog(event, details);
  }

  #emitState(state, message) {
    this.#onStateChange?.(state, message);
  }
}

export function calculateContainSize(sourceWidth, sourceHeight, viewportWidth, viewportHeight) {
  if (sourceWidth <= 0 || sourceHeight <= 0 || viewportWidth <= 0 || viewportHeight <= 0) {
    return null;
  }
  const sourceAspect = sourceWidth / sourceHeight;
  const viewportAspect = viewportWidth / viewportHeight;
  const constrainByWidth = viewportAspect <= sourceAspect;
  return {
    width: constrainByWidth ? viewportWidth : viewportHeight * sourceAspect,
    height: constrainByWidth ? viewportWidth / sourceAspect : viewportHeight,
  };
}

function concatenate(segments) {
  if (segments.length === 1) return segments[0];
  const size = segments.reduce((total, segment) => total + segment.byteLength, 0);
  const output = new Uint8Array(size);
  let offset = 0;
  for (const segment of segments) {
    output.set(segment, offset);
    offset += segment.byteLength;
  }
  return output;
}
