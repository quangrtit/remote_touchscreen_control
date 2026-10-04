export const TouchAction = Object.freeze({
  down: 0,
  move: 1,
  up: 2,
  cancel: 3,
});

const VERSION = 1;
const PACKET_SIZE = 20;

export class TouchPacketEncoder {
  #buffer = new ArrayBuffer(PACKET_SIZE);
  #view = new DataView(this.#buffer);

  encode({ action, pointerId, x, y, pressure }) {
    this.#view.setUint8(0, VERSION);
    this.#view.setUint8(1, action);
    this.#view.setUint16(2, 0, true);
    this.#view.setUint32(4, pointerId >>> 0, true);
    this.#view.setFloat32(8, clamp01(x), true);
    this.#view.setFloat32(12, clamp01(y), true);
    this.#view.setFloat32(16, clamp01(pressure), true);
    return this.#buffer;
  }
}

function clamp01(value) {
  return Math.min(1, Math.max(0, Number.isFinite(value) ? value : 0));
}
