#!/usr/bin/env python3
"""Compile and run the real replay producer against the real firmware parser."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
SWIFT_SOURCE = ROOT / "tools" / "v1replay" / "Sources" / "v1replay" / "V1Protocol.swift"

SWIFT_PRODUCER_SOURCE = r"""
import Foundation

private func packetHex(_ packet: [UInt8]) -> String {
    packet.map { String(format: "%02X", $0) }.joined()
}

private func emit(_ fields: [String]) {
    print(fields.joined(separator: "\t"))
}

@main
struct ProtocolContractProducer {
    static func main() {
        let bands: [(V1.Band, UInt16)] = [
            (.laser, 0),
            (.ka, 34_700),
            (.k, 24_150),
            (.x, 10_525),
            (.ku, 13_450),
        ]
        let directions: [(V1.Direction, Int)] = [
            (.front, 1),
            (.side, 2),
            (.rear, 4),
        ]

        for (band, frequency) in bands {
            for (direction, decodedDirection) in directions {
                for bars in 0...8 {
                    for muted in [false, true] {
                        for blinkArrow in [false, true] {
                            let frame = V1.DisplayFrame.alerting(
                                bars: bars,
                                band: band,
                                direction: direction,
                                bogeyCount: 1,
                                muted: muted,
                                volume: 0x40,
                                displayOn: true,
                                blinkArrow: blinkArrow
                            )
                            let expectedBand = band.mask == V1.Band.ku.mask ? 0 : Int(band.mask)
                            let expectedMuted = muted || band.mask == V1.Band.ku.mask
                            emit([
                                "display",
                                band.name,
                                String(expectedBand),
                                String(decodedDirection),
                                String(bars),
                                expectedMuted ? "1" : "0",
                                muted ? "1" : "0",
                                blinkArrow ? String(direction.rawValue) : "0",
                                packetHex(frame.packet(header: .broadcastInformation, checksum: true)),
                            ])
                        }
                    }

                    let row = V1.AlertRow.single(
                        bars: bars,
                        band: band,
                        direction: direction,
                        frequencyMHz: frequency,
                        priority: true
                    )
                    let expectedBars = band.mask == V1.Band.laser.mask ? 8 : bars
                    emit([
                        "alert",
                        band.name,
                        String(band.mask),
                        String(decodedDirection),
                        String(expectedBars),
                        String(frequency),
                        packetHex(row.packet(header: .broadcastInformation, checksum: true)),
                    ])
                }
            }
        }
    }
}
""".lstrip()

CPP_CONSUMER_SOURCE = r"""
#include <Arduino.h>

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "packet_parser.h"
#include "packet_parser.cpp"
#include "packet_parser_alerts.cpp"

#include <cstdint>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::vector<std::string> splitTabs(const std::string& line) {
    std::vector<std::string> fields;
    std::stringstream stream(line);
    std::string field;
    while (std::getline(stream, field, '\t')) {
        fields.push_back(field);
    }
    return fields;
}

std::vector<uint8_t> decodeHex(const std::string& value) {
    if (value.size() % 2 != 0) {
        throw std::runtime_error("odd-length packet hex");
    }
    std::vector<uint8_t> bytes;
    bytes.reserve(value.size() / 2);
    for (size_t index = 0; index < value.size(); index += 2) {
        bytes.push_back(static_cast<uint8_t>(std::stoul(value.substr(index, 2), nullptr, 16)));
    }
    return bytes;
}

int integer(const std::string& value) {
    return std::stoi(value);
}

bool verifyDisplay(const std::vector<std::string>& fields, std::string& failure) {
    if (fields.size() != 9) {
        failure = "display row has " + std::to_string(fields.size()) + " fields";
        return false;
    }
    const auto packet = decodeHex(fields[8]);
    PacketParser parser;
    if (!parser.parse(packet.data(), packet.size(), 1000) || !parser.parse(packet.data(), packet.size(), 1001)) {
        failure = "firmware rejected display packet";
        return false;
    }
    const DisplayState& state = parser.getDisplayState();
    const bool matches =
        state.activeBands == integer(fields[2]) && static_cast<int>(state.arrows) == integer(fields[3]) &&
        state.signalBars == integer(fields[4]) && state.muted == (integer(fields[5]) != 0) &&
        state.softMuted == (integer(fields[6]) != 0) && state.flashBits == integer(fields[7]) &&
        state.bandFlashBits == 0 && state.systemStatus && state.mainVolume == 4 && state.muteVolume == 0 &&
        state.hasVolumeData;
    if (!matches) {
        failure = "display semantics diverged for " + fields[1];
    }
    return matches;
}

bool verifyAlert(const std::vector<std::string>& fields, std::string& failure) {
    if (fields.size() != 7) {
        failure = "alert row has " + std::to_string(fields.size()) + " fields";
        return false;
    }
    const auto packet = decodeHex(fields[6]);
    PacketParser parser;
    if (!parser.parse(packet.data(), packet.size(), 1000) || parser.getAlertCount() != 1) {
        failure = "firmware rejected alert packet";
        return false;
    }
    const AlertData& alert = parser.getAllAlerts()[0];
    const int selectedBars = alert.direction == DIR_REAR ? alert.rearStrength : alert.frontStrength;
    const bool matches =
        static_cast<int>(alert.band) == integer(fields[2]) &&
        static_cast<int>(alert.direction) == integer(fields[3]) && selectedBars == integer(fields[4]) &&
        static_cast<int>(alert.frequency) == integer(fields[5]) && alert.isPriority && alert.isValid;
    if (!matches) {
        failure = "alert semantics diverged for " + fields[1];
    }
    return matches;
}

} // namespace

int main() {
    size_t cases = 0;
    size_t failures = 0;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) {
            continue;
        }
        ++cases;
        try {
            const auto fields = splitTabs(line);
            std::string failure;
            const bool valid = !fields.empty() &&
                               ((fields[0] == "display" && verifyDisplay(fields, failure)) ||
                                (fields[0] == "alert" && verifyAlert(fields, failure)));
            if (!valid) {
                ++failures;
                std::cerr << "[v1replay-protocol] case " << cases << ": "
                          << (failure.empty() ? "unknown row kind" : failure) << '\n';
            }
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[v1replay-protocol] case " << cases << ": " << error.what() << '\n';
        }
    }

    if (cases == 0) {
        std::cerr << "[v1replay-protocol] producer emitted no cases\n";
        return 1;
    }
    if (failures != 0) {
        std::cerr << "[v1replay-protocol] " << failures << " of " << cases << " cases failed\n";
        return 1;
    }
    std::cout << "[v1replay-protocol] " << cases
              << " actual Swift producer -> firmware PacketParser cases passed\n";
    return 0;
}
""".lstrip()

ARDUINO_JSON_STUB = """\
#pragma once

// The parser needs perf hook declarations; the transitive snapshot header does
// not use ArduinoJson types in this host-only contract build.
"""


def run(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, cwd=ROOT, text=True, check=False, **kwargs)


def main() -> int:
    swiftc = shutil.which("swiftc")
    cxx = shutil.which(os.environ.get("CXX", "c++"))
    if not swiftc or not cxx:
        missing = "swiftc" if not swiftc else "C++ compiler"
        print(f"[v1replay-protocol] missing {missing}", file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory(prefix="v1replay-protocol-") as temporary:
        build_dir = Path(temporary)
        module_cache = build_dir / "module-cache"
        module_cache.mkdir()
        producer = build_dir / "protocol-producer"
        consumer = build_dir / "protocol-consumer"
        swift_producer = build_dir / "ProtocolContractProducer.swift"
        cpp_consumer = build_dir / "protocol_contract_consumer.cpp"
        stub_dir = build_dir / "stubs"
        stub_dir.mkdir()
        swift_producer.write_text(SWIFT_PRODUCER_SOURCE, encoding="utf-8")
        cpp_consumer.write_text(CPP_CONSUMER_SOURCE, encoding="utf-8")
        (stub_dir / "ArduinoJson.h").write_text(ARDUINO_JSON_STUB, encoding="utf-8")

        environment = os.environ.copy()
        environment["CLANG_MODULE_CACHE_PATH"] = str(module_cache)
        developer = Path("/Applications/Xcode.app/Contents/Developer")
        if sys.platform == "darwin" and developer.is_dir():
            environment["DEVELOPER_DIR"] = str(developer)

        compiled_producer = run(
            [
                swiftc,
                "-module-cache-path",
                str(module_cache),
                str(SWIFT_SOURCE),
                str(swift_producer),
                "-o",
                str(producer),
            ],
            capture_output=True,
            env=environment,
        )
        if compiled_producer.returncode != 0:
            print(compiled_producer.stdout + compiled_producer.stderr, file=sys.stderr)
            return compiled_producer.returncode

        compiled_consumer = run(
            [
                cxx,
                "-std=c++17",
                "-DUNIT_TEST=1",
                "-I",
                str(stub_dir),
                "-I",
                str(ROOT / "src"),
                "-I",
                str(ROOT / "test" / "mocks"),
                "-I",
                str(ROOT / "include"),
                str(cpp_consumer),
                "-o",
                str(consumer),
            ],
            capture_output=True,
        )
        if compiled_consumer.returncode != 0:
            print(compiled_consumer.stdout + compiled_consumer.stderr, file=sys.stderr)
            return compiled_consumer.returncode

        produced = run([str(producer)], capture_output=True, env=environment)
        if produced.returncode != 0:
            print(produced.stdout + produced.stderr, file=sys.stderr)
            return produced.returncode

        verified = run([str(consumer)], input=produced.stdout, capture_output=True)
        if verified.stdout:
            print(verified.stdout, end="")
        if verified.stderr:
            print(verified.stderr, file=sys.stderr, end="")
        return verified.returncode


if __name__ == "__main__":
    raise SystemExit(main())
