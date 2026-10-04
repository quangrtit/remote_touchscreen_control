import { TouchAction, TouchPacketEncoder } from "../protocol/touch-packet.js";

const CONTACT_KEEP_ALIVE_MS = 50;

export class TouchController {
  #surface;
  #markers;
  #transport;
  #encoder = new TouchPacketEncoder();
  #activePointers = new Map();
  #abortController = new AbortController();
  #keepAliveTimer = 0;
  #enabled = true;
  #syntheticPointerId = 0x70000000;

  #coordinateElement;

  constructor(surface, markers, transport, coordinateElement = surface) {
    this.#surface = surface;
    this.#markers = markers;
    this.#transport = transport;
    this.#coordinateElement = coordinateElement;

    const options = { signal: this.#abortController.signal };
    surface.addEventListener("pointerdown", this.#onPointerDown, options);
    surface.addEventListener("pointermove", this.#onPointerMove, options);
    surface.addEventListener("pointerup", this.#onPointerUp, options);
    surface.addEventListener("pointercancel", this.#onPointerCancel, options);
    surface.addEventListener("lostpointercapture", this.#onLostCapture, options);
    window.addEventListener("blur", this.#onWindowBlur, options);
    this.#keepAliveTimer = window.setInterval(
      () => this.#sendKeepAlive(), CONTACT_KEEP_ALIVE_MS,
    );
  }

  reset(sendCancel = false) {
    if (sendCancel) {
      for (const [pointerId, state] of this.#activePointers) {
        this.#send(TouchAction.cancel, pointerId, state, true);
      }
    }
    this.#activePointers.clear();
    this.#markers.replaceChildren();
  }

  setEnabled(enabled) {
    if (this.#enabled === enabled) return;
    if (!enabled) this.reset(true);
    this.#enabled = enabled;
  }

  sendTapAt(clientX, clientY) {
    const point = this.#normalizedPoint({ clientX, clientY, pressure: 1 }, false);
    if (!point) return false;
    this.#syntheticPointerId = (this.#syntheticPointerId + 1) >>> 0;
    if (this.#syntheticPointerId === 0) this.#syntheticPointerId = 1;
    const pointerId = this.#syntheticPointerId;
    if (!this.#send(TouchAction.down, pointerId, point, true)) return false;
    this.#send(TouchAction.move, pointerId, point, true);
    return this.#send(
      TouchAction.up, pointerId, { ...point, pressure: 0 }, true,
    );
  }

  dispose() {
    this.reset(true);
    clearInterval(this.#keepAliveTimer);
    this.#abortController.abort();
  }

  #onPointerDown = (event) => {
    if (!this.#enabled) return;
    event.preventDefault();
    if (this.#activePointers.has(event.pointerId)) return;

    this.#surface.setPointerCapture(event.pointerId);
    const point = this.#normalizedPoint(event, false);
    if (!point) {
      this.#surface.releasePointerCapture(event.pointerId);
      return;
    }
    const marker = document.createElement("span");
    marker.className = "touch-marker";
    this.#markers.append(marker);

    const state = { ...point, marker };
    this.#activePointers.set(event.pointerId, state);
    this.#moveMarker(state);
    if (!this.#send(TouchAction.down, event.pointerId, state, true)) {
      marker.remove();
      this.#activePointers.delete(event.pointerId);
      this.#surface.releasePointerCapture(event.pointerId);
    }
  };

  #onPointerMove = (event) => {
    if (!this.#enabled) return;
    const state = this.#activePointers.get(event.pointerId);
    if (!state) return;
    event.preventDefault();

    const samples = event.getCoalescedEvents?.();
    const latest = samples?.length ? samples[samples.length - 1] : event;
    const point = this.#normalizedPoint(latest);
    if (!point) return;
    Object.assign(state, point);
    this.#moveMarker(state);
    this.#send(TouchAction.move, event.pointerId, state, false);
  };

  #onPointerUp = (event) => {
    if (!this.#enabled) return;
    this.#finishPointer(event, TouchAction.up);
  };

  #onPointerCancel = (event) => {
    if (!this.#enabled) return;
    this.#finishPointer(event, TouchAction.cancel);
  };

  #onLostCapture = (event) => {
    if (!this.#enabled) return;
    const state = this.#activePointers.get(event.pointerId);
    if (!state) return;
    this.#send(TouchAction.cancel, event.pointerId, state, true);
    state.marker.remove();
    this.#activePointers.delete(event.pointerId);
  };

  #onWindowBlur = () => this.reset(true);

  #finishPointer(event, action) {
    const state = this.#activePointers.get(event.pointerId);
    if (!state) return;
    event.preventDefault();

    const point = this.#normalizedPoint(event);
    if (point) Object.assign(state, point);
    state.pressure = 0;
    this.#send(action, event.pointerId, state, true);
    state.marker.remove();
    this.#activePointers.delete(event.pointerId);
    if (this.#surface.hasPointerCapture(event.pointerId)) {
      this.#surface.releasePointerCapture(event.pointerId);
    }
  }

  #normalizedPoint(event, clampOutside = true) {
    const rect = this.#coordinateElement.getBoundingClientRect();
    if (rect.width <= 0 || rect.height <= 0) return null;
    const rawX = (event.clientX - rect.left) / rect.width;
    const rawY = (event.clientY - rect.top) / rect.height;
    if (!clampOutside && (rawX < 0 || rawX > 1 || rawY < 0 || rawY > 1)) {
      return null;
    }
    return {
      x: clamp01(rawX),
      y: clamp01(rawY),
      pressure: event.pressure > 0 ? event.pressure : 1,
    };
  }

  #send(action, pointerId, state, highPriority) {
    const packet = this.#encoder.encode({ action, pointerId, ...state });
    return this.#transport.send(packet, highPriority);
  }

  #sendKeepAlive() {
    const first = this.#activePointers.entries().next();
    if (first.done) return;
    const [pointerId, state] = first.value;
    // The host turns one UPDATE into an atomic frame containing every active
    // contact. One small heartbeat therefore keeps two-handed holds alive.
    this.#send(TouchAction.move, pointerId, state, false);
  }

  #moveMarker({ marker, x, y }) {
    const surfaceRect = this.#surface.getBoundingClientRect();
    const videoRect = this.#coordinateElement.getBoundingClientRect();
    const markerX = videoRect.left - surfaceRect.left + x * videoRect.width;
    const markerY = videoRect.top - surfaceRect.top + y * videoRect.height;
    marker.style.transform = `translate3d(${markerX}px, ${markerY}px, 0)`;
  }
}

function clamp01(value) {
  return Math.min(1, Math.max(0, Number.isFinite(value) ? value : 0));
}
