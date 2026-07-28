package com.zheliu.pokecapsule.storage;

import com.zheliu.pokecapsule.core.Ids;
import com.zheliu.pokecapsule.core.TimeFormat;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

public final class RootWriteLock implements AutoCloseable {
    private static final long STALE_AFTER_MS = 30L * 60L * 1000L;
    private static final long HEARTBEAT_INTERVAL_MS = 30L * 1000L;
    private final File file;
    private final String transactionId;
    private final Thread heartbeatThread;
    private volatile boolean held;

    private RootWriteLock(File file, String transactionId) {
        this.file = file;
        this.transactionId = transactionId;
        this.held = true;
        this.heartbeatThread = new Thread(this::heartbeatLoop, "PokeCapsuleWriteLock");
        this.heartbeatThread.setDaemon(true);
        this.heartbeatThread.start();
    }

    public static RootWriteLock acquire(PokePaths paths, String owner) throws IOException {
        paths.ensureBase();
        File file = paths.writeLock();
        String transactionId = Ids.newId();
        if (!file.createNewFile()
                && !(isStale(file) && file.delete() && file.createNewFile())) {
            throw new IOException("设备正在维护或执行其他写操作");
        }
        try {
            String now = TimeFormat.utcNow();
            String data = "{\n"
                    + "  \"schemaVersion\": 1,\n"
                    + "  \"owner\": \"" + escape(owner) + "\",\n"
                    + "  \"transactionId\": \"" + transactionId + "\",\n"
                    + "  \"createdAt\": \"" + now + "\",\n"
                    + "  \"heartbeatAt\": \"" + now + "\"\n"
                    + "}\n";
            try (FileOutputStream output = new FileOutputStream(file, false)) {
                output.write(data.getBytes(StandardCharsets.UTF_8));
                output.flush();
                output.getFD().sync();
            }
            return new RootWriteLock(file, transactionId);
        } catch (Exception error) {
            file.delete();
            throw error instanceof IOException
                    ? (IOException) error
                    : new IOException("无法写入根锁", error);
        }
    }

    private static boolean isStale(File file) {
        return file.isFile()
                && System.currentTimeMillis() - file.lastModified() > STALE_AFTER_MS;
    }

    public String transactionId() {
        return transactionId;
    }

    private void heartbeatLoop() {
        while (held) {
            try {
                Thread.sleep(HEARTBEAT_INTERVAL_MS);
                if (!held || !isOwnedBy(file, transactionId)) return;
                if (!file.setLastModified(System.currentTimeMillis())) return;
            } catch (InterruptedException ignored) {
                return;
            } catch (Exception ignored) {
                return;
            }
        }
    }

    private static boolean isOwnedBy(File file, String transactionId) {
        try {
            return transactionId.equalsIgnoreCase(readJsonString(file, "transactionId"));
        } catch (Exception ignored) {
            return false;
        }
    }

    static String readJsonString(File file, String field) throws IOException {
        if (!file.isFile()) return null;
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        try (FileInputStream input = new FileInputStream(file)) {
            byte[] buffer = new byte[4096];
            int count;
            while ((count = input.read(buffer)) >= 0) output.write(buffer, 0, count);
        }
        String text = output.toString(StandardCharsets.UTF_8.name());
        Pattern pattern = Pattern.compile(
                "\\\"" + Pattern.quote(field) + "\\\"\\s*:\\s*\\\"([^\\\"]+)\\\"");
        Matcher matcher = pattern.matcher(text);
        return matcher.find() ? matcher.group(1) : null;
    }

    private static String escape(String value) {
        return value.replace("\\", "\\\\").replace("\"", "\\\"");
    }

    @Override public void close() throws IOException {
        if (!held) return;
        held = false;
        heartbeatThread.interrupt();
        if (isOwnedBy(file, transactionId) && !file.delete()) {
            throw new IOException("无法释放根写锁");
        }
    }
}
