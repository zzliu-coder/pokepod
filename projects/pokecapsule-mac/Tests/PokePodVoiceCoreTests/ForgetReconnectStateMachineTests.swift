import XCTest
@testable import PokePodVoiceCore

final class ForgetReconnectStateMachineTests: XCTestCase {
    func testWriteResponseOrdersForgetDisconnectThenReconnect() {
        var machine = ForgetReconnectStateMachine(writeTimeoutSeconds: 0.75)
        var trace = [String]()
        trace += labels(machine.begin(now: 10))
        XCTAssertEqual(trace, ["send-forget"])
        XCTAssertTrue(machine.isAwaitingWrite)
        XCTAssertEqual(machine.tick(now: 10.74), [])

        trace += labels(machine.writeCompleted())
        XCTAssertEqual(trace, ["send-forget", "disconnect"])
        XCTAssertEqual(machine.writeCompleted(), [])
        trace += labels(machine.disconnected())

        XCTAssertEqual(trace, ["send-forget", "disconnect", "reconnect"])
        XCTAssertEqual(machine.phase, .idle)
    }

    func testMissingWriteResponseUsesBoundedTimeoutBeforeDisconnect() {
        var machine = ForgetReconnectStateMachine(writeTimeoutSeconds: 0.75)
        var trace = labels(machine.begin(now: 20))
        XCTAssertEqual(machine.tick(now: 20.74), [])
        trace += labels(machine.tick(now: 20.75))
        XCTAssertEqual(trace, ["send-forget", "disconnect"])
        trace += labels(machine.disconnected())
        XCTAssertEqual(trace, ["send-forget", "disconnect", "reconnect"])
    }

    func testCancelAndDuplicateBeginCannotCreateLateReconnect() {
        var machine = ForgetReconnectStateMachine()
        XCTAssertEqual(machine.begin(now: 0), [.sendForget])
        XCTAssertEqual(machine.begin(now: 0.1), [])
        machine.cancel()
        XCTAssertEqual(machine.writeCompleted(), [])
        XCTAssertEqual(machine.tick(now: 99), [])
        XCTAssertEqual(machine.disconnected(), [])
        XCTAssertEqual(machine.phase, .idle)
    }

    func testFirmwareDisconnectBeforeWriteResponseStillReconnects() {
        var machine = ForgetReconnectStateMachine(writeTimeoutSeconds: 0.75)
        var trace = labels(machine.begin(now: 0))
        trace += labels(machine.disconnected())
        XCTAssertEqual(trace, ["send-forget", "reconnect"])
        XCTAssertEqual(machine.phase, .idle)

        // A late CoreBluetooth write callback and the old timeout are inert.
        XCTAssertEqual(machine.writeCompleted(), [])
        XCTAssertEqual(machine.tick(now: 10), [])
        XCTAssertEqual(machine.disconnected(), [])
    }

    private func labels(_ actions: [ForgetReconnectAction]) -> [String] {
        actions.map {
            switch $0 {
            case .sendForget: return "send-forget"
            case .disconnect: return "disconnect"
            case .reconnect: return "reconnect"
            }
        }
    }
}
