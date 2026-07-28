package com.zheliu.pokecapsule.storage;

import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;

import org.junit.Test;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;

public final class MaintenanceSessionTest {
    @Test public void onlyOwnerCanUseAndEndSession() throws Exception {
        File root = Files.createTempDirectory("pokecapsule-maintenance").toFile();
        PokePaths paths = new PokePaths(root);
        String owner = "5937a678-c1db-4e94-9356-dd2f61bca462";
        String other = "eb9c8a7e-f21d-4e36-af85-b2495b4c08b7";

        paths.ensureBase();
        String marker = "{\"schemaVersion\":1,\"owner\":\"mac\","
                + "\"maintenanceId\":\"" + owner + "\"}";
        Files.write(paths.maintenanceLock().toPath(), marker.getBytes());
        assertTrue(paths.maintenanceLock().isFile());
        MaintenanceSession.requireOwner(paths, owner);
        expectFailure(() -> MaintenanceSession.requireOwner(paths, other));
        expectFailure(() -> MaintenanceSession.end(paths, other));
        assertTrue(paths.maintenanceLock().isFile());

        MaintenanceSession.end(paths, owner);
        assertFalse(paths.maintenanceLock().exists());
    }

    private static void expectFailure(ThrowingAction action) throws Exception {
        try {
            action.run();
            fail("expected IOException");
        } catch (IOException expected) {
            // Expected safe rejection.
        }
    }

    private interface ThrowingAction {
        void run() throws Exception;
    }
}
