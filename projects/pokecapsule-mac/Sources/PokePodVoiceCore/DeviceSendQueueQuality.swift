public enum DeviceSendQueueQualityFormatter {
    public static func summary(_ info: BLEVoiceDeviceInfo?) -> String {
        guard let info,
              let attempts = info.notifyAttempts,
              let accepted = info.notifyAccepted else {
            return "固件未提供发送队列统计"
        }

        var parts = ["主机入队 \(accepted)/尝试 \(attempts)"]
        append(info.notifyFailures, label: "主机拒绝", to: &parts)
        append(info.queueOverflows, label: "队列溢出", to: &parts)
        append(info.sessionFailures, label: "会话失败", to: &parts)
        append(info.readyTimeouts, label: "Ready 超时", to: &parts)
        append(info.streamTimeouts, label: "音频超时", to: &parts)
        append(info.stopAckTimeouts, label: "StopAck 超时", to: &parts)
        if let code = info.lastErrorCode {
            parts.append("最近错误码 \(code)")
        }
        return parts.joined(separator: " · ")
    }

    private static func append(_ value: UInt64?, label: String, to parts: inout [String]) {
        if let value { parts.append("\(label) \(value)") }
    }
}

public enum DeviceInfoRefreshTrigger: Equatable {
    case sessionCompleted
    case sessionFailed
    case connectionChanged
}

public enum DeviceInfoRefreshPolicy {
    public static func shouldRefresh(after trigger: DeviceInfoRefreshTrigger) -> Bool {
        switch trigger {
        case .sessionCompleted, .sessionFailed: return true
        case .connectionChanged: return false
        }
    }
}
