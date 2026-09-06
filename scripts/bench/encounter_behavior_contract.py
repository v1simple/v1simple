"""Recorded-source explanations for the external V1 display comparison.

This is a source map, not a pixel reader or response-time policy. Rules are
applied only to the reviewed owning files in the recorded firmware revision.
New firmware can still be observed when a source rule needs review; the old
explanation must not silently become a claim about changed implementation.
"""
from __future__ import annotations

from copy import deepcopy
import hashlib
import re
import subprocess


# These owning files were reviewed together. Hashes bind the descriptions to
# implementation, not to the tooling commit or whichever checkout is open.
_SOURCES = {
    "src/packet_parser.cpp": "360441d6cdb2eea4f39ba412ae7f492adf877ec1c05b942f8e90c7aa419852ac",
    "src/packet_parser_alerts.cpp": "4963cc3e1213db986fcc956d86bd391911ce29dbba0c8b681ec519147e87ef92",
    "src/display.h": "32d955848c3a80fb127a1ccda0efd7b7f14353fa77c10acc21421d9d5128652c",
    "src/display_frequency.cpp": "c1c0d897d8ff4fa19b29faca70805a1fb684076e7726a72596bad0d8df1f03da",
    "include/color_themes.h": "bd0448752777d5faaf9c329de3ac2138fb12333f9232841f30e4544834eea012",
    "include/display_palette.h": "e9bb062aa27a6af0f280a5db8a0ed17965a99ad408b593a5c12605f80862b6fa",
    "src/display_bands.cpp": "68a49007928004f9b7302d909c2d36bf5cc798fbc19b8015714e4d36fabb1afd",
    "src/display_arrow.cpp": "a99be71867e8f1a887795783c61dd2f1a5254d9a1220e6aa641bf7b45d51cf8c",
    "src/display_cards.cpp": "0e8aa435f9607b77ec755f365e25a1a85f7573d9d69c1ef634654f0a91938ce3",
    "src/display_update.cpp": "08a6ae93e006a5c700ab7420a6d93117ddb4538a83ee53f8594ec0e6df8f8513",
    "src/display_top_counter.cpp": "2ee97c61e61c542d4425c55f4db1915894f3669c32530faff6c71abe45ef8b24",
    "include/display_visual_contract.h": "2344aa8e3377af216f485418ec7959486daf2cfcafb0a5c6b6a86c359353e32c",
    "src/modules/display/display_pipeline_module.cpp": "2df69abf2149f32326be44166e198b735422c9228fd7ae2ca8917d582b0c15f9",
    "src/modules/display/render_frame_composer.cpp": "fc8b9b7863b319dfccce5103d8540e4bc2d412e08f46e28241f9c9e25383a322",
    "src/modules/display/display_orchestration_module.cpp": "aef2e32f8d21493475f6ea614a1ca234898187a6baaf642b9a7f62f6789b2772",
    "src/modules/alert_persistence/alert_persistence_module.cpp": "daa2492f60be023e8760e7d9b575a896319f2f2bde9c5e2d8370a362f9a4b2d9",
    "src/drive_runtime.cpp": "8c9f4e9bd646c53a79d31bc00c82c0fe4b857a40fda243c5b043c69681fc1c36",
    "src/modules/ble/connection_state_cadence_module.cpp": "1d2f38d2088acb075b367d72fe6ad48521cd587674d87e553b15d290aa876dfb",
}


def _rule(fields, statement, locations, repair_direction):
    return {"fields": fields, "statement": statement, "locations": locations,
            "repair_direction": repair_direction}


_RULES = {
    "counter_glyph": _rule(["counter_glyph"],
        "The top glyph follows the two InfDisplayData counter images. They are phases of one glyph, "
        "not two digits. Counter, bands and arrows share the local blink phase.",
        [("src/packet_parser.cpp", 312, 324), ("src/display_update.cpp", 570, 579),
         ("src/display_top_counter.cpp", 452, 454)],
        "Compare the decoded counter images with drawStatusStrip and the top-counter renderer; "
        "a phase disagreement is different from a wrong literal glyph."),
    "primary_frequency": _rule(["primary_frequency"],
        "The usable priority row is selected by its priority flag. Its MHz integer renders with "
        "three decimal GHz digits. Ordinary resting dispatch calls the zero-frequency renderer, "
        "which requests --.--- in dark gray, subject to the zero-volume warning. A reader's "
        "no-bright-glyph observation does not establish that these dark strokes are absent.",
        [("src/packet_parser_alerts.cpp", 397, 435), ("src/display_frequency.cpp", 124, 131),
         ("src/display_update.cpp", 342, 348), ("src/display_update.cpp", 684, 702),
         ("src/display_update.cpp", 856, 858),
         ("src/display_frequency.cpp", 139, 145), ("include/display_palette.h", 19, 21),
         ("include/color_themes.h", 28, 33)],
        "If the wrong threat is primary, inspect priority selection and frame composition; if the "
        "right threat has wrong digits, inspect frequency drawing, cache invalidation and physical delivery."),
    "active_bands": _rule(["active_bands"],
        "Active bands come from InfDisplayData image1, gated by systemStatus. Image1 bits absent "
        "from image2 blink off together. They are not recomputed from alert-row count or RSSI.",
        [("src/packet_parser.cpp", 357, 375), ("src/packet_parser.cpp", 385, 394),
         ("src/display_bands.cpp", 41, 51)],
        "Compare parsed band planes with effectiveBandMask and the painted band region. Check the "
        "shared phase before treating a blink-off image as a missing band."),
    "main_arrows": _rule(["main_arrows"],
        "Arrows follow the InfDisplayData direction set, optionally intersected with the priority "
        "direction by priorityArrowOnly. Inactive arrows are dim grey shapes; active arrows use the "
        "configured or muted color; an active arrow in blink-off phase is erased to background.",
        [("src/packet_parser.cpp", 377, 394), ("src/display_update.cpp", 834, 844),
         ("src/display_arrow.cpp", 45, 60),
         ("src/display_arrow.cpp", 103, 113)],
        "Separate active color, grey resting shape and blink-off erase. For a retained outgoing "
        "color inspect direction/phase invalidation and the full-versus-partial display dispatch."),
    "main_bars": _rule(["main_bars"],
        "Main strength is the InfDisplayData LED bitmap decoded on the 0..8 V1 scale, then clamped "
        "to six visible bars. Per-alert RSSI must not be substituted for this value.",
        [("src/packet_parser.cpp", 427, 434), ("src/display_bands.cpp", 275, 280),
         ("src/modules/display/render_frame_composer.cpp", 142, 149)],
        "Check the display bitmap decode and six-cell clamp, then the bar repaint/full-flush path."),
    "secondary": _rule(["secondary"],
        "Up to two non-priority live alerts render as associated band/frequency/direction/strength "
        "cards. Card strength projects directional RSSI bars onto six cells. Identity tolerance is "
        "2 MHz and continuity/redraw tolerance is 5 MHz; the ordinary distinct-alert test excludes overlaps.",
        [("include/display_visual_contract.h", 11, 25), ("src/packet_parser_alerts.cpp", 117, 145),
         ("src/display_cards.cpp", 29, 47), ("src/display_cards.cpp", 87, 107),
         ("src/display_cards.cpp", 268, 287)],
        "Preserve each card's field association. For an obsolete card inspect retirement state, "
        "card-region clear and physical delivery; for a missing live card inspect slot capacity and startup deferral."),
    "muted_badge": _rule(["muted_badge"],
        "The live MUTED badge and muted presentation require image1 bit4 in two consecutive "
        "display packets; one clear bit immediately releases it. auxData0 soft mute is separate. "
        "Resting mode intentionally hides MUTED.",
        [("src/packet_parser.cpp", 396, 412), ("src/display_update.cpp", 622, 634),
         ("src/display_update.cpp", 590, 593), ("src/display_top_counter.cpp", 485, 491)],
        "Compare the consecutive display mute bits with the badge and palette transition. "
        "Do not infer visible mute from audio soft mute or row flags."),
    "shared_blink": _rule(["counter_glyph", "active_bands", "main_arrows"],
        "The implementation advances one shared phase in exact 96 ms quanta of DUT millis(). "
        "The phase epoch is established by the first renderer entry. This specifies code cadence, "
        "not a host-to-screen acquisition deadline or an exposure phase.",
        [("src/display.h", 381, 400), ("src/display.h", 407, 430),
         ("src/modules/display/display_orchestration_module.cpp", 129, 145)],
        "Measure visible phase durations and shared-phase consistency. Locate missing refreshes "
        "or missed display writes using the renderer and lightweight refresh owner."),
    "retired_card": _rule(["secondary"],
        "The source admits a vanished previous priority as a grey card even at persistence zero. "
        "Zero is converted to 1 ms; the slot expires only on a subsequent card render after elapsed >1 ms. "
        "Thus initial outgoing-card presence is source-explained, while the configured steady target "
        "contains no retired card. Seconds-long visible retention is not explained by a 1 ms timer alone.",
        [("src/display_cards.cpp", 80, 84), ("src/display_cards.cpp", 107, 136),
         ("src/display_cards.cpp", 167, 172), ("src/display_cards.cpp", 311, 320)],
        "A minimal candidate is to prevent vanished-priority grace when persistence is zero and "
        "expire missing slots immediately in that setting. Preserve live-card admission and nonzero "
        "persistence. Verify the physical result: this source correction alone does not prove that "
        "isolated card clears reach the panel."),
    "physical_dispatch": _rule(["primary_frequency", "active_bands", "main_arrows", "main_bars", "secondary"],
        "The renderer marks a removed card's rectangle clear and consumes its dirty state after dispatch. "
        "Blink, painted bars and certain arrow changes force full pushes; an isolated card clear "
        "can use a partial push. Subsequent cache hits do not repaint an already-cleared card.",
        [("src/display_cards.cpp", 311, 325), ("src/display_update.cpp", 893, 924),
         ("src/display_update.cpp", 927, 933)],
        "A card persisting physically after the renderer clears its state points to a delivery/cache "
        "boundary to investigate. A later full push clearing it supports that hypothesis; camera "
        "evidence alone does not prove which SPI write failed."),
    "persistence_and_clear": _rule(["primary_frequency", "main_arrows", "main_bars", "secondary", "muted_badge"],
        "Live V1 presentation outranks persisted presentation. With persistence zero, no-alert input "
        "returns to resting. Nonzero primary persistence starts at the DUT-side clear and lasts "
        "until elapsed reaches the configured seconds; persisted mode uses grey prior frequency "
        "and direction, zero bars and no cards. Host acceptance does not locate that DUT timer exactly.",
        [("src/modules/display/display_pipeline_module.cpp", 256, 272),
         ("src/modules/display/render_frame_composer.cpp", 142, 174),
         ("src/modules/alert_persistence/alert_persistence_module.cpp", 48, 59),
         ("src/display_update.cpp", 745, 768)],
        "Separate primary persistence from live secondary retirement. Check the configured slot, "
        "persistence owner and mode transition; retain observed clear times without inventing DUT receipt."),
    "response_timing": _rule([], 
        "There is no 100 ms complete-display response requirement in this owning path. Parsed input "
        "runs the display pipeline directly. DISPLAY_UPDATE_MS gates connection-state service. "
        "Record host-input-to-camera times as observations; neither the macro nor 96 ms blink cadence "
        "makes a firmware-response pass/fail deadline.",
        [("src/drive_runtime.cpp", 574, 594), ("src/drive_runtime.cpp", 598, 611),
         ("src/modules/ble/connection_state_cadence_module.cpp", 24, 40)],
        "Use the observed acquisition, transition and clear sequence to locate the affected path. "
        "A measured interval becomes a timing defect only against an independently supported requirement."),
    "idle_volume_warning": _rule(["primary_frequency"],
        "At positive main volume, normal resting requests --.--- in PALETTE_GRAY (RGB565 0x1082), "
        "then draws bars/arrows, clears retired cards and dispatches the resting frame. At zero volume, a separate "
        "runtime warning can replace that region, depending on fresh BLE/proxy/speed-mute context. "
        "A dark-region reader result is not a measured absence of this faint placeholder.",
        [("src/modules/display/render_frame_composer.cpp", 165, 174),
         ("src/display_update.cpp", 342, 348), ("src/display_update.cpp", 639, 647),
         ("src/display_update.cpp", 684, 702), ("src/display_frequency.cpp", 124, 145),
         ("include/display_palette.h", 19, 21), ("include/color_themes.h", 28, 33)],
        "Establish the accepted main volume and a reader qualified for dark idle strokes before "
        "calling the placeholder missing. The numeric reader's brightness threshold cannot establish "
        "this failure. Zero-volume warning behavior separately requires its runtime context."),
    "connect_card_deferral": _rule(["secondary"],
        "The display pipeline can defer secondary cards during BLE connect-burst settling. "
        "This is a runtime condition, not a fixed camera deadline.",
        [("src/modules/display/display_pipeline_module.cpp", 411, 428)],
        "When live cards are missing at startup, inspect connect-burst settling before attributing "
        "their absence to card parsing or drawing. Persistent absence later is a separate observation."),
}


def _git(repo_root, *args):
    return subprocess.run(["git", "-C", str(repo_root), *args], capture_output=True,
                          check=True, timeout=15).stdout


def behavior_contract(repo_root, firmware_commit):
    """Return source-grounded rules for the recorded commit, never current HEAD.

    Unreviewed source does not discard physical observations or stop analysis.
    It withholds the affected implementation explanation until it is reviewed.
    No files, index, working tree, hardware or firmware are changed.
    """
    base = {"schema_version": 1, "comparison_key": "v1-ordinary-seven-fields-v1",
            "status": "SOURCE_UNAVAILABLE", "firmware_commit": None,
            "scope": "Ordinary V1 X/K/Ka, distinct alerts, at most two secondary cards; "
                     "bound stealth, priority-arrow and persistence settings.",
            "response_deadline_ms": None,
            "basis": "Recorded Git source explains expected behavior; physical camera observations "
                     "remain independent. Source correspondence is not DUT receipt or display delivery proof.",
            "sources": {}, "rules": {}, "field_rule_ids": {}}
    if not isinstance(firmware_commit, str) or not re.fullmatch(r"[0-9a-fA-F]{7,40}", firmware_commit):
        return {**base, "reason": "recorded firmware commit must be an unambiguous hexadecimal commit id"}
    try:
        commit = _git(repo_root, "rev-parse", "--verify", f"{firmware_commit}^{{commit}}").decode().strip()
    except (OSError, subprocess.SubprocessError, UnicodeError):
        return {**base, "reason": "recorded firmware commit is not available in this repository"}
    base["firmware_commit"] = commit
    contents = {}
    for path, reviewed_hash in _SOURCES.items():
        try:
            data = _git(repo_root, "show", f"{commit}:{path}")
            digest = hashlib.sha256(data).hexdigest()
            contents[path] = data.decode("utf-8").splitlines()
            state = "VERIFIED" if digest == reviewed_hash else "UNREVIEWED"
        except (OSError, subprocess.SubprocessError, UnicodeError):
            digest, state = None, "UNAVAILABLE"
        base["sources"][path] = {"sha256": digest, "reviewed_sha256": reviewed_hash,
                                  "status": state, "git_object": f"{commit}:{path}"}
    for rule_id, definition in _RULES.items():
        rule = deepcopy(definition)
        verified = all(base["sources"][path]["status"] == "VERIFIED"
                       for path, _, _ in definition["locations"])
        rule["status"] = "VERIFIED" if verified else "UNREVIEWED"
        if not verified:
            rule["statement"] = "Recorded owning source is changed or unavailable; this implementation rule needs review."
            rule["repair_direction"] = "Inspect the recorded source before applying the earlier implementation explanation."
        rule["locations"] = [{"path": path, "line_start": start if verified else None,
                              "line_end": end if verified else None,
                              "url": f"https://github.com/v1simple/v1simple/blob/{commit}/{path}"
                                     + (f"#L{start}-L{end}" if verified else ""),
                              "excerpt": "\n".join(contents[path][start - 1:end]) if verified else None}
                             for path, start, end in definition["locations"]]
        base["rules"][rule_id] = rule
        for field in rule["fields"]:
            base["field_rule_ids"].setdefault(field, []).append(rule_id)
    base["status"] = ("VERIFIED" if all(s["status"] == "VERIFIED" for s in base["sources"].values())
                      else "UNREVIEWED")
    return base
