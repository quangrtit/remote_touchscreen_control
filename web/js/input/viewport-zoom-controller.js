const MIN_SCALE = 1;
const MAX_SCALE = 4;
const TAP_MAX_DURATION_MS = 350;
const TAP_MAX_MOVEMENT_PX = 8;

export class ViewportZoomController {
  #viewport;
  #frame;
  #onTap;
  #onScaleChange;
  #enabled = false;
  #scale = 1;
  #panX = 0;
  #panY = 0;
  #pointers = new Map();
  #startScale = 1;
  #startPanX = 0;
  #startPanY = 0;
  #startDistance = 1;
  #startCenterX = 0;
  #startCenterY = 0;
  #panPointerX = 0;
  #panPointerY = 0;
  #tapCandidate = false;
  #tapStartedAt = 0;
  #abortController = new AbortController();
  #resizeObserver;

  constructor(viewport, frame, onTap, onScaleChange) {
    this.#viewport = viewport;
    this.#frame = frame;
    this.#onTap = onTap;
    this.#onScaleChange = onScaleChange;
    const options = { signal: this.#abortController.signal };
    viewport.addEventListener("pointerdown", this.#onPointerDown, options);
    viewport.addEventListener("pointermove", this.#onPointerMove, options);
    viewport.addEventListener("pointerup", this.#onPointerUp, options);
    viewport.addEventListener("pointercancel", this.#onPointerUp, options);
    viewport.addEventListener("lostpointercapture", this.#onLostCapture, options);
    this.#resizeObserver = new ResizeObserver(() => {
      this.#clampPan();
      this.#applyTransform();
    });
    this.#resizeObserver.observe(viewport);
    this.#applyTransform();
  }

  get scale() {
    return this.#scale;
  }

  setEnabled(enabled) {
    this.#enabled = enabled;
    this.#clearPointers();
  }

  reset() {
    this.#scale = 1;
    this.#panX = 0;
    this.#panY = 0;
    this.#applyTransform();
    this.#onScaleChange?.(this.#scale);
  }

  dispose() {
    this.#clearPointers();
    this.#resizeObserver.disconnect();
    this.#abortController.abort();
  }

  #onPointerDown = (event) => {
    if (!this.#enabled || event.target.closest?.(".hud")) return;
    event.preventDefault();
    this.#viewport.setPointerCapture(event.pointerId);
    this.#pointers.set(event.pointerId, { x: event.clientX, y: event.clientY });
    if (this.#pointers.size === 1) {
      this.#beginPan(event.clientX, event.clientY);
      this.#tapCandidate = true;
      this.#tapStartedAt = performance.now();
    } else {
      this.#tapCandidate = false;
      this.#beginPinch();
    }
  };

  #onPointerMove = (event) => {
    if (!this.#enabled || !this.#pointers.has(event.pointerId)) return;
    event.preventDefault();
    this.#pointers.set(event.pointerId, { x: event.clientX, y: event.clientY });
    if (this.#pointers.size >= 2) {
      this.#tapCandidate = false;
      this.#updatePinch();
      return;
    }
    const movement = Math.hypot(
      event.clientX - this.#panPointerX,
      event.clientY - this.#panPointerY,
    );
    if (movement > TAP_MAX_MOVEMENT_PX) this.#tapCandidate = false;
    this.#panX = this.#startPanX + event.clientX - this.#panPointerX;
    this.#panY = this.#startPanY + event.clientY - this.#panPointerY;
    this.#clampPan();
    this.#applyTransform();
  };

  #onPointerUp = (event) => {
    if (!this.#enabled || !this.#pointers.has(event.pointerId)) return;
    event.preventDefault();
    const shouldTap = this.#pointers.size === 1 && this.#tapCandidate &&
      performance.now() - this.#tapStartedAt <= TAP_MAX_DURATION_MS;
    this.#pointers.delete(event.pointerId);
    if (this.#viewport.hasPointerCapture(event.pointerId)) {
      this.#viewport.releasePointerCapture(event.pointerId);
    }
    if (shouldTap) this.#onTap?.(event.clientX, event.clientY);
    this.#tapCandidate = false;
    if (this.#pointers.size === 1) {
      const remaining = this.#pointers.values().next().value;
      this.#beginPan(remaining.x, remaining.y);
    }
  };

  #onLostCapture = (event) => {
    if (!this.#pointers.delete(event.pointerId)) return;
    this.#tapCandidate = false;
  };

  #beginPan(x, y) {
    this.#startPanX = this.#panX;
    this.#startPanY = this.#panY;
    this.#panPointerX = x;
    this.#panPointerY = y;
  }

  #beginPinch() {
    const [first, second] = [...this.#pointers.values()];
    this.#startScale = this.#scale;
    this.#startPanX = this.#panX;
    this.#startPanY = this.#panY;
    this.#startDistance = Math.max(
      1, Math.hypot(second.x - first.x, second.y - first.y),
    );
    this.#startCenterX = (first.x + second.x) / 2;
    this.#startCenterY = (first.y + second.y) / 2;
  }

  #updatePinch() {
    const [first, second] = [...this.#pointers.values()];
    const centerX = (first.x + second.x) / 2;
    const centerY = (first.y + second.y) / 2;
    const distance = Math.max(1, Math.hypot(second.x - first.x, second.y - first.y));
    const nextScale = clamp(
      this.#startScale * distance / this.#startDistance,
      MIN_SCALE,
      MAX_SCALE,
    );
    const viewportRect = this.#viewport.getBoundingClientRect();
    const viewportCenterX = viewportRect.left + viewportRect.width / 2;
    const viewportCenterY = viewportRect.top + viewportRect.height / 2;
    const contentX = (
      this.#startCenterX - viewportCenterX - this.#startPanX
    ) / this.#startScale;
    const contentY = (
      this.#startCenterY - viewportCenterY - this.#startPanY
    ) / this.#startScale;
    this.#scale = nextScale;
    this.#panX = centerX - viewportCenterX - contentX * nextScale;
    this.#panY = centerY - viewportCenterY - contentY * nextScale;
    this.#clampPan();
    this.#applyTransform();
    this.#onScaleChange?.(this.#scale);
  }

  #clampPan() {
    if (this.#scale <= MIN_SCALE) {
      this.#panX = 0;
      this.#panY = 0;
      return;
    }
    const maxPanX = Math.max(
      0,
      (this.#frame.offsetWidth * this.#scale - this.#viewport.clientWidth) / 2,
    );
    const maxPanY = Math.max(
      0,
      (this.#frame.offsetHeight * this.#scale - this.#viewport.clientHeight) / 2,
    );
    this.#panX = clamp(this.#panX, -maxPanX, maxPanX);
    this.#panY = clamp(this.#panY, -maxPanY, maxPanY);
  }

  #applyTransform() {
    this.#frame.style.setProperty("--zoom-scale", this.#scale.toFixed(4));
    this.#frame.style.setProperty("--zoom-pan-x", `${this.#panX.toFixed(2)}px`);
    this.#frame.style.setProperty("--zoom-pan-y", `${this.#panY.toFixed(2)}px`);
  }

  #clearPointers() {
    for (const pointerId of this.#pointers.keys()) {
      if (this.#viewport.hasPointerCapture(pointerId)) {
        this.#viewport.releasePointerCapture(pointerId);
      }
    }
    this.#pointers.clear();
    this.#tapCandidate = false;
  }
}

function clamp(value, minimum, maximum) {
  return Math.min(maximum, Math.max(minimum, value));
}
