package com.zheliu.pokecapsule.service;

import android.os.Build;

import java.util.Locale;

public final class DeviceRuntimeProfile {
    private DeviceRuntimeProfile() {}

    public static boolean isLowPowerReader() {
        return isLowPowerReader(Build.MANUFACTURER, Build.MODEL);
    }

    static boolean isLowPowerReader(String manufacturer, String model) {
        String maker = normalize(manufacturer);
        String name = normalize(model);
        return maker.contains("onyx")
                || maker.contains("boox")
                || name.contains("poke3")
                || name.contains("boox poke3");
    }

    private static String normalize(String value) {
        return value == null ? "" : value.trim().toLowerCase(Locale.ROOT);
    }
}
