"""Preserve bond authority when pinned NimBLE NVS initialization is incomplete.

The upstream void initializer only logs errors and leaves partial RAM tables.
Expose its completion result and avoid indexing empty bond arrays. Whole-file
fingerprints refuse source drift instead of silently applying a partial repair.
"""

import hashlib
from pathlib import Path

UPSTREAM_SHA256 = "e4d61d3b6403e263d40f2f498c4d6ac64ee1727ccd3fd6f040ba0e1908c26144"
PATCHED_SHA256 = "552d8ae874cc5d5fd70632ec7230f22b8bc9dcd51060e7759bb9acada121c3a8"
REPLACEMENTS = (
    ("""void ble_store_config_conf_init(void)
{
    int err;

    err = ble_nvs_restore_sec_keys();
    if (err != 0) {
        ESP_LOGE(LOG_TAG, "NVS operation failed, can't retrieve the bonding info");
    }
#if MYNEWT_VAL(BLE_HOST_BASED_PRIVACY)
    err = ble_nvs_restore_peer_records();
    if (err != 0) {
        ESP_LOGE(LOG_TAG, "NVS operation failed, can't retrieve the peer records");
    }
#endif
}""",
     """/* v1simple-nimble-bond-bootstrap-v1: publish complete NVS initialization. */
static int v1simple_bond_bootstrap_result = -1;

int v1simple_nimble_bond_bootstrap_status(void)
{
    return v1simple_bond_bootstrap_result;
}

void ble_store_config_conf_init(void)
{
    v1simple_bond_bootstrap_result = ble_nvs_restore_sec_keys();
    if (v1simple_bond_bootstrap_result != 0) {
        ESP_LOGE(LOG_TAG, "NVS operation failed, can't retrieve the bonding info");
        return;
    }
#if MYNEWT_VAL(BLE_HOST_BASED_PRIVACY)
    v1simple_bond_bootstrap_result = ble_nvs_restore_peer_records();
    if (v1simple_bond_bootstrap_result != 0) {
        ESP_LOGE(LOG_TAG, "NVS operation failed, can't retrieve the peer records");
    }
#endif
}"""),
    ("""    ble_store_config_our_bond_count = ble_store_config_our_secs[ble_store_config_num_our_secs - 1].bond_count;
    ble_store_config_peer_bond_count = ble_store_config_peer_secs[ble_store_config_num_peer_secs - 1].bond_count;""",
     """    ble_store_config_our_bond_count = ble_store_config_num_our_secs > 0
        ? ble_store_config_our_secs[ble_store_config_num_our_secs - 1].bond_count : 0;
    ble_store_config_peer_bond_count = ble_store_config_num_peer_secs > 0
        ? ble_store_config_peer_secs[ble_store_config_num_peer_secs - 1].bond_count : 0;"""),
)


def patch_text(text: str) -> str:
    actual = hashlib.sha256(text.encode("utf-8")).hexdigest()
    if actual == PATCHED_SHA256:
        return text
    if actual != UPSTREAM_SHA256:
        raise RuntimeError("NimBLE bond bootstrap source does not match pinned 2.5.1")
    for upstream, repaired in REPLACEMENTS:
        if text.count(upstream) != 1:
            raise RuntimeError("NimBLE bond bootstrap patch site is not unique")
        text = text.replace(upstream, repaired, 1)
    if hashlib.sha256(text.encode("utf-8")).hexdigest() != PATCHED_SHA256:
        raise RuntimeError("NimBLE bond bootstrap generated source identity mismatch")
    return text


if "Import" in globals():
    Import("env")  # noqa: F821
    source = (Path(env.subst("$PROJECT_LIBDEPS_DIR")) / env["PIOENV"] /  # noqa: F821
              "NimBLE-Arduino/src/nimble/nimble/host/store/config/src/ble_store_nvs.c")
    try:
        original = source.read_text(encoding="utf-8")
        repaired = patch_text(original)
        if repaired != original:
            source.write_text(repaired, encoding="utf-8")
        print("[patch_nimble_bond_bootstrap] " + ("applied" if repaired != original else "already applied"))
    except (OSError, RuntimeError) as error:
        print(f"[patch_nimble_bond_bootstrap] ERROR: {error}")
        env.Exit(1)  # noqa: F821
