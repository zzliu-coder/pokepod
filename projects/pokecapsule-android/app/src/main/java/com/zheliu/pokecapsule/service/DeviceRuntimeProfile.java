package com.zheliu.pokecapsule.service;

public final class DeviceRuntimeProfile {
    private DeviceRuntimeProfile() {}

    public static boolean isLowPowerReader() {
        return DeviceCapabilities.current().eink;
    }

    static boolean isLowPowerReader(String manufacturer, String model) {
        return DeviceCapabilities.forModel(manufacturer, model, "").eink;
    }
}
