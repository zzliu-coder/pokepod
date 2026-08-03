package com.zheliu.pokecapsule.service;

import android.os.Build;

import java.util.Locale;

/** Central product capabilities for the shared phone/Poke3 APK. */
public final class DeviceCapabilities {
    public final boolean eink;
    public final boolean inlineRecorder;
    public final boolean systemOverlayRecorder;
    public final boolean directSpeakerPlayback;
    public final boolean cellularTranscription;
    public final boolean animatedTransitions;
    public final boolean translucentSurfaces;

    private DeviceCapabilities(boolean eink) {
        this.eink = eink;
        inlineRecorder = !eink;
        systemOverlayRecorder = eink;
        directSpeakerPlayback = !eink;
        cellularTranscription = !eink;
        animatedTransitions = !eink;
        translucentSurfaces = !eink;
    }

    public static DeviceCapabilities current() {
        return forModel(Build.MANUFACTURER, Build.MODEL, Build.DEVICE);
    }

    static DeviceCapabilities forModel(String manufacturer, String model, String device) {
        String identity = String.join(" ", safe(manufacturer), safe(model), safe(device));
        return new DeviceCapabilities(identity.contains("onyx") || identity.contains("boox")
                || identity.contains("poke3") || identity.contains("poke_3"));
    }

    private static String safe(String value) {
        return value == null ? "" : value.trim().toLowerCase(Locale.ROOT);
    }
}
