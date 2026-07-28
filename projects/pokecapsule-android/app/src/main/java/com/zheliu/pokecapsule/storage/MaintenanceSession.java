package com.zheliu.pokecapsule.storage;

import com.zheliu.pokecapsule.core.Ids;
import com.zheliu.pokecapsule.core.TimeFormat;

import org.json.JSONObject;

import java.io.File;
import java.io.IOException;

public final class MaintenanceSession {
    private static final long STALE_AFTER_MS = 30L * 60L * 1000L;

    private MaintenanceSession() {}

    public static void begin(PokePaths paths, String maintenanceId) throws IOException {
        requireId(maintenanceId);
        paths.ensureBase();
        try (RootWriteLock ignored = RootWriteLock.acquire(paths, "maintenance")) {
            File marker = paths.maintenanceLock();
            if (isActive(marker)) {
                throw new IOException("另一台电脑正在管理 PokeCapsule");
            }
            if (marker.exists() && !marker.delete()) {
                throw new IOException("无法清理遗留维护会话");
            }
            try {
                JSONObject data = new JSONObject();
                data.put("schemaVersion", 1);
                data.put("owner", "mac");
                data.put("maintenanceId", maintenanceId);
                data.put("createdAt", TimeFormat.utcNow());
                data.put("heartbeatAt", TimeFormat.utcNow());
                AtomicFiles.writeUtf8(marker, data.toString(2) + "\n");
            } catch (Exception error) {
                throw error instanceof IOException
                        ? (IOException) error
                        : new IOException("无法创建维护会话", error);
            }
        }
    }

    public static void requireOwner(PokePaths paths, String maintenanceId) throws IOException {
        requireId(maintenanceId);
        File marker = paths.maintenanceLock();
        if (!isActive(marker)) throw new IOException("维护会话已失效，请重新同步");
        if (!maintenanceId.equalsIgnoreCase(
                RootWriteLock.readJsonString(marker, "maintenanceId"))) {
            throw new IOException("维护会话不属于当前电脑");
        }
        marker.setLastModified(System.currentTimeMillis());
    }

    public static void end(PokePaths paths, String maintenanceId) throws IOException {
        requireId(maintenanceId);
        try (RootWriteLock ignored = RootWriteLock.acquire(paths, "maintenance")) {
            requireOwner(paths, maintenanceId);
            File marker = paths.maintenanceLock();
            if (!marker.delete()) throw new IOException("无法结束维护模式");
        }
    }

    public static boolean isActive(File marker) {
        return marker.isFile()
                && System.currentTimeMillis() - marker.lastModified() <= STALE_AFTER_MS;
    }

    private static void requireId(String maintenanceId) throws IOException {
        if (!Ids.isUuid(maintenanceId)) throw new IOException("维护会话 UUID 无效");
    }
}
