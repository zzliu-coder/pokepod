package com.zheliu.pokecapsule.command;

import static org.junit.Assert.assertEquals;

import org.junit.Test;

public final class CommandOperationTest {
    @Test public void preservesProtocolWireNames() {
        assertEquals(CommandOperation.MOVE_CAPSULES,
                CommandOperation.fromWire("moveCapsules"));
        assertEquals("commitFinalText", CommandOperation.COMMIT_FINAL_TEXT.wireName);
        assertEquals(CommandOperation.DELETE_FOLDER,
                CommandOperation.fromWire("rmdir"));
    }
}
