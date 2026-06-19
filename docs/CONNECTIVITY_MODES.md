# Connectivity Modes

V1 Simple has an explicit operating-mode choice on the Dashboard page. The
Dashboard is reached from maintenance mode, and the saved choice applies on the
next normal boot. Neither mode is "active" while the maintenance Web UI is open:
maintenance boot intentionally skips BLE/V1/OBD runtime work and starts only the
WiFi/web surface.

The two normal-runtime choices are intentionally mutually exclusive because the
ESP32-S3 BLE radio should not be asked to sustain V1 central, phone-proxy
peripheral, and OBD central work at the same time.

## OBD / Standalone

Use this when V1 Simple should own local V1 features.

- BLE proxy is disabled.
- OBD is enabled and can provide vehicle speed when an adapter is configured.
- Local V1 writes are allowed: speed mute, profile pushes, local volume/mode
  writes, display dark-mode writes, and local mute writes.
- Display rendering and SD logging behave normally. WiFi and the Web UI remain
  off during normal drive runtime; use maintenance mode for configuration and
  logs.

## Proxy / App

Use this when a companion phone app should be the authority.

- OBD is disabled and any active OBD scan/connect/poll work is dropped if a
  proxy client is connected.
- The proxy relays raw V1 bytes. V1-to-phone notifications are copied from the
  upstream V1 packet unchanged, and phone-to-V1 writes are forwarded unchanged.
- V1 Simple display rendering and SD logging remain active.
- Local V1 Simple write features are suppressed while a proxy client is
  connected. The phone app is trusted to manage low-speed muting and
  other V1-control writes.
- Proxy mode has no local announcement path to suppress.
- WiFi and the Web UI remain off during normal drive runtime; use maintenance
  mode for configuration and logs.
- Advertising remains available for the drive once the V1 settles. It starts at
  a fast discovery cadence for app launch, downshifts to a slower background
  cadence if no app connects, and briefly returns to fast cadence after a phone
  disconnect so the app can reconnect.

## Selection and migration rules

- Dashboard `OBD / Standalone` first disables proxy, then enables OBD.
- Dashboard `Proxy / App` first disables OBD, then enables proxy.
- The backend enforces the same mutual exclusion for direct API calls:
  enabling `proxy_ble` disables `obdEnabled`; enabling OBD disables `proxyBLE`.
- Legacy settings or backups that contain both enabled are healed on load/restore
  by keeping OBD and disabling proxy. OBD wins because it was the explicit
  opt-in setting while proxy historically defaulted on.

## Runtime safety behavior

- Proxy advertising policy is gated by the explicit `proxy_ble` setting.
- If a proxy client connects through any path, OBD immediately stops scans,
  cancels pending connects, disconnects the OBD BLE client if active, clears
  transient OBD state, and returns to idle.
- `proxyOpenWindowMs` is a passive/fallback timing knob. Explicit Proxy / App
  mode enforces a 90-second minimum open window even if an older setting has the
  window clamped down near the 1-second minimum.
- When proxy mode is turned off at runtime, proxy advertising is stopped,
  connected proxy phones are disconnected, and proxy queues are released.

## Maintenance-mode security model

The maintenance web server is reachable only after an explicit maintenance boot:
normal drive runtime keeps WiFi and the HTTP API off. The WPA2 AP password (or a
saved STA network's own security) is the app-layer security boundary; the HTTP
routes do not add sessions or per-request authentication. Mutating API POST
routes require the bundled UI's `X-V1Simple-Request: maintenance-ui` header so
ordinary cross-origin forms cannot trigger maintenance writes. Change the
default AP password before using maintenance mode in any shared RF environment.
Treat backup exports as sensitive when credential export is explicitly
requested, because they can include saved WiFi passwords.
