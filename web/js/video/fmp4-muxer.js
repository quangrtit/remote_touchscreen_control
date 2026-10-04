const TIMESCALE = 1_000_000;

export class Fmp4Muxer {
  #width;
  #height;
  #frameDuration;
  #sequence = 1;

  constructor(width, height, frameDurationMicroseconds = Math.round(TIMESCALE / 30)) {
    this.#width = width;
    this.#height = height;
    this.#frameDuration = Math.max(1, Math.round(frameDurationMicroseconds));
  }

  initializationSegment(sps, pps) {
    return concat(
      box("ftyp", ascii("isom"), u32(512), ascii("isomiso6avc1mp41")),
      this.#movieBox(sps, pps),
    );
  }

  mediaSegment(annexB, timestamp, keyFrame) {
    const sample = annexBToAvcc(annexB);
    const mdat = box("mdat", sample);
    const sampleFlags = keyFrame ? 0x02000000 : 0x01010000;
    let moof = this.#movieFragment(timestamp, sample.length, sampleFlags, 0);
    moof = this.#movieFragment(timestamp, sample.length, sampleFlags, moof.length + 8);
    this.#sequence += 1;
    return concat(moof, mdat);
  }

  #movieBox(sps, pps) {
    const matrix = concat(
      u32(0x00010000), u32(0), u32(0),
      u32(0), u32(0x00010000), u32(0),
      u32(0), u32(0), u32(0x40000000),
    );
    const mvhd = fullBox("mvhd", 0, 0, concat(
      u32(0), u32(0), u32(TIMESCALE), u32(0),
      u32(0x00010000), u16(0x0100), u16(0), u32(0), u32(0),
      matrix, zeros(24), u32(2),
    ));
    const tkhd = fullBox("tkhd", 0, 7, concat(
      u32(0), u32(0), u32(1), u32(0), u32(0),
      u32(0), u32(0), u16(0), u16(0), u16(0), u16(0),
      matrix, u32(this.#width << 16), u32(this.#height << 16),
    ));
    const mdhd = fullBox("mdhd", 0, 0, concat(
      u32(0), u32(0), u32(TIMESCALE), u32(0), u16(0x55c4), u16(0),
    ));
    const hdlr = fullBox("hdlr", 0, 0, concat(
      u32(0), ascii("vide"), zeros(12), ascii("VideoHandler\0"),
    ));
    const vmhd = fullBox("vmhd", 0, 1, zeros(8));
    const url = fullBox("url ", 0, 1, new Uint8Array());
    const dref = fullBox("dref", 0, 0, concat(u32(1), url));
    const dinf = box("dinf", dref);
    const stsd = fullBox("stsd", 0, 0, concat(u32(1), this.#avcSampleEntry(sps, pps)));
    const stbl = box("stbl", stsd, emptyTable("stts"), emptyTable("stsc"),
      fullBox("stsz", 0, 0, concat(u32(0), u32(0))), emptyTable("stco"));
    const minf = box("minf", vmhd, dinf, stbl);
    const mdia = box("mdia", mdhd, hdlr, minf);
    const trak = box("trak", tkhd, mdia);
    const trex = fullBox("trex", 0, 0, concat(
      u32(1), u32(1), u32(this.#frameDuration), u32(0), u32(0),
    ));
    return box("moov", mvhd, trak, box("mvex", trex));
  }

  #avcSampleEntry(sps, pps) {
    const compressorName = new Uint8Array(32);
    const avcC = box("avcC", concat(
      bytes(1, sps[1], sps[2], sps[3], 0xff, 0xe1),
      u16(sps.length), sps, bytes(1), u16(pps.length), pps,
    ));
    return box("avc1", concat(
      zeros(6), u16(1), zeros(16), u16(this.#width), u16(this.#height),
      u32(0x00480000), u32(0x00480000), u32(0), u16(1),
      compressorName, u16(0x0018), u16(0xffff), avcC,
    ));
  }

  #movieFragment(timestamp, sampleSize, sampleFlags, dataOffset) {
    const mfhd = fullBox("mfhd", 0, 0, u32(this.#sequence));
    const tfhd = fullBox("tfhd", 0, 0x020000, u32(1));
    const tfdt = fullBox("tfdt", 1, 0, u64(timestamp));
    const trun = fullBox("trun", 0, 0x000701, concat(
      u32(1), i32(dataOffset), u32(this.#frameDuration),
      u32(sampleSize), u32(sampleFlags),
    ));
    return box("moof", mfhd, box("traf", tfhd, tfdt, trun));
  }
}

export function parseParameterSets(annexB) {
  let sps = null;
  let pps = null;
  for (const nal of splitAnnexB(annexB)) {
    const type = nal[0] & 0x1f;
    if (type === 7) sps = nal;
    if (type === 8) pps = nal;
  }
  return { sps, pps };
}

export function codecFromSps(sps) {
  if (!sps || sps.length < 4) return "avc1.42E029";
  return `avc1.${hex(sps[1])}${hex(sps[2])}${hex(sps[3])}`;
}

function annexBToAvcc(data) {
  const nals = splitAnnexB(data);
  return concat(...nals.map((nal) => concat(u32(nal.length), nal)));
}

function splitAnnexB(data) {
  const starts = [];
  for (let index = 0; index + 3 < data.length; index += 1) {
    if (data[index] !== 0 || data[index + 1] !== 0) continue;
    if (data[index + 2] === 1) {
      starts.push({ start: index, nal: index + 3 });
      index += 2;
    } else if (data[index + 2] === 0 && data[index + 3] === 1) {
      starts.push({ start: index, nal: index + 4 });
      index += 3;
    }
  }
  const nals = [];
  for (let index = 0; index < starts.length; index += 1) {
    const end = index + 1 < starts.length ? starts[index + 1].start : data.length;
    if (end > starts[index].nal) nals.push(data.slice(starts[index].nal, end));
  }
  return nals;
}

function emptyTable(type) {
  return fullBox(type, 0, 0, u32(0));
}

function fullBox(type, version, flags, payload) {
  return box(type, bytes(version, (flags >>> 16) & 0xff, (flags >>> 8) & 0xff, flags & 0xff), payload);
}

function box(type, ...payloads) {
  const payload = concat(...payloads);
  return concat(u32(payload.length + 8), ascii(type), payload);
}

function concat(...arrays) {
  const length = arrays.reduce((total, array) => total + array.length, 0);
  const output = new Uint8Array(length);
  let offset = 0;
  for (const array of arrays) {
    output.set(array, offset);
    offset += array.length;
  }
  return output;
}

function ascii(text) {
  return Uint8Array.from(text, (character) => character.charCodeAt(0));
}

function bytes(...values) {
  return Uint8Array.from(values);
}

function zeros(length) {
  return new Uint8Array(length);
}

function u16(value) {
  const output = new Uint8Array(2);
  new DataView(output.buffer).setUint16(0, value);
  return output;
}

function u32(value) {
  const output = new Uint8Array(4);
  new DataView(output.buffer).setUint32(0, value >>> 0);
  return output;
}

function i32(value) {
  const output = new Uint8Array(4);
  new DataView(output.buffer).setInt32(0, value);
  return output;
}

function u64(value) {
  const output = new Uint8Array(8);
  new DataView(output.buffer).setBigUint64(0, BigInt(Math.max(0, value)));
  return output;
}

function hex(value) {
  return value.toString(16).padStart(2, "0").toUpperCase();
}
