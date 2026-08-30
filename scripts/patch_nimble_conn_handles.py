"""Patch NimBLE-Arduino 2.5.1's raw connection-handle array indexing.

HCI handles are not bounded by the configured simultaneous-connection count.
Keep connect-delivery state on the connection object and bounds-check the
optional data-length caches.  Fail closed if the pinned source changes.
"""

Import("env")  # noqa: F821  (SCons construction environment)

from pathlib import Path


PATCHES = {
    "ble_hs_conn_priv.h": (
        "v1simple-nimble-conn-handle-patch-v1: per-connection flag",
        ((
            "#define BLE_HS_CONN_F_TX_FRAG       0x04 /* Cur ACL packet partially txed. */",
            "#define BLE_HS_CONN_F_TX_FRAG       0x04 /* Cur ACL packet partially txed. */\n"
            "#define BLE_HS_CONN_F_CONNECT_EVENT 0x08 /* v1simple-nimble-conn-handle-patch-v1: per-connection flag */",
        ),),
    ),
    "ble_gap.c": (
        "v1simple-nimble-conn-handle-patch-v1: no raw-handle state array",
        (
            (
                "int slave_conn[MYNEWT_VAL(BLE_MAX_CONNECTIONS) + 1];",
                "/* v1simple-nimble-conn-handle-patch-v1: no raw-handle state array */",
            ),
            (
                """    struct ble_gap_event event;
    struct ble_hs_conn *conn;
    bool send = 1;""",
                """    struct ble_gap_event event;
    ble_hs_conn_flags_t conn_flags;
    bool send = 1;""",
            ),
            (
                """    ble_hs_lock();
    conn = ble_hs_conn_find(conn_handle);
    ble_hs_unlock();

    // Send disconnect event in slave role if connect was sent
    if ((conn != NULL) &&  !(conn->bhc_flags & BLE_HS_CONN_F_MASTER)) {
        if (slave_conn[conn_handle]) {
            slave_conn[conn_handle] = 0;
\t} else {
\t    send = 0;
\t}
    }""",
                """    rc = ble_hs_atomic_conn_flags(conn_handle, &conn_flags);

    // Send disconnect event in slave role if connect was sent
    if (rc == 0 && !(conn_flags & BLE_HS_CONN_F_MASTER) &&
            !(conn_flags & BLE_HS_CONN_F_CONNECT_EVENT)) {
        send = 0;
    }""",
            ),
            (
                """    g_max_tx_time[conn_handle] = 0;
    g_max_rx_time[conn_handle] = 0;
    g_max_tx_octets[conn_handle] = 0;
    g_max_rx_octets[conn_handle] = 0;""",
                """    if (conn_handle <= MYNEWT_VAL(BLE_MAX_CONNECTIONS)) {
        g_max_tx_time[conn_handle] = 0;
        g_max_rx_time[conn_handle] = 0;
        g_max_tx_octets[conn_handle] = 0;
        g_max_rx_octets[conn_handle] = 0;
    }""",
            ),
            (
                """\tif (conn != NULL) {
            ble_gap_event_connect_call(ev->conn_handle, ev->status);
            slave_conn[ev->conn_handle] = 1;
        }""",
                """\tif ((conn != NULL) &&
                ble_hs_atomic_conn_set_flags(le16toh(ev->conn_handle),
                                             BLE_HS_CONN_F_CONNECT_EVENT, 1) == 0) {
            ble_gap_event_connect_call(ev->conn_handle, ev->status);
        }""",
            ),
            (
                """    g_max_tx_octets[conn_handle] = event.data_len_chg.max_tx_octets;
    g_max_rx_octets[conn_handle] = event.data_len_chg.max_rx_octets;
    g_max_tx_time[conn_handle] = event.data_len_chg.max_tx_time;
    g_max_rx_time[conn_handle] = event.data_len_chg.max_rx_time;""",
                """    if (conn_handle <= MYNEWT_VAL(BLE_MAX_CONNECTIONS)) {
        g_max_tx_octets[conn_handle] = event.data_len_chg.max_tx_octets;
        g_max_rx_octets[conn_handle] = event.data_len_chg.max_rx_octets;
        g_max_tx_time[conn_handle] = event.data_len_chg.max_tx_time;
        g_max_rx_time[conn_handle] = event.data_len_chg.max_rx_time;
    }""",
            ),
            (
                "    if (g_max_tx_time[conn_handle] == tx_time && g_max_tx_octets[conn_handle] == tx_octets) {",
                "    if (conn_handle <= MYNEWT_VAL(BLE_MAX_CONNECTIONS) &&\n"
                "            g_max_tx_time[conn_handle] == tx_time &&\n"
                "            g_max_tx_octets[conn_handle] == tx_octets) {",
            ),
        ),
    ),
    "ble_hs_hci_evt.c": (
        "v1simple-nimble-conn-handle-patch-v1: state is on ble_hs_conn",
        (
            (
                "extern int slave_conn[MYNEWT_VAL(BLE_MAX_CONNECTIONS) + 1];",
                "/* v1simple-nimble-conn-handle-patch-v1: state is on ble_hs_conn */",
            ),
            (
                """    const struct ble_hci_ev_disconn_cmp *ev = data;
    const struct ble_hs_conn *conn;""",
                """    const struct ble_hci_ev_disconn_cmp *ev = data;
    const struct ble_hs_conn *conn;
    ble_hs_conn_flags_t conn_flags = 0;
    bool conn_found;""",
            ),
            (
                """    conn = ble_hs_conn_find(le16toh(ev->conn_handle));
    if (conn != NULL) {
        ble_hs_hci_add_avail_pkts(conn->bhc_outstanding_pkts);
    }
    ble_hs_unlock();""",
                """    conn = ble_hs_conn_find(le16toh(ev->conn_handle));
    conn_found = conn != NULL;
    if (conn_found) {
        ble_hs_hci_add_avail_pkts(conn->bhc_outstanding_pkts);
        conn_flags = conn->bhc_flags;
    }
    ble_hs_unlock();""",
            ),
            (
                """#if MYNEWT_VAL(BLE_ENABLE_CONN_REATTEMPT)
    if (conn) {""",
                """#if MYNEWT_VAL(BLE_ENABLE_CONN_REATTEMPT)
    if (conn_found) {""",
            ),
            (
                "        if ((conn->bhc_flags & BLE_HS_CONN_F_MASTER) && \\",
                "        if ((conn_flags & BLE_HS_CONN_F_MASTER) && \\",
            ),
            (
                "\telse if (!(conn->bhc_flags & BLE_HS_CONN_F_MASTER) && \\",
                "\telse if (!(conn_flags & BLE_HS_CONN_F_MASTER) && \\",
            ),
            (
                "\t\t(!slave_conn[ev->conn_handle] && ev->reason == BLE_ERR_CONN_SPVN_TMO))) { //slave",
                "\t\t(!(conn_flags & BLE_HS_CONN_F_CONNECT_EVENT) &&\n"
                "\t\t ev->reason == BLE_ERR_CONN_SPVN_TMO))) { //slave",
            ),
        ),
    ),
}


def fail(message: str) -> None:
    print(f"[patch_nimble_conn_handles] ERROR: {message}")
    env.Exit(1)


root = (Path(env.subst("$PROJECT_LIBDEPS_DIR")) / env["PIOENV"] /
        "NimBLE-Arduino" / "src" / "nimble" / "nimble" / "host" / "src")
updates = {}

for filename, (marker, replacements) in PATCHES.items():
    source = root / filename
    if not source.exists():
        fail(f"{source} not found; the pinned NimBLE source is required")

    text = source.read_text(encoding="utf-8")
    if marker in text:
        if any(patched not in text for _, patched in replacements):
            fail(f"{filename} carries an incomplete connection-handle patch")
        continue

    for upstream, _ in replacements:
        if text.count(upstream) != 1:
            fail(f"{filename} does not match pinned NimBLE-Arduino 2.5.1")
    for upstream, patched in replacements:
        text = text.replace(upstream, patched, 1)
    updates[source] = text

for source, text in updates.items():
    source.write_text(text, encoding="utf-8")

print("[patch_nimble_conn_handles] " + ("applied" if updates else "already applied"))
