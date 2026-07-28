package com.zheliu.pokecapsule.service;

import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

public final class BatteryPolicyTest {
    @Test public void automaticTranscriptionStartsAtFifteenPercent() {
        assertFalse(BatteryPolicy.allowsAutomatic(14));
        assertTrue(BatteryPolicy.allowsAutomatic(15));
        assertTrue(BatteryPolicy.allowsAutomatic(100));
    }

    @Test public void unavailableBatteryReadingDoesNotBlockForever() {
        assertTrue(BatteryPolicy.allowsAutomatic(-1));
    }
}
