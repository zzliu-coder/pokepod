package com.zheliu.pokecapsule.service;

import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

public final class DeviceRuntimeProfileTest {
    @Test public void recognizesPoke3AsLowPowerReader() {
        assertTrue(DeviceRuntimeProfile.isLowPowerReader("ONYX", "BOOX Poke3"));
        assertTrue(DeviceRuntimeProfile.isLowPowerReader("", "Poke3"));
    }

    @Test public void recognizesVivoAsPhone() {
        assertFalse(DeviceRuntimeProfile.isLowPowerReader("vivo", "V2303A"));
    }
}
