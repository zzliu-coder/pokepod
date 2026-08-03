package com.zheliu.pokecapsule.storage;

import com.zheliu.pokecapsule.core.PathPolicy;

import java.io.File;
import java.io.IOException;

/** Package-internal folder transactions used only through CapsuleStore's lock boundary. */
final class FolderTransactions {
    private final PokePaths paths;

    FolderTransactions(PokePaths paths) { this.paths = paths; }

    void create(String relative) throws IOException {
        requireUserFolder(relative);
        File folder = paths.resolveUserFolder(relative);
        if (folder.exists()) throw new IOException("同级目录已存在");
        File parent = folder.getParentFile();
        if (parent == null || !parent.isDirectory() || !folder.mkdir()) {
            throw new IOException("无法创建目录");
        }
    }

    void rename(String oldRelative, String newRelative) throws IOException {
        requireUserFolder(oldRelative);
        requireUserFolder(newRelative);
        File source = paths.resolveUserFolder(oldRelative);
        File target = paths.resolveUserFolder(newRelative);
        if (!source.isDirectory()) throw new IOException("原目录不存在");
        if (target.exists()) throw new IOException("目标目录已存在");
        File parent = target.getParentFile();
        if (parent == null || !parent.isDirectory()) throw new IOException("目标上级目录不存在");
        if (!source.renameTo(target)) throw new IOException("无法重命名目录");
    }

    private static void requireUserFolder(String relative) throws IOException {
        if (!PathPolicy.isSafeRelativeFolder(relative) || relative.isEmpty()) {
            throw new IOException("目录名称不合法");
        }
    }
}
