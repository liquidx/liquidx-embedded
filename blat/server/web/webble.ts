// blat over Web Bluetooth: the browser is the GATT central. Chrome and Edge
// on desktop and Android; not Safari or Firefox.

import type { Link, LinkHandlers } from '../src/blat/link.ts';
import { CHANNEL, CHARACTERISTIC, SERVICE } from '../src/blat/wire.ts';

export const webBluetoothAvailable = () => typeof navigator !== 'undefined' && !!navigator.bluetooth;

/** Ask the user to pick a device advertising the blat service. Must be
 * called from a click (a user gesture). */
export async function chooseDevice(): Promise<BluetoothDevice> {
  if (!webBluetoothAvailable()) throw new Error('This browser has no Web Bluetooth. Use Chrome or Edge.');
  return navigator.bluetooth.requestDevice({ filters: [{ services: [SERVICE] }] });
}

type Chars = Record<keyof typeof CHARACTERISTIC, BluetoothRemoteGATTCharacteristic>;

export class WebBluetoothLink implements Link {
  readonly device: BluetoothDevice;
  #chars: Chars | null = null;
  #handlers: LinkHandlers | null = null;
  #onDisconnect = () => this.#closed();

  constructor(device: BluetoothDevice) {
    this.device = device;
  }

  async open(handlers: LinkHandlers): Promise<void> {
    this.#handlers = handlers;
    const gatt = this.device.gatt;
    if (!gatt) throw new Error('This device has no GATT server');
    this.device.addEventListener('gattserverdisconnected', this.#onDisconnect);
    const server = gatt.connected ? gatt : await gatt.connect();
    const service = await server.getPrimaryService(SERVICE);
    const get = (key: keyof typeof CHARACTERISTIC) => service.getCharacteristic(CHARACTERISTIC[key]);
    const chars: Chars = {
      info: await get('info'),
      request: await get('request'),
      reply: await get('reply'),
      data: await get('data'),
      dataOut: await get('dataOut'),
      event: await get('event'),
    };
    for (const [key, channel] of [['reply', CHANNEL.reply], ['dataOut', CHANNEL.dataOut], ['event', CHANNEL.event]] as const) {
      const c = chars[key];
      c.addEventListener('characteristicvaluechanged', () => {
        const v = c.value;
        if (v) this.#handlers?.onMessage(channel, new Uint8Array(v.buffer, v.byteOffset, v.byteLength).slice());
      });
      await c.startNotifications();
    }
    this.#chars = chars;
  }

  #closed() {
    this.device.removeEventListener('gattserverdisconnected', this.#onDisconnect);
    this.#chars = null;
    const h = this.#handlers;
    this.#handlers = null;
    h?.onClose();
  }

  async send(channel: number, data: Uint8Array): Promise<void> {
    const chars = this.#chars;
    if (!chars) throw new Error('Not connected');
    const bytes = new Uint8Array(data); // an ArrayBuffer-backed copy, as Web Bluetooth wants
    // Request is write-with-response; Data is write-without-response.
    if (channel === CHANNEL.request) await chars.request.writeValueWithResponse(bytes);
    else await chars.data.writeValueWithoutResponse(bytes);
  }

  async readInfo(): Promise<Uint8Array | null> {
    if (!this.#chars) throw new Error('Not connected');
    const v = await this.#chars.info.readValue();
    return new Uint8Array(v.buffer, v.byteOffset, v.byteLength).slice();
  }

  // Web Bluetooth doesn't expose the MTU; Chrome negotiates the largest the
  // device offers, and splits longer writes itself.
  maxMessage(): number {
    return 512;
  }

  close(): void {
    this.device.gatt?.disconnect();
  }
}
