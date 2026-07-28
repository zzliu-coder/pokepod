package com.zheliu.pokecapsule.core;

import java.util.EnumSet;
import java.util.Set;

public enum ProcessingState {
    RECORDING("recording"),
    RECORDED("recorded"),
    QUEUED("queued"),
    TRANSCRIBING("transcribing"),
    RAW_READY("raw_ready"),
    CORRECTING("correcting"),
    READY("ready"),
    FAILED("failed");

    private final String wireValue;

    ProcessingState(String wireValue) {
        this.wireValue = wireValue;
    }

    public String wireValue() {
        return wireValue;
    }

    public boolean canTransitionTo(ProcessingState next) {
        Set<ProcessingState> allowed;
        switch (this) {
            case RECORDING:
                allowed = EnumSet.of(RECORDED, FAILED);
                break;
            case RECORDED:
                allowed = EnumSet.of(QUEUED, FAILED);
                break;
            case QUEUED:
                allowed = EnumSet.of(TRANSCRIBING, FAILED);
                break;
            case TRANSCRIBING:
                allowed = EnumSet.of(RAW_READY, QUEUED, FAILED);
                break;
            case RAW_READY:
                allowed = EnumSet.of(CORRECTING, READY, FAILED);
                break;
            case CORRECTING:
                allowed = EnumSet.of(READY, RAW_READY, FAILED);
                break;
            case FAILED:
                allowed = EnumSet.of(QUEUED, RAW_READY);
                break;
            default:
                allowed = EnumSet.noneOf(ProcessingState.class);
        }
        return allowed.contains(next);
    }

    public static ProcessingState fromWire(String value) {
        for (ProcessingState state : values()) {
            if (state.wireValue.equals(value)) return state;
        }
        throw new IllegalArgumentException("Unknown processing state: " + value);
    }
}
