package com.zheliu.pokecapsule.storage;

import android.os.Environment;

import com.zheliu.pokecapsule.core.Ids;
import com.zheliu.pokecapsule.core.PathPolicy;

import java.io.File;
import java.io.IOException;

public final class PokePaths {
    private final File root;

    public PokePaths() {
        this(new File(Environment.getExternalStorageDirectory(), "PokeCapsule"));
    }

    public PokePaths(File root) {
        this.root = root;
    }

    public File root() { return root; }
    public File inbox() { return new File(root, PathPolicy.INBOX); }
    public File archive() { return new File(root, PathPolicy.ARCHIVE); }
    public File staging() { return new File(root, ".staging"); }
    public File locks() { return new File(root, ".locks"); }
    public File commands() { return new File(root, ".commands"); }
    public File trash() { return new File(root, ".trash"); }
    public File deviceIdentity() { return new File(root, "device.json"); }
    public File writeLock() { return new File(locks(), "write.json"); }
    public File maintenanceLock() { return new File(locks(), "mac-maintenance.json"); }

    public void ensureBase() throws IOException {
        requireDirectory(root);
        requireDirectory(inbox());
        requireDirectory(archive());
        requireDirectory(staging());
        requireDirectory(locks());
        requireDirectory(commands());
        requireDirectory(trash());
    }

    public File resolveUserFolder(String relative) throws IOException {
        if (!PathPolicy.isSafeRelativeFolder(relative)) {
            throw new IOException("目录名称不安全或超过两级");
        }
        File target = relative == null || relative.isEmpty() ? root : new File(root, relative);
        assertInsideRoot(target);
        return target;
    }

    public File findCapsuleById(String id) throws IOException {
        if (!Ids.isUuid(id)) throw new IOException("无效胶囊 UUID");
        return findRecursive(root, id, 0);
    }

    public void assertInsideRoot(File target) throws IOException {
        String rootPath = root.getCanonicalPath();
        String targetPath = target.getCanonicalPath();
        if (!targetPath.equals(rootPath) && !targetPath.startsWith(rootPath + File.separator)) {
            throw new IOException("路径越过 PokeCapsule 根目录");
        }
    }

    public boolean isMaintenanceActive() {
        File marker = maintenanceLock();
        if (marker.isFile() && !MaintenanceSession.isActive(marker)) {
            marker.delete();
        }
        return MaintenanceSession.isActive(marker);
    }

    private File findRecursive(File folder, String id, int depth) throws IOException {
        if (depth > 3 || folder.getName().startsWith(".")) return null;
        File[] children = folder.listFiles(File::isDirectory);
        if (children == null) return null;
        for (File child : children) {
            if (Ids.isUuid(child.getName()) && child.getName().equalsIgnoreCase(id)) {
                assertInsideRoot(child);
                return child;
            }
            if (child.getName().startsWith(".") || Ids.isUuid(child.getName())) continue;
            File found = findRecursive(child, id, depth + 1);
            if (found != null) return found;
        }
        return null;
    }

    private static void requireDirectory(File directory) throws IOException {
        if (!directory.isDirectory() && !directory.mkdirs()) {
            throw new IOException("无法创建目录: " + directory);
        }
    }
}
