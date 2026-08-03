package com.zheliu.pokecapsule.storage;

import com.zheliu.pokecapsule.model.CapsuleRecord;

import junit.framework.TestCase;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Collections;
import java.util.List;
import java.util.Locale;

public final class CapsuleStoreDeviceTest extends TestCase {
    private static final String ID = "4010f256-ce50-4db9-98d9-e1076d89f061";
    private File testRoot;

    @Override protected void setUp() throws Exception {
        super.setUp();
        String temporary = System.getProperty("java.io.tmpdir");
        assertNotNull(temporary);
        testRoot = new File(
                temporary,
                "pokecapsule-instrumentation/capsule-store-" + System.nanoTime());
    }

    @Override protected void tearDown() throws Exception {
        deleteTree(testRoot);
        super.tearDown();
    }

    public void testStartupDoesNotCreateMissingProcessingMetadata() throws Exception {
        PokePaths paths = new PokePaths(testRoot);
        paths.ensureBase();
        File directory = createCapsule(paths, ID);
        write(directory, "raw.txt", "保留原文");

        CapsuleStore store = new CapsuleStore(paths);
        List<CapsuleRecord> before = store.scan();
        assertEquals(1, before.size());
        assertTrue(before.get(0).readOnly);
        store.recoverInterruptedWork();

        assertFalse(new File(directory, "processing.json").exists());
        assertTrue(store.scan().get(0).readOnly);
    }

    public void testStartupDoesNotRewriteMismatchedProcessingMetadata() throws Exception {
        PokePaths paths = new PokePaths(testRoot);
        paths.ensureBase();
        File directory = createCapsule(paths, ID);
        File processing = new File(directory, "processing.json");
        byte[] original = processing(
                "11111111-1111-1111-1111-111111111111", "transcribing")
                .getBytes(StandardCharsets.UTF_8);
        Files.write(processing.toPath(), original);
        write(directory, "raw.txt", "保留原文");

        CapsuleStore store = new CapsuleStore(paths);
        assertTrue(store.scan().get(0).readOnly);
        store.recoverInterruptedWork();

        assertTrue(java.util.Arrays.equals(original, Files.readAllBytes(processing.toPath())));
    }

    public void testUppercaseProtocolUuidMovesAndRestoresLowercaseDirectory() throws Exception {
        String upper = ID.toUpperCase(Locale.ROOT);
        PokePaths paths = new PokePaths(testRoot);
        paths.ensureBase();
        File active = createCapsule(paths, ID);
        write(active, "processing.json", processing(ID, "ready"));

        CapsuleStore capsules = new CapsuleStore(paths);
        TrashStore trash = new TrashStore(paths, capsules);
        trash.moveToTrash(Collections.singletonList(upper));
        assertFalse(active.exists());
        assertTrue(new File(paths.trash(), ID).isDirectory());

        trash.restore(Collections.singletonList(upper));
        assertTrue(new File(paths.inbox(), ID).isDirectory());
        assertFalse(new File(paths.inbox(), upper).exists());
    }

    private static File createCapsule(PokePaths paths, String id) throws Exception {
        File directory = new File(paths.inbox(), id);
        assertTrue(directory.mkdir());
        write(directory, "capsule.json", "{\"schemaVersion\":1,\"id\":\"" + id
                + "\",\"title\":\"恢复测试\",\"createdAt\":\"2026-08-03T00:00:00Z\","
                + "\"updatedAt\":\"2026-08-03T00:00:00Z\",\"revision\":1,"
                + "\"favorite\":false,\"tags\":[]}");
        return directory;
    }

    private static String processing(String id, String status) {
        return "{\"schemaVersion\":1,\"capsuleId\":\"" + id
                + "\",\"revision\":4,\"durationMs\":8000,\"status\":\"" + status
                + "\",\"audioFile\":\"audio.m4a\",\"rawTextFile\":null,"
                + "\"polishedTextFile\":null}";
    }

    private static void write(File directory, String name, String value) throws Exception {
        Files.write(new File(directory, name).toPath(), value.getBytes(StandardCharsets.UTF_8));
    }

    private static void deleteTree(File file) {
        if (file == null || !file.exists()) return;
        File[] children = file.listFiles();
        if (children != null) {
            for (File child : children) deleteTree(child);
        }
        file.delete();
    }
}
