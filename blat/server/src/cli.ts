#!/usr/bin/env node
// blat-server: find blat devices over Bluetooth LE and serve a web page to
// read and change their settings. Run with --help for the options.

import { homedir, hostname } from 'node:os';
import { join } from 'node:path';
import { parseArgs } from 'node:util';

// ble.ts loads noble (and opens the adapter) only when it first scans.
import { NobleLink, Scanner } from './ble.ts';
import { DeviceManager } from './devices.ts';
import { createApp } from './http.ts';
import { DEFAULT_SIM, SimDevice } from './sim.ts';
import { Store } from './store.ts';

const USAGE = `Usage: blat-server [options]

Finds blat devices over Bluetooth LE and serves a web page to read and change
their settings. Open the address it prints.

  --port <n>            HTTP port (default 8080)
  --host <address>      Address to listen on (default 127.0.0.1: this machine
                        only). Anyone who can reach the page can change the
                        settings of every device this server has paired with.
  --data <dir>          Where to keep remembered devices and cached schemas
                        (default ~/.blat-server)
  --name <text>         How the server names itself to devices
                        (default "blat-server on <hostname>")
  --scan <s>            Seconds per scan (default 15); also scans once at start
  --sim                 Use the simulated device from blat/firmware/test instead
                        of Bluetooth (build it: make -C blat/firmware/test sim).
                        Its pairing code shows in the log and on the page.
  --sim-path <path>     A sim_device binary somewhere else
  --dev                 Serve the page through Vite, with live reload
  -h, --help
`;

const { values: args } = parseArgs({
  options: {
    port: { type: 'string', default: '8080' },
    host: { type: 'string', default: '127.0.0.1' },
    data: { type: 'string', default: join(homedir(), '.blat-server') },
    name: { type: 'string', default: `blat-server on ${hostname()}` },
    scan: { type: 'string', default: '15' },
    sim: { type: 'boolean', default: false },
    'sim-path': { type: 'string', default: DEFAULT_SIM },
    dev: { type: 'boolean', default: false },
    help: { type: 'boolean', short: 'h', default: false },
  },
  allowPositionals: true,
});

if (args.help) {
  process.stdout.write(USAGE);
  process.exit(0);
}

const log = (text: string) => console.log(`${new Date().toISOString().slice(11, 19)} ${text}`);
const store = new Store(args.data);
const manager = new DeviceManager({ store, hostName: args.name, log, canScan: !args.sim });
let scanner: Scanner | null = null;
let sim: SimDevice | null = null;

if (args.sim) {
  sim = new SimDevice(args['sim-path'], {
    onCode: (code) => {
      if (code) log(`Simulated device shows pairing code ${code}`);
      manager.emit({ type: 'simCode', id: 'sim', code });
    },
    onInvoked: (action) => log(`Simulated device ran action ${action}`),
  });
  const device = sim;
  manager.add({ id: 'sim', name: 'Sim-0001 (simulated)', rssi: null, link: () => device.link() });
} else {
  scanner = new Scanner();
  scanner.onSeen = (seen) => {
    manager.add({ id: seen.id, name: seen.name, rssi: seen.rssi, link: () => new NobleLink(seen.peripheral) });
  };
  scanner.onScanning = (scanning) => manager.setScanning(scanning);
}

const server = await createApp({ manager, scanner, dev: args.dev, scanSeconds: Number(args.scan) });
server.listen(Number(args.port), args.host, () => {
  log(`blat-server: http://${args.host === '0.0.0.0' ? 'localhost' : args.host}:${args.port}/`);
  if (!['127.0.0.1', 'localhost', '::1'].includes(args.host)) {
    log('Listening beyond this machine: anyone who can reach it can change paired devices.');
  }
});

if (scanner) {
  scanner.start(Number(args.scan)).catch((err) => log(`Can't scan: ${err.message}`));
}

const shutdown = () => {
  manager.close();
  sim?.close();
  void scanner?.stop();
  server.close();
  process.exit(0);
};
process.on('SIGINT', shutdown);
process.on('SIGTERM', shutdown);
