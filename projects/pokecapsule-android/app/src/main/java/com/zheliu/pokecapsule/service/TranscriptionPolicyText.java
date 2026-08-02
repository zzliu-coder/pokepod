package com.zheliu.pokecapsule.service;

public final class TranscriptionPolicyText {
    private TranscriptionPolicyText() {}

    public static String automaticCondition() {
        return "打开应用后，只要联网就处理待转写胶囊";
    }

    public static String shortCondition() {
        return "打开后 · 联网";
    }
}
