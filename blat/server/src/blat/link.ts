// Links carry blat messages between a host and one device: GATT over BLE
// (../ble.ts with noble, ../../web/webble.ts with Web Bluetooth), or a byte
// stream with the stream framing (../stream.ts).

export interface LinkHandlers {
  /** A message from the device on Reply, DataOut or Event. */
  onMessage(channel: number, data: Uint8Array): void;
  onClose(): void;
}

export interface Link {
  open(handlers: LinkHandlers): Promise<void>;
  /** A message to the device on Request or Data. */
  send(channel: number, data: Uint8Array): Promise<void>;
  /** The Info characteristic, or null on transports that have none (the host
   * gets Info from hello instead). */
  readInfo(): Promise<Uint8Array | null>;
  /** The largest message the link carries now. */
  maxMessage(): number;
  /** Disconnect. onClose follows. */
  close(): void;
}
