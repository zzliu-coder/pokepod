package com.zheliu.pokecapsule.transcription;

public final class TranscriptionErrorText {
    private TranscriptionErrorText() {}

    public static boolean isRetryable(String code) {
        if (code == null) return true;
        if ("EmptyResult".equals(code)) return false;
        if (SentenceAudioPolicy.isDurationLimitError(code)) return false;
        return !code.startsWith("AuthFailure")
                && !code.startsWith("InvalidParameter")
                && !code.startsWith("UnsupportedOperation");
    }

    public static String userMessage(String code) {
        if (SentenceAudioPolicy.isDurationLimitError(code)) {
            return "录音超过转写上限，自动裁剪失败；原录音已保留";
        }
        if ("EmptyResult".equals(code)) {
            return "没有识别到清晰语音；可以播放检查或重新录制";
        }
        if (code != null && code.startsWith("AuthFailure")) {
            return "转写服务配置失效，请在设置中重新导入";
        }
        if (code != null && (code.contains("LimitExceeded")
                || code.contains("RequestLimitExceeded"))) {
            return "转写服务繁忙，稍后会自动重试";
        }
        if (code != null && (code.startsWith("InvalidParameter")
                || code.startsWith("UnsupportedOperation"))) {
            return "这段录音暂时无法转写；原录音已保留";
        }
        return "网络或转写服务暂时不可用，稍后会自动重试";
    }
}
