package com.zheliu.pokecapsule.service;

import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

public final class DeviceCapabilitiesTest {
    @Test public void poke3UsesEinkCapabilities() {
        DeviceCapabilities value = DeviceCapabilities.forModel("ONYX", "Poke3", "poke3");
        assertTrue(value.eink);
        assertTrue(value.systemOverlayRecorder);
        assertFalse(value.inlineRecorder);
        assertFalse(value.directSpeakerPlayback);
        assertFalse(value.cellularTranscription);
        assertFalse(value.animatedTransitions);
    }

    @Test public void phoneUsesMobileCapabilities() {
        DeviceCapabilities value = DeviceCapabilities.forModel("vivo", "V2303A", "phone");
        assertFalse(value.eink);
        assertFalse(value.systemOverlayRecorder);
        assertTrue(value.inlineRecorder);
        assertTrue(value.directSpeakerPlayback);
        assertTrue(value.cellularTranscription);
        assertTrue(value.animatedTransitions);
    }
}
