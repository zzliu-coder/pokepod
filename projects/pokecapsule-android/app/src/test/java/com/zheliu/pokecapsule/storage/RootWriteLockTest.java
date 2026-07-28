package com.zheliu.pokecapsule.storage;

import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotEquals;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

import java.io.File;
import java.nio.file.Files;

public final class RootWriteLockTest {
    @Test public void oldOwnerCannotDeleteReplacementLock() throws Exception {
        File root = Files.createTempDirectory("pokecapsule-lock").toFile();
        PokePaths paths = new PokePaths(root);
        RootWriteLock first = RootWriteLock.acquire(paths, "first");
        String firstId = first.transactionId();

        String foreign = "{\"schemaVersion\":1,\"owner\":\"foreign\","
                + "\"transactionId\":\"28edee65-d9a5-4c75-b240-e49371aa1d86\"}";
        Files.write(paths.writeLock().toPath(), foreign.getBytes());
        first.close();
        assertTrue(paths.writeLock().isFile());

        paths.writeLock().setLastModified(
                System.currentTimeMillis() - 31L * 60L * 1000L);
        RootWriteLock replacement = RootWriteLock.acquire(paths, "replacement");
        assertNotEquals(firstId, replacement.transactionId());
        replacement.close();
        assertFalse(paths.writeLock().exists());
    }
}
