// Parsing the schema and Info (PROTOCOL.md#schema, #info).

import { ATTR, FLAG, HINT_NAME, INFO, Reader, TYPE, TYPE_NAME, type TypeName, type Value, decodeText, decodeValue, readTlvs, toHex } from './wire.ts';

export interface Option {
  value: number;
  label: string;
}

/** One control, as the device declared it. Plain data: it goes to the web
 * page as JSON too. */
export interface Control {
  id: number;
  key: string;
  type: TypeName;
  typeCode: number;
  parent: number; // 0: top level
  read: number; // access levels
  write: number; // 0 for read-only controls
  label: string;
  help: string;
  unit: string;
  hint: string;
  min?: number;
  max?: number;
  step?: number;
  scale: number;
  options: Option[];
  minLength?: number;
  maxLength?: number;
  default?: Value;
  readOnly: boolean;
  live: boolean;
  dynamic: boolean;
  restart: boolean;
  confirm: boolean;
  advanced: boolean;
  required: boolean;
  hidden: boolean;
}

export function parseSchema(blob: Uint8Array): Control[] {
  const r = new Reader(blob);
  const version = r.u8();
  if (version !== 1) throw new Error(`Unsupported schema version ${version}`);
  const count = r.u16();
  const controls: Control[] = [];
  for (let i = 0; i < count; i++) {
    const record = new Reader(r.take(r.u16()));
    const id = record.u16();
    const typeCode = record.u8();
    const access = record.u8();
    const parent = record.u16();
    const flags = record.u16();
    const c: Control = {
      id,
      key: '',
      type: TYPE_NAME[typeCode] ?? 'group',
      typeCode,
      parent,
      read: access & 0x0f,
      write: access >> 4,
      label: '',
      help: '',
      unit: '',
      hint: '',
      scale: 0,
      options: [],
      readOnly: !!(flags & FLAG.readOnly),
      live: !!(flags & FLAG.live),
      dynamic: !!(flags & FLAG.dynamic),
      restart: !!(flags & FLAG.restart),
      confirm: !!(flags & FLAG.confirm),
      advanced: !!(flags & FLAG.advanced),
      required: !!(flags & FLAG.required),
      hidden: !!(flags & FLAG.hidden),
    };
    for (const [tag, value] of readTlvs(record.rest())) {
      const v = new Reader(value);
      switch (tag) {
        case ATTR.key:
          c.key = decodeText(value);
          break;
        case ATTR.label:
          c.label = decodeText(value);
          break;
        case ATTR.help:
          c.help = decodeText(value);
          break;
        case ATTR.unit:
          c.unit = decodeText(value);
          break;
        case ATTR.range:
          c.min = v.i32();
          c.max = v.i32();
          c.step = v.i32();
          break;
        case ATTR.scale:
          c.scale = v.u8();
          break;
        case ATTR.option:
          c.options.push({ value: v.u16(), label: decodeText(v.rest()) });
          break;
        case ATTR.length:
          c.minLength = v.u8();
          c.maxLength = v.u8();
          break;
        case ATTR.hint:
          c.hint = HINT_NAME[v.u8()] ?? '';
          break;
        case ATTR.default:
          if (typeCode !== TYPE.secret) c.default = decodeValue(typeCode, v);
          break;
        default:
          break; // unknown attributes are skipped
      }
    }
    if (!c.label) c.label = c.key;
    controls.push(c);
  }
  return controls;
}

export interface Info {
  version: number;
  name: string;
  model: string;
  firmware: string;
  deviceId: string; // hex
  schemaCrc: number;
  schemaSize: number;
  chunk: number;
  window: number;
  maxFile: number;
  features: number;
  level: number;
  methods: number;
  digits: number;
}

export function parseInfo(bytes: Uint8Array): Info {
  const info: Info = {
    version: bytes[0] ?? 0,
    name: '',
    model: '',
    firmware: '',
    deviceId: '',
    schemaCrc: 0,
    schemaSize: 0,
    chunk: 20,
    window: 1,
    maxFile: 0,
    features: 0,
    level: 0,
    methods: 0,
    digits: 6,
  };
  for (const [tag, value] of readTlvs(bytes.subarray(1))) {
    const r = new Reader(value);
    switch (tag) {
      case INFO.name:
        info.name = decodeText(value);
        break;
      case INFO.model:
        info.model = decodeText(value);
        break;
      case INFO.firmware:
        info.firmware = decodeText(value);
        break;
      case INFO.deviceId:
        info.deviceId = toHex(value);
        break;
      case INFO.schema:
        info.schemaCrc = r.u32();
        info.schemaSize = r.u32();
        break;
      case INFO.limits:
        info.chunk = r.u16();
        info.window = r.u16();
        info.maxFile = r.u32();
        break;
      case INFO.features:
        info.features = r.u32();
        break;
      case INFO.auth:
        info.level = r.u8();
        info.methods = r.u8();
        info.digits = r.u8();
        break;
      default:
        break;
    }
  }
  return info;
}
