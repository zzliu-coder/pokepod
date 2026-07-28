package com.zheliu.pokecapsule.core;

import org.junit.Test;

import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertSame;
import static org.junit.Assert.assertTrue;

public final class ProcessingStateTest {
    @Test public void followsHappyPath() {
        assertTrue(ProcessingState.RECORDING.canTransitionTo(ProcessingState.RECORDED));
        assertTrue(ProcessingState.RECORDED.canTransitionTo(ProcessingState.QUEUED));
        assertTrue(ProcessingState.QUEUED.canTransitionTo(ProcessingState.TRANSCRIBING));
        assertTrue(ProcessingState.TRANSCRIBING.canTransitionTo(ProcessingState.RAW_READY));
        assertTrue(ProcessingState.RAW_READY.canTransitionTo(ProcessingState.CORRECTING));
        assertTrue(ProcessingState.CORRECTING.canTransitionTo(ProcessingState.READY));
    }

    @Test public void blocksInvalidJumpsAndAllowsRecovery() {
        assertFalse(ProcessingState.QUEUED.canTransitionTo(ProcessingState.READY));
        assertFalse(ProcessingState.READY.canTransitionTo(ProcessingState.RECORDING));
        assertTrue(ProcessingState.TRANSCRIBING.canTransitionTo(ProcessingState.QUEUED));
        assertTrue(ProcessingState.FAILED.canTransitionTo(ProcessingState.QUEUED));
    }

    @Test public void parsesWireValues() {
        assertSame(ProcessingState.RAW_READY, ProcessingState.fromWire("raw_ready"));
    }
}
