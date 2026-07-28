package com.zheliu.pokecapsule.service;

final class BatteryPolicy {
    static final int MIN_AUTOMATIC_PERCENT = 15;

    private BatteryPolicy() {}

    static boolean allowsAutomatic(int percent) {
        return percent < 0 || percent >= MIN_AUTOMATIC_PERCENT;
    }
}
