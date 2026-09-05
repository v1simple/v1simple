import XCTest
@testable import v1replay

final class V1DisplayAlertContractTests: XCTestCase {

    func testReaderQualificationLeavesTheNormalResolvedStimulusUnchanged() throws {
        // Retained before adding the separate reader exercise; covers every
        // normal alert, mute, volume, mode, timing and blink value.
        XCTAssertEqual(sha256Hex(try BenchScenario.make().resolvedScenarioEvidenceData()),
                       "144b04f4cced4461578168c9b162e97a02bf34973e9918c73b4a972888935f6f")
    }

    func testReaderQualificationEmitsEveryBandInEveryDisplayRole() throws {
        let encounter = BenchScenario.makeReaderQualification()
        XCTAssertEqual(encounter.samples.count, 792)
        XCTAssertEqual(encounter.duration, 263.0 + 2.0 / 3.0, accuracy: 0.000_001)
        XCTAssertLessThan(encounter.duration, 276)
        XCTAssertTrue(encounter.samples.prefix(12).allSatisfy { $0.alerts.isEmpty })
        XCTAssertTrue(encounter.samples.suffix(60).allSatisfy { $0.alerts.isEmpty })
        let state = V1.Session.ControlState(mode: .advancedLogic, mainVolume: 4,
                                           mutedVolume: 0, savedMainVolume: 4,
                                           savedMutedVolume: 0)
        let identities: [UInt8: Int] = [0x08: 10_525, 0x04: 24_150, 0x02: 34_700]
        var roles = Set<String>()
        var directions = Set<UInt8>()
        var phases: [String: Int] = [:]
        var previousPackets: [[UInt8]]?
        for (index, sample) in encounter.samples.enumerated() {
            XCTAssertEqual(sample.sourceIndex, index)
            XCTAssertEqual(sample.offset, Double(index) / 3.0, accuracy: 0.000_001)
            let plan = V1.PlaybackPacketPlan(
                sample: sample, controlState: state, displayOn: true, muted: sample.muted,
                blinkBogey: false, blinkArrow: sample.scenarioArrowBlink)
            if (12..<732).contains(index) {
                let relative = index - 12
                let step = (relative / 6) % 5
                let rowCount = step == 0 ? 1 : step == 1 ? 2 : 3
                let rows = try plan.alertTablePackets.map(IndependentFrame.decode)
                XCTAssertEqual(rows.count, rowCount)
                XCTAssertEqual(sample.muted, step == 3)
                XCTAssertEqual(sample.scenarioArrowBlink, rowCount > 1)
                XCTAssertEqual(rows.map { $0.payload[6] & 0x80 },
                               [0x80] + Array(repeating: UInt8(0), count: rowCount - 1))
                var frequencies = Set<Int>()
                for (slot, row) in rows.enumerated() {
                    XCTAssertEqual(row.packetID, 0x43)
                    XCTAssertEqual(row.payload[0], UInt8(((slot + 1) << 4) | rowCount))
                    let frequency = Int(row.payload[1]) * 256 + Int(row.payload[2])
                    let band = row.payload[5] & 0x1F
                    XCTAssertEqual(identities[band], frequency)
                    frequencies.insert(frequency)
                    roles.insert("\(band):\(slot)")
                    directions.insert(row.payload[5] & 0xE0)
                }
                XCTAssertEqual(frequencies.count, rowCount)
                if relative % 6 != 0 {
                    XCTAssertEqual(plan.emissions.map(\.bytes), previousPackets,
                                   "two-second held state changed before the next authored step")
                }
                phases[sample.phase, default: 0] += 1
            }
            previousPackets = plan.emissions.map(\.bytes)
        }
        XCTAssertEqual(roles, Set(["8:0", "8:1", "8:2", "4:0", "4:1", "4:2",
                                  "2:0", "2:1", "2:2"]))
        XCTAssertEqual(directions, Set([0x20, 0x40, 0x80]))
        XCTAssertEqual(phases.count, 5)
        XCTAssertTrue(phases.values.allSatisfy { $0 == 144 })
        let resolved = try XCTUnwrap(JSONSerialization.jsonObject(
            with: encounter.resolvedScenarioEvidenceData()) as? [String: Any])
        let retained = try XCTUnwrap(resolved["samples"] as? [[String: Any]])
        XCTAssertEqual(retained.count, 792)
        XCTAssertEqual(retained[12]["phase"] as? String, "reader_qualification_primary")
        XCTAssertEqual(retained[24]["phase"] as? String, "reader_qualification_two_cards")
        XCTAssertEqual((retained[24]["alerts"] as? [[String: Any]])?.map { $0["band"] as? String },
                       ["x", "k", "ka"])
    }

    func testHandshakeClearEarlyStartQueuesBeforePolling() {
        var state = HandshakeClearDeliveryState()

        XCTAssertEqual(state.ensure(), .enqueue)
        XCTAssertTrue(state.isPending)
        XCTAssertFalse(state.isDeliveryConfirmed)
    }

    func testHandshakeClearPreDeliveryStartRetriesOnePendingRow() {
        var state = HandshakeClearDeliveryState()
        XCTAssertEqual(state.ensure(), .enqueue)

        XCTAssertEqual(state.ensure(), .retryPending)
        XCTAssertTrue(state.isPending)
        XCTAssertFalse(state.isDeliveryConfirmed)
    }

    func testHandshakeClearPostDeliveryStartQueuesNothingAndStaysQuiet() {
        var state = HandshakeClearDeliveryState()
        XCTAssertEqual(state.ensure(), .enqueue)

        XCTAssertTrue(state.confirmDelivery())
        XCTAssertFalse(state.confirmDelivery())
        XCTAssertTrue(state.isDeliveryConfirmed)
        XCTAssertFalse(state.isPending)
        XCTAssertEqual(state.ensure(), .alreadyDelivered)
        XCTAssertFalse(state.isPending)
    }

    func testHandshakeClearDroppedPendingRowCanBeRequeued() {
        var state = HandshakeClearDeliveryState()
        XCTAssertEqual(state.ensure(), .enqueue)

        state.discardPending()

        XCTAssertFalse(state.isPending)
        XCTAssertFalse(state.isDeliveryConfirmed)
        XCTAssertEqual(state.ensure(), .enqueue)
    }

    /// Public behavior ID: `V1-RECONNECT-SESSION-001`.
    func testHandshakeOnlyPlanIsExactlyOneLiteralShortClearRow() throws {
        let emissions = V1.PlaybackPacketPlan.handshakeOnlyEmissions()
        let clear: [UInt8] = [
            0xAA, 0xD8, 0xEA, 0x43, 0x08,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0xB7, 0xAB,
        ]

        XCTAssertEqual(emissions, [V1.PlaybackPacketPlan.Emission(
            kind: .alertRow(index: 0, count: 0),
            channel: .displayShort,
            bytes: clear
        )])
        let decoded = try IndependentFrame.decode(emissions[0].bytes)
        XCTAssertEqual(decoded.destination, 0xD8)
        XCTAssertEqual(decoded.origin, 0xEA)
        XCTAssertEqual(decoded.packetID, 0x43)
        XCTAssertEqual(decoded.payload, Array(repeating: 0, count: 7))
    }

    /// Public behavior IDs: `V1-ALERT-TABLE-001` and `V1-DISPLAY-FRAME-001`.
    func testTwoRowPlanUsesLiteralBroadcastPacketsAndPriorityDisplay() throws {
        let sample = TimedSample(
            offset: 0,
            phase: "contract",
            muted: false,
            alerts: [
                ReplayAlert(
                    band: .k, frequencyMHz: 24_150, strength: 5,
                    direction: .front, isPriority: false
                ),
                ReplayAlert(
                    band: .ka, frequencyMHz: 34_700, strength: 6,
                    direction: .rear, isPriority: true
                ),
            ],
            sourceIndex: 0
        )

        let plan = V1.PlaybackPacketPlan(
            sample: sample,
            controlState: V1.Session.ControlState(
                mode: .advancedLogic,
                mainVolume: 4,
                mutedVolume: 0,
                savedMainVolume: 4,
                savedMutedVolume: 0
            ),
            displayOn: true,
            muted: false,
            blinkBogey: false,
            blinkArrow: false
        )
        let expectedRows: [[UInt8]] = [
            [
                0xAA, 0xD8, 0xEA, 0x43, 0x08,
                0x12, 0x5E, 0x56, 0xA9, 0x00, 0x24, 0x00,
                0x4A, 0xAB,
            ],
            [
                0xAA, 0xD8, 0xEA, 0x43, 0x08,
                0x22, 0x87, 0x8C, 0x00, 0xAF, 0x82, 0x80,
                0x9D, 0xAB,
            ],
        ]
        let expectedDisplay: [UInt8] = [
            0xAA, 0xD8, 0xEA, 0x31, 0x09,
            0x5B, 0x5B, 0x3F, 0x82, 0x82, 0x0C, 0x0C, 0x40,
            0xF7, 0xAB,
        ]

        XCTAssertEqual(plan.alertTablePackets, expectedRows)
        XCTAssertEqual(plan.displayPacket, expectedDisplay)
        XCTAssertEqual(plan.emissions.map(\.channel), [
            .displayShort, .displayShort, .displayShort,
        ])
        XCTAssertEqual(plan.emissions.map(\.kind), [
            .alertRow(index: 1, count: 2),
            .alertRow(index: 2, count: 2),
            .displayFrame,
        ])
        XCTAssertEqual(plan.emissions.map(\.bytes), expectedRows + [expectedDisplay])

        // This pins the deterministic emulator plan. CoreBluetooth buffering,
        // subscription mechanics, and actual notification delivery remain
        // integration evidence.
        let decodedRows = try plan.alertTablePackets.map(IndependentFrame.decode)
        XCTAssertEqual(decodedRows.map(\.destination), [0xD8, 0xD8])
        XCTAssertEqual(decodedRows.map(\.origin), [0xEA, 0xEA])
        XCTAssertEqual(decodedRows.map(\.packetID), [0x43, 0x43])
        XCTAssertEqual(decodedRows.map { $0.payload[0] }, [0x12, 0x22])
        XCTAssertEqual(decodedRows.map { $0.payload[6] & 0x80 }, [0x00, 0x80])

        let decodedDisplay = try IndependentFrame.decode(plan.displayPacket)
        XCTAssertEqual(decodedDisplay.destination, 0xD8)
        XCTAssertEqual(decodedDisplay.packetID, 0x31)
        XCTAssertEqual(decodedDisplay.payload, [
            0x5B, 0x5B, 0x3F, 0x82, 0x82, 0x0C, 0x0C, 0x40,
        ])
    }

    /// Public behavior IDs: `V1-ALERT-TABLE-001` and `V1-DISPLAY-FRAME-001`.
    func testEmptyPlanUsesLiteralClearThenIdlePackets() throws {
        let sample = TimedSample(
            offset: 0, phase: "idle", muted: false,
            alerts: [], sourceIndex: 0
        )
        let plan = V1.PlaybackPacketPlan(
            sample: sample,
            controlState: V1.Session.ControlState(
                mode: .advancedLogic,
                mainVolume: 4,
                mutedVolume: 0,
                savedMainVolume: 4,
                savedMutedVolume: 0
            ),
            displayOn: true,
            muted: false,
            blinkBogey: false,
            blinkArrow: false
        )
        let clear: [UInt8] = [
            0xAA, 0xD8, 0xEA, 0x43, 0x08,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0xB7, 0xAB,
        ]
        let idle: [UInt8] = [
            0xAA, 0xD8, 0xEA, 0x31, 0x09,
            0x38, 0x38, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x40,
            0x6E, 0xAB,
        ]

        XCTAssertEqual(plan.alertTablePackets, [clear])
        XCTAssertEqual(plan.displayPacket, idle)
        XCTAssertEqual(plan.emissions.map(\.bytes), [clear, idle])
        XCTAssertEqual(plan.emissions.map(\.channel), [.displayShort, .displayShort])
        XCTAssertEqual(try IndependentFrame.decode(clear).payload, Array(repeating: 0, count: 7))
        XCTAssertEqual(try IndependentFrame.decode(idle).payload, [
            0x38, 0x38, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x40,
        ])
    }

    func testOneTwoAndThreeRowPlansRepeatCountAndUseOneBasedIndexes() throws {
        for count in 1...3 {
            let alerts: [ReplayAlert] = (0..<count).map { (index: Int) -> ReplayAlert in
                let frequencyMHz = UInt16(34_700 + index)
                let strength = index + 1
                let isPriority = index == count - 1

                return ReplayAlert(
                    band: .ka,
                    frequencyMHz: frequencyMHz,
                    strength: strength,
                    direction: .front,
                    isPriority: isPriority
                )
            }
            let sample = TimedSample(
                offset: 0, phase: "count-\(count)", muted: false,
                alerts: alerts, sourceIndex: 0
            )
            let plan = V1.PlaybackPacketPlan(
                sample: sample,
                controlState: V1.Session.ControlState(
                    mode: .advancedLogic,
                    mainVolume: 4,
                    mutedVolume: 0,
                    savedMainVolume: 4,
                    savedMutedVolume: 0
                ),
                displayOn: true,
                muted: false,
                blinkBogey: false,
                blinkArrow: false
            )
            let rows = try plan.alertTablePackets.map(IndependentFrame.decode)

            XCTAssertEqual(rows.count, count)
            XCTAssertEqual(rows.map { Int($0.payload[0] >> 4) }, Array(1...count))
            XCTAssertEqual(rows.map { Int($0.payload[0] & 0x0F) }, Array(repeating: count, count: count))
            XCTAssertEqual(rows.filter { ($0.payload[6] & 0x80) != 0 }.count, 1)
        }
    }

    /// Public behavior IDs: `V1-CONTROL-MODE-001` and
    /// `V1-CONTROL-VOLUME-001`.
    func testControlCommandsFlowIntoLiteralIdleAndActiveDisplayFrames() {
        let volumeWrite: [UInt8] = [
            0xAA, 0xDA, 0xE6, 0x39, 0x04, 0x07, 0x02, 0x00, 0xB0, 0xAB,
        ]
        let modeCases: [([UInt8], V1.ModeGlyph, [UInt8])] = [
            (
                [0xAA, 0xDA, 0xE6, 0x36, 0x02, 0x01, 0xA3, 0xAB],
                .allBogeys,
                [
                    0xAA, 0xD8, 0xEA, 0x31, 0x09,
                    0x77, 0x77, 0x00, 0x00, 0x00, 0x0C, 0x04, 0x72,
                    0x16, 0xAB,
                ]
            ),
            (
                [0xAA, 0xDA, 0xE6, 0x36, 0x02, 0x02, 0xA4, 0xAB],
                .logic,
                [
                    0xAA, 0xD8, 0xEA, 0x31, 0x09,
                    0x18, 0x18, 0x00, 0x00, 0x00, 0x0C, 0x08, 0x72,
                    0x5C, 0xAB,
                ]
            ),
            (
                [0xAA, 0xDA, 0xE6, 0x36, 0x02, 0x03, 0xA5, 0xAB],
                .advancedLogic,
                [
                    0xAA, 0xD8, 0xEA, 0x31, 0x09,
                    0x38, 0x38, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x72,
                    0xA0, 0xAB,
                ]
            ),
        ]

        for (modeRequest, expectedMode, expectedIdle) in modeCases {
            var session = V1.Session()
            let outcomes = session.receive(volumeWrite + modeRequest)
            XCTAssertEqual(outcomes[0].effects, [.volumeChanged(V1.Session.ControlState(
                mode: .advancedLogic,
                mainVolume: 7,
                mutedVolume: 2,
                savedMainVolume: 4,
                savedMutedVolume: 0
            ))])
            XCTAssertEqual(outcomes[1].effects, [.modeChanged(expectedMode)])
            XCTAssertFalse(outcomes.flatMap(\.effects).contains { effect in
                if case .reply = effect { return true }
                return false
            })
            XCTAssertEqual(V1.PlaybackPacketPlan.idleDisplayPacket(
                controlState: session.controlState,
                displayOn: true,
                muted: false
            ), expectedIdle)
        }

        var activeSession = V1.Session()
        let modeOne = modeCases[0].0
        _ = activeSession.receive(volumeWrite + modeOne)
        let sample = TimedSample(
            offset: 0,
            phase: "control-active",
            muted: false,
            alerts: [
                ReplayAlert(
                    band: .k, frequencyMHz: 24_150, strength: 5,
                    direction: .front, isPriority: false
                ),
                ReplayAlert(
                    band: .ka, frequencyMHz: 34_700, strength: 6,
                    direction: .rear, isPriority: true
                ),
            ],
            sourceIndex: 0
        )
        let active = V1.PlaybackPacketPlan(
            sample: sample,
            controlState: activeSession.controlState,
            displayOn: true,
            muted: false,
            blinkBogey: false,
            blinkArrow: false,
            includeAlertTable: false
        )
        XCTAssertEqual(active.emissions.map(\.channel), [.displayShort])
        XCTAssertEqual(active.displayPacket, [
            0xAA, 0xD8, 0xEA, 0x31, 0x09,
            0x5B, 0x5B, 0x3F, 0x82, 0x82, 0x0C, 0x04, 0x72,
            0x21, 0xAB,
        ])
    }

    func testBenchDetectorVolumeCheckpointsApplyOnceAndReachLiteralIdleAux2() throws {
        let encounter = BenchScenario.make()
        let expected = [
            DetectorVolumeCheckpoint(replaySecond: 244, mainVolume: 4, muteVolume: 0),
            DetectorVolumeCheckpoint(replaySecond: 250, mainVolume: 7, muteVolume: 0),
            DetectorVolumeCheckpoint(replaySecond: 252, mainVolume: 7, muteVolume: 2),
            DetectorVolumeCheckpoint(replaySecond: 254, mainVolume: 4, muteVolume: 2),
            DetectorVolumeCheckpoint(replaySecond: 256, mainVolume: 4, muteVolume: 0),
        ]
        let expectedAux2: [UInt8] = [0x40, 0x70, 0x72, 0x42, 0x40]
        let expectedLines = [
            "V1REPLAY_EVENT {\"state\":\"detector_volume\",\"replaySecond\":244,\"mainVolume\":4,\"muteVolume\":0}",
            "V1REPLAY_EVENT {\"state\":\"detector_volume\",\"replaySecond\":250,\"mainVolume\":7,\"muteVolume\":0}",
            "V1REPLAY_EVENT {\"state\":\"detector_volume\",\"replaySecond\":252,\"mainVolume\":7,\"muteVolume\":2}",
            "V1REPLAY_EVENT {\"state\":\"detector_volume\",\"replaySecond\":254,\"mainVolume\":4,\"muteVolume\":2}",
            "V1REPLAY_EVENT {\"state\":\"detector_volume\",\"replaySecond\":256,\"mainVolume\":4,\"muteVolume\":0}",
        ]

        XCTAssertEqual(encounter.samples.count, 828)
        XCTAssertEqual(BenchScenario.detectorVolumeCheckpoints, expected)
        XCTAssertEqual(encounter.detectorVolumeCheckpoints, expected)
        XCTAssertEqual(encounter.detectorVolumeCheckpoints.map(\.machineEventLine), expectedLines)
        XCTAssertTrue(encounter.samples[..<(244 * 3)].allSatisfy {
            $0.detectorVolume == nil
        })

        var session = V1.Session()
        for (index, checkpoint) in expected.enumerated() {
            let sampleIndex = checkpoint.replaySecond * 3
            XCTAssertEqual(encounter.detectorVolumeCheckpoint(at: sampleIndex), checkpoint)
            let nextSecond = index + 1 < expected.count
                ? expected[index + 1].replaySecond
                : BenchScenario.durationSeconds
            for heldIndex in (sampleIndex + 1)..<(nextSecond * 3) {
                XCTAssertNil(encounter.detectorVolumeCheckpoint(at: heldIndex))
                XCTAssertEqual(encounter.samples[heldIndex].detectorVolume, checkpoint.volume)
            }

            let control = session.applyDetectorCurrentVolume(
                main: checkpoint.mainVolume,
                muted: checkpoint.muteVolume
            )
            let packet = V1.PlaybackPacketPlan.idleDisplayPacket(
                controlState: control,
                displayOn: true,
                muted: false
            )
            let decoded = try IndependentFrame.decode(packet)
            XCTAssertEqual(decoded.packetID, 0x31)
            XCTAssertEqual(decoded.payload[7], expectedAux2[index])
        }

        XCTAssertEqual(session.controlState.mainVolume, 4)
        XCTAssertEqual(session.controlState.mutedVolume, 0)
        XCTAssertEqual(session.controlState.displayVolume, 0x40)
    }

    func testBenchDetectorMuteEdgesReachLiteralAlertBits() throws {
        let encounter = BenchScenario.make()
        let expected = [
            DetectorMuteCheckpoint(replaySecond: 185, muted: true),
            DetectorMuteCheckpoint(replaySecond: 189, muted: false),
            DetectorMuteCheckpoint(replaySecond: 202, muted: true),
            DetectorMuteCheckpoint(replaySecond: 203, muted: false),
            DetectorMuteCheckpoint(replaySecond: 205, muted: true),
            DetectorMuteCheckpoint(replaySecond: 206, muted: false),
            DetectorMuteCheckpoint(replaySecond: 208, muted: true),
            DetectorMuteCheckpoint(replaySecond: 209, muted: false),
            DetectorMuteCheckpoint(replaySecond: 211, muted: true),
            DetectorMuteCheckpoint(replaySecond: 212, muted: false),
            DetectorMuteCheckpoint(replaySecond: 214, muted: true),
            DetectorMuteCheckpoint(replaySecond: 215, muted: false),
            DetectorMuteCheckpoint(replaySecond: 217, muted: true),
            DetectorMuteCheckpoint(replaySecond: 218, muted: false),
            DetectorMuteCheckpoint(replaySecond: 220, muted: true),
            DetectorMuteCheckpoint(replaySecond: 221, muted: false),
            DetectorMuteCheckpoint(replaySecond: 222, muted: true),
            DetectorMuteCheckpoint(replaySecond: 223, muted: false),
            DetectorMuteCheckpoint(replaySecond: 225, muted: true),
            DetectorMuteCheckpoint(replaySecond: 226, muted: false),
            DetectorMuteCheckpoint(replaySecond: 228, muted: true),
            DetectorMuteCheckpoint(replaySecond: 229, muted: false),
            DetectorMuteCheckpoint(replaySecond: 231, muted: true),
            DetectorMuteCheckpoint(replaySecond: 232, muted: false),
            DetectorMuteCheckpoint(replaySecond: 234, muted: true),
            DetectorMuteCheckpoint(replaySecond: 235, muted: false),
            DetectorMuteCheckpoint(replaySecond: 237, muted: true),
            DetectorMuteCheckpoint(replaySecond: 238, muted: false),
            DetectorMuteCheckpoint(replaySecond: 240, muted: true),
            DetectorMuteCheckpoint(replaySecond: 241, muted: false),
        ]
        XCTAssertEqual(BenchScenario.detectorMuteCheckpoints, expected)
        XCTAssertEqual(encounter.detectorMuteCheckpoints, expected)
        XCTAssertEqual(encounter.detectorMuteCheckpoints.map(\.machineEventLine), [
            "V1REPLAY_EVENT {\"state\":\"detector_mute\",\"replaySecond\":185,\"muted\":true}",
            "V1REPLAY_EVENT {\"state\":\"detector_mute\",\"replaySecond\":189,\"muted\":false}",
            "V1REPLAY_EVENT {\"state\":\"detector_mute\",\"replaySecond\":202,\"muted\":true}",
            "V1REPLAY_EVENT {\"state\":\"detector_mute\",\"replaySecond\":203,\"muted\":false}",
            "V1REPLAY_EVENT {\"state\":\"detector_mute\",\"replaySecond\":205,\"muted\":true}",
            "V1REPLAY_EVENT {\"state\":\"detector_mute\",\"replaySecond\":206,\"muted\":false}",
            "V1REPLAY_EVENT {\"state\":\"detector_mute\",\"replaySecond\":208,\"muted\":true}",
            "V1REPLAY_EVENT {\"state\":\"detector_mute\",\"replaySecond\":209,\"muted\":false}",
            "V1REPLAY_EVENT {\"state\":\"detector_mute\",\"replaySecond\":211,\"muted\":true}",
            "V1REPLAY_EVENT {\"state\":\"detector_mute\",\"replaySecond\":212,\"muted\":false}",
            "V1REPLAY_EVENT {\"state\":\"detector_mute\",\"replaySecond\":214,\"muted\":true}",
            "V1REPLAY_EVENT {\"state\":\"detector_mute\",\"replaySecond\":215,\"muted\":false}",
            "V1REPLAY_EVENT {\"state\":\"detector_mute\",\"replaySecond\":217,\"muted\":true}",
            "V1REPLAY_EVENT {\"state\":\"detector_mute\",\"replaySecond\":218,\"muted\":false}",
            "V1REPLAY_EVENT {\"state\":\"detector_mute\",\"replaySecond\":220,\"muted\":true}",
            "V1REPLAY_EVENT {\"state\":\"detector_mute\",\"replaySecond\":221,\"muted\":false}",
            "V1REPLAY_EVENT {\"state\":\"detector_mute\",\"replaySecond\":222,\"muted\":true}",
            "V1REPLAY_EVENT {\"state\":\"detector_mute\",\"replaySecond\":223,\"muted\":false}",
            "V1REPLAY_EVENT {\"state\":\"detector_mute\",\"replaySecond\":225,\"muted\":true}",
            "V1REPLAY_EVENT {\"state\":\"detector_mute\",\"replaySecond\":226,\"muted\":false}",
            "V1REPLAY_EVENT {\"state\":\"detector_mute\",\"replaySecond\":228,\"muted\":true}",
            "V1REPLAY_EVENT {\"state\":\"detector_mute\",\"replaySecond\":229,\"muted\":false}",
            "V1REPLAY_EVENT {\"state\":\"detector_mute\",\"replaySecond\":231,\"muted\":true}",
            "V1REPLAY_EVENT {\"state\":\"detector_mute\",\"replaySecond\":232,\"muted\":false}",
            "V1REPLAY_EVENT {\"state\":\"detector_mute\",\"replaySecond\":234,\"muted\":true}",
            "V1REPLAY_EVENT {\"state\":\"detector_mute\",\"replaySecond\":235,\"muted\":false}",
            "V1REPLAY_EVENT {\"state\":\"detector_mute\",\"replaySecond\":237,\"muted\":true}",
            "V1REPLAY_EVENT {\"state\":\"detector_mute\",\"replaySecond\":238,\"muted\":false}",
            "V1REPLAY_EVENT {\"state\":\"detector_mute\",\"replaySecond\":240,\"muted\":true}",
            "V1REPLAY_EVENT {\"state\":\"detector_mute\",\"replaySecond\":241,\"muted\":false}",
        ])

        let control = V1.Session().controlState
        for checkpoint in expected {
            let sampleIndex = Int(checkpoint.replaySecond * Double(BenchScenario.cadenceHz))
            let sample = encounter.samples[sampleIndex]
            XCTAssertEqual(encounter.detectorMuteCheckpoint(at: sampleIndex), checkpoint)
            XCTAssertEqual(sample.muted, checkpoint.muted)
            XCTAssertEqual(sample.alerts.count, 1)
            if checkpoint.replaySecond < 200 {
                XCTAssertEqual(sample.priorityAlert?.frequencyMHz, 34_700)
                XCTAssertEqual(sample.priorityAlert?.band.mask, V1.Band.ka.mask)
                XCTAssertEqual(sample.priorityAlert?.direction, .front)
                XCTAssertEqual(sample.priorityAlert?.strength, 6)
            }

            let plan = V1.PlaybackPacketPlan(
                sample: sample,
                controlState: control,
                displayOn: true,
                muted: sample.muted,
                blinkBogey: false,
                blinkArrow: false
            )
            let decoded = try IndependentFrame.decode(plan.displayPacket)
            XCTAssertEqual(decoded.packetID, 0x31)
            XCTAssertEqual(decoded.payload[3] & 0x10, checkpoint.muted ? 0x10 : 0x00)
            XCTAssertEqual(decoded.payload[5] & 0x01, checkpoint.muted ? 0x01 : 0x00)
            XCTAssertEqual(decoded.payload[7], 0x40)
        }

        XCTAssertTrue(encounter.samples[(185 * 3)..<(189 * 3)].allSatisfy(\.muted))
        XCTAssertTrue(encounter.samples[(189 * 3)..<(202 * 3)].allSatisfy { !$0.muted })
        XCTAssertTrue(encounter.samples[(241 * 3)...].allSatisfy { !$0.muted })
        XCTAssertNil(encounter.detectorMuteCheckpoint(at: 185 * 3 + 1))
        XCTAssertNil(encounter.detectorMuteCheckpoint(at: 189 * 3 + 1))

        XCTAssertEqual(BenchScenario.muteQualificationCycles.count, 14)
        XCTAssertEqual(encounter.samples.filter { $0.phase == "mute_qualification" }.count, 126)
        XCTAssertEqual(BenchScenario.muteQualificationCycles.filter { !$0.startsMuted }.count, 7)
        XCTAssertEqual(BenchScenario.muteQualificationCycles.filter(\.startsMuted).count, 7)

        typealias AlertSpec = (
            bandMask: UInt8,
            frequencyMHz: UInt16,
            strength: Int,
            direction: V1.Direction
        )
        let expectedCycles: [(
            entry: AlertSpec,
            muteCommit: AlertSpec,
            unmuted: AlertSpec
        )] = [
            ((V1.Band.x.mask, 10_525, 2, .front),
             (V1.Band.x.mask, 10_525, 2, .front),
             (V1.Band.x.mask, 10_525, 2, .front)),
            ((V1.Band.k.mask, 24_150, 5, .side),
             (V1.Band.k.mask, 24_150, 5, .side),
             (V1.Band.k.mask, 24_150, 5, .side)),
            ((V1.Band.ka.mask, 34_700, 3, .rear),
             (V1.Band.ka.mask, 34_700, 3, .rear),
             (V1.Band.ka.mask, 34_700, 3, .rear)),
            ((V1.Band.ka.mask, 35_500, 6, .side),
             (V1.Band.ka.mask, 35_500, 6, .side),
             (V1.Band.ka.mask, 35_500, 6, .side)),
            ((V1.Band.x.mask, 10_525, 4, .rear),
             (V1.Band.x.mask, 10_525, 4, .rear),
             (V1.Band.x.mask, 10_525, 4, .rear)),
            ((V1.Band.k.mask, 24_150, 1, .front),
             (V1.Band.k.mask, 24_150, 1, .front),
             (V1.Band.k.mask, 24_150, 1, .front)),
            ((V1.Band.ka.mask, 34_700, 5, .rear),
             (V1.Band.ka.mask, 34_700, 5, .rear),
             (V1.Band.ka.mask, 34_700, 5, .rear)),
            ((V1.Band.x.mask, 10_525, 1, .front),
             (V1.Band.ka.mask, 34_700, 4, .rear),
             (V1.Band.k.mask, 24_150, 6, .side)),
            ((V1.Band.k.mask, 24_150, 6, .side),
             (V1.Band.x.mask, 10_525, 3, .front),
             (V1.Band.ka.mask, 35_500, 1, .rear)),
            ((V1.Band.ka.mask, 35_500, 1, .rear),
             (V1.Band.k.mask, 24_150, 4, .side),
             (V1.Band.x.mask, 10_525, 6, .front)),
            ((V1.Band.x.mask, 10_525, 6, .side),
             (V1.Band.ka.mask, 34_700, 3, .front),
             (V1.Band.k.mask, 24_150, 1, .rear)),
            ((V1.Band.k.mask, 24_150, 1, .rear),
             (V1.Band.x.mask, 10_525, 4, .side),
             (V1.Band.ka.mask, 35_500, 6, .front)),
            ((V1.Band.ka.mask, 35_500, 6, .front),
             (V1.Band.k.mask, 24_150, 3, .rear),
             (V1.Band.x.mask, 10_525, 1, .side)),
            ((V1.Band.x.mask, 10_525, 1, .rear),
             (V1.Band.ka.mask, 34_700, 4, .side),
             (V1.Band.k.mask, 24_150, 6, .front)),
        ]

        func assertAlert(_ actual: BenchScenario.MuteQualificationAlert,
                         _ expected: AlertSpec) {
            XCTAssertEqual(actual.band.mask, expected.bandMask)
            XCTAssertEqual(actual.frequencyMHz, expected.frequencyMHz)
            XCTAssertEqual(actual.strength, expected.strength)
            XCTAssertEqual(actual.direction, expected.direction)
        }

        func packetPlan(at sampleIndex: Int) -> V1.PlaybackPacketPlan {
            let sample = encounter.samples[sampleIndex]
            return V1.PlaybackPacketPlan(
                sample: sample,
                controlState: control,
                displayOn: true,
                muted: sample.muted,
                blinkBogey: false,
                blinkArrow: false
            )
        }

        func assertRawMute(_ plan: V1.PlaybackPacketPlan,
                           _ expectedMuted: Bool) throws {
            let frame = try IndependentFrame.decode(plan.displayPacket)
            XCTAssertEqual(frame.payload[3] & 0x10, expectedMuted ? 0x10 : 0x00)
            XCTAssertEqual(frame.payload[5] & 0x01, expectedMuted ? 0x01 : 0x00)
        }

        for (index, cycle) in BenchScenario.muteQualificationCycles.enumerated() {
            let expectedCycle = expectedCycles[index]
            XCTAssertEqual(cycle.startSecond, 201 + index * 3)
            XCTAssertEqual(cycle.startsMuted, index >= 7)
            assertAlert(cycle.entryAlert, expectedCycle.entry)
            assertAlert(cycle.muteCommitAlert, expectedCycle.muteCommit)
            assertAlert(cycle.unmutedAlert, expectedCycle.unmuted)

            let start = cycle.startSecond * BenchScenario.cadenceHz
            for local in 0..<9 {
                let sample = encounter.samples[start + local]
                let expectedAlert = local == 0 ? cycle.entryAlert
                    : local < 3 ? cycle.muteCommitAlert
                    : cycle.unmutedAlert
                let expectedMuted = cycle.startsMuted ? local < 3 : (3..<6).contains(local)
                XCTAssertEqual(sample.phase, "mute_qualification")
                XCTAssertEqual(sample.muted, expectedMuted)
                XCTAssertEqual(sample.alerts.count, 1)
                XCTAssertEqual(sample.priorityAlert?.band.mask, expectedAlert.band.mask)
                XCTAssertEqual(sample.priorityAlert?.frequencyMHz, expectedAlert.frequencyMHz)
                XCTAssertEqual(sample.priorityAlert?.strength, expectedAlert.strength)
                XCTAssertEqual(sample.priorityAlert?.direction, expectedAlert.direction)
                XCTAssertEqual(sample.priorityAlert?.isPriority, true)
                XCTAssertNil(sample.detectorVolume)
                XCTAssertNil(sample.detectorMode)
                XCTAssertFalse(sample.scenarioArrowBlink)
            }

            if cycle.startsMuted {
                XCTAssertNotEqual(cycle.entryAlert.band.mask, cycle.muteCommitAlert.band.mask)
                XCTAssertNotEqual(cycle.entryAlert.frequencyMHz,
                                  cycle.muteCommitAlert.frequencyMHz)
                XCTAssertGreaterThanOrEqual(
                    abs(cycle.entryAlert.strength - cycle.muteCommitAlert.strength), 2
                )
                XCTAssertNotEqual(cycle.entryAlert.direction, cycle.muteCommitAlert.direction)
                XCTAssertNotEqual(cycle.muteCommitAlert.band.mask, cycle.unmutedAlert.band.mask)
                XCTAssertNotEqual(cycle.muteCommitAlert.frequencyMHz,
                                  cycle.unmutedAlert.frequencyMHz)
                XCTAssertGreaterThanOrEqual(
                    abs(cycle.muteCommitAlert.strength - cycle.unmutedAlert.strength), 2
                )
                XCTAssertNotEqual(cycle.muteCommitAlert.direction,
                                  cycle.unmutedAlert.direction)

                let firstRawMute = packetPlan(at: start)
                let secondRawMute = packetPlan(at: start + 1)
                let heldRawMute = packetPlan(at: start + 2)
                let rawUnmute = packetPlan(at: start + 3)
                try assertRawMute(firstRawMute, true)
                try assertRawMute(secondRawMute, true)
                try assertRawMute(heldRawMute, true)
                try assertRawMute(rawUnmute, false)
                XCTAssertNotEqual(firstRawMute.alertTablePackets,
                                  secondRawMute.alertTablePackets)
                XCTAssertNotEqual(firstRawMute.displayPacket, secondRawMute.displayPacket)
                XCTAssertEqual(secondRawMute.emissions, heldRawMute.emissions)
                XCTAssertNotEqual(secondRawMute.alertTablePackets,
                                  rawUnmute.alertTablePackets)
                XCTAssertNotEqual(secondRawMute.displayPacket, rawUnmute.displayPacket)
            } else {
                XCTAssertEqual(cycle.entryAlert.band.mask, cycle.muteCommitAlert.band.mask)
                XCTAssertEqual(cycle.entryAlert.frequencyMHz,
                               cycle.muteCommitAlert.frequencyMHz)
                XCTAssertEqual(cycle.entryAlert.strength, cycle.muteCommitAlert.strength)
                XCTAssertEqual(cycle.entryAlert.direction, cycle.muteCommitAlert.direction)
                XCTAssertEqual(cycle.muteCommitAlert.band.mask, cycle.unmutedAlert.band.mask)
                XCTAssertEqual(cycle.muteCommitAlert.frequencyMHz,
                               cycle.unmutedAlert.frequencyMHz)
                XCTAssertEqual(cycle.muteCommitAlert.strength, cycle.unmutedAlert.strength)
                XCTAssertEqual(cycle.muteCommitAlert.direction, cycle.unmutedAlert.direction)
            }
        }
    }

    func testDetectorMuteCheckpointPreservesFractionalReplayOffset() {
        let encounter = Encounter(
            origin: .externalInput,
            samples: [
                TimedSample(
                    offset: 20.5,
                    phase: "external",
                    muted: false,
                    alerts: [],
                    sourceIndex: 0
                ),
                TimedSample(
                    offset: 20.9,
                    phase: "external",
                    muted: true,
                    alerts: [],
                    sourceIndex: 1
                ),
            ]
        )

        let checkpoint = encounter.detectorMuteCheckpoint(at: 1)
        XCTAssertEqual(checkpoint?.replaySecond, 20.9)
        XCTAssertEqual(
            checkpoint?.machineEventLine,
            "V1REPLAY_EVENT {\"state\":\"detector_mute\",\"replaySecond\":20.9,\"muted\":true}"
        )
    }

    func testBenchDetectorModeCheckpointsApplyOnceAndReachLiteralFrames() throws {
        let encounter = BenchScenario.make()
        let expected = [
            DetectorModeCheckpoint(replaySecond: 260, mode: .advancedLogic),
            DetectorModeCheckpoint(replaySecond: 264, mode: .allBogeys),
            DetectorModeCheckpoint(replaySecond: 268, mode: .logic),
            DetectorModeCheckpoint(replaySecond: 272, mode: .advancedLogic),
        ]
        let expectedGlyphs: [UInt8] = [0x38, 0x77, 0x18, 0x38]
        let expectedModeBits: [UInt8] = [0x0C, 0x04, 0x08, 0x0C]
        XCTAssertEqual(BenchScenario.detectorModeCheckpoints, expected)
        XCTAssertEqual(encounter.detectorModeCheckpoints, expected)
        XCTAssertEqual(encounter.detectorModeCheckpoints.map(\.machineEventLine), [
            "V1REPLAY_EVENT {\"state\":\"detector_mode\",\"replaySecond\":260,\"modeChar\":\"L\"}",
            "V1REPLAY_EVENT {\"state\":\"detector_mode\",\"replaySecond\":264,\"modeChar\":\"A\"}",
            "V1REPLAY_EVENT {\"state\":\"detector_mode\",\"replaySecond\":268,\"modeChar\":\"l\"}",
            "V1REPLAY_EVENT {\"state\":\"detector_mode\",\"replaySecond\":272,\"modeChar\":\"L\"}",
        ])
        XCTAssertTrue(encounter.samples[..<(260 * 3)].allSatisfy { $0.detectorMode == nil })

        var session = V1.Session()
        for (index, checkpoint) in expected.enumerated() {
            let sampleIndex = checkpoint.replaySecond * BenchScenario.cadenceHz
            let sample = encounter.samples[sampleIndex]
            XCTAssertEqual(encounter.detectorModeCheckpoint(at: sampleIndex), checkpoint)
            XCTAssertEqual(sample.phase, "idle_tail")
            XCTAssertTrue(sample.alerts.isEmpty)

            let nextSecond = index + 1 < expected.count
                ? expected[index + 1].replaySecond
                : BenchScenario.durationSeconds
            for heldIndex in (sampleIndex + 1)..<(nextSecond * BenchScenario.cadenceHz) {
                XCTAssertNil(encounter.detectorModeCheckpoint(at: heldIndex))
                XCTAssertEqual(encounter.samples[heldIndex].detectorMode, checkpoint.mode)
            }

            let prior = session.controlState
            let control = session.applyDetectorMode(checkpoint.mode)
            XCTAssertEqual(control.mode, checkpoint.mode)
            XCTAssertEqual(control.mainVolume, prior.mainVolume)
            XCTAssertEqual(control.mutedVolume, prior.mutedVolume)
            XCTAssertEqual(control.savedMainVolume, prior.savedMainVolume)
            XCTAssertEqual(control.savedMutedVolume, prior.savedMutedVolume)

            let idle = try IndependentFrame.decode(V1.PlaybackPacketPlan.idleDisplayPacket(
                controlState: control,
                displayOn: true,
                muted: false
            ))
            XCTAssertEqual(idle.payload[0], expectedGlyphs[index])
            XCTAssertEqual(idle.payload[1], expectedGlyphs[index])
            XCTAssertEqual(idle.payload[6], expectedModeBits[index])

            let activeSample = TimedSample(
                offset: 0,
                phase: "mode-active-contract",
                muted: false,
                alerts: [ReplayAlert(
                    band: .ka,
                    frequencyMHz: 34_700,
                    strength: 6,
                    direction: .front,
                    isPriority: true
                )],
                sourceIndex: 0
            )
            let active = try IndependentFrame.decode(V1.PlaybackPacketPlan(
                sample: activeSample,
                controlState: control,
                displayOn: true,
                muted: false,
                blinkBogey: false,
                blinkArrow: false
            ).displayPacket)
            XCTAssertEqual(active.payload[0], 0x06)
            XCTAssertEqual(active.payload[6], expectedModeBits[index])
        }

        XCTAssertEqual(session.controlState.mode, .advancedLogic)
    }

    func testExplicitDraftHeaderPreservesFixtureAux1() {
        let control = V1.Session.ControlState(
            mode: .advancedLogic,
            mainVolume: 4,
            mutedVolume: 0,
            savedMainVolume: 4,
            savedMutedVolume: 0
        )
        XCTAssertEqual(V1.PlaybackPacketPlan.idleDisplayPacket(
            controlState: control,
            displayOn: true,
            muted: false,
            header: .repoConvention
        ), [
            0xAA, 0xDA, 0xE4, 0x31, 0x09,
            0x38, 0x38, 0x00, 0x00, 0x00, 0x0C, 0x00, 0x40,
            0x5E, 0xAB,
        ])

        let activeSample = TimedSample(
            offset: 0,
            phase: "draft-active",
            muted: false,
            alerts: [
                ReplayAlert(
                    band: .k, frequencyMHz: 24_150, strength: 5,
                    direction: .front, isPriority: false
                ),
                ReplayAlert(
                    band: .ka, frequencyMHz: 34_700, strength: 6,
                    direction: .rear, isPriority: true
                ),
            ],
            sourceIndex: 0
        )
        let active = V1.PlaybackPacketPlan(
            sample: activeSample,
            controlState: control,
            displayOn: true,
            muted: false,
            blinkBogey: false,
            blinkArrow: false,
            header: .repoConvention,
            includeAlertTable: false
        )
        XCTAssertEqual(active.displayPacket, [
            0xAA, 0xDA, 0xE4, 0x31, 0x09,
            0x5B, 0x5B, 0x3F, 0x82, 0x82, 0x0C, 0x00, 0x40,
            0xE7, 0xAB,
        ])
    }
}

private struct IndependentFrame {
    let destination: UInt8
    let origin: UInt8
    let packetID: UInt8
    let payload: [UInt8]

    static func decode(_ bytes: [UInt8]) throws -> IndependentFrame {
        guard bytes.count >= 7 else { throw IndependentFrameError.tooShort }
        guard bytes.first == 0xAA, bytes.last == 0xAB else {
            throw IndependentFrameError.badBoundary
        }
        let declaredLength = Int(bytes[4])
        guard declaredLength >= 1, bytes.count == declaredLength + 6 else {
            throw IndependentFrameError.badLength
        }
        let checksumIndex = bytes.count - 2
        let checksum = bytes[..<checksumIndex].reduce(UInt8(0), &+)
        guard bytes[checksumIndex] == checksum else {
            throw IndependentFrameError.badChecksum
        }
        return IndependentFrame(
            destination: bytes[1],
            origin: bytes[2],
            packetID: bytes[3],
            payload: Array(bytes[5..<checksumIndex])
        )
    }
}

private enum IndependentFrameError: Error {
    case tooShort
    case badBoundary
    case badLength
    case badChecksum
}
