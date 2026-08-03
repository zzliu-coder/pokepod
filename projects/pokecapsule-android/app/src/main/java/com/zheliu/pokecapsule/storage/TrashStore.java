package com.zheliu.pokecapsule.storage;

import com.zheliu.pokecapsule.core.Ids;
import com.zheliu.pokecapsule.core.PathPolicy;
import com.zheliu.pokecapsule.core.TimeFormat;
import com.zheliu.pokecapsule.model.CapsuleRecord;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Map;

/**
 * Owns the reversible-delete boundary. Active capsule lookup deliberately skips
 * hidden folders, so trash operations can never be mistaken for normal edits.
 */
public final class TrashStore {
    private final PokePaths paths;
    private final CapsuleStore capsules;

    public TrashStore(PokePaths paths, CapsuleStore capsules) {
        this.paths = paths;
        this.capsules = capsules;
    }

    public List<CapsuleRecord> scan() throws IOException {
        paths.ensureBase();
        ArrayList<CapsuleRecord> records = new ArrayList<>();
        File[] children = paths.trash().listFiles(File::isDirectory);
        if (children != null) {
            for (File directory : children) {
                try {
                    JSONObject trash = metadataForScan(directory);
                    records.add(capsules.readTrashedCapsule(directory, trash));
                } catch (IOException ignored) {
                    // Keep damaged trash on disk for manual recovery.
                }
            }
        }
        Collections.sort(records, (left, right) -> right.trashedAt.compareTo(left.trashedAt));
        return records;
    }

    public void moveToTrash(List<String> ids) throws IOException {
        moveToTrash(ids, null);
    }

    public void moveToTrash(List<String> ids, Map<String, Integer> expected) throws IOException {
        ArrayList<File> sources = new ArrayList<>();
        ArrayList<File> destinations = new ArrayList<>();
        ArrayList<File> metadataFiles = new ArrayList<>();
        try (RootWriteLock ignored = RootWriteLock.acquire(paths, "android")) {
            try {
                capsules.validateExpectedRevisionsLocked(ids, expected);
                for (String id : ids) {
                    File source = capsules.requireActiveCapsule(id);
                    CapsuleRecord record = capsules.readCapsule(source);
                    File destination = new File(paths.trash(), source.getName());
                    if (destination.exists()) {
                        throw new IOException("回收站已存在同名胶囊: " + id);
                    }
                    JSONObject metadata = metadata(record, record.relativeFolder);
                    File metadataFile = new File(source, "trash.json");
                    AtomicFiles.writeUtf8(metadataFile, CapsuleStore.prettyJson(metadata));
                    sources.add(source);
                    destinations.add(destination);
                    metadataFiles.add(metadataFile);
                }
                CapsuleStore.moveDirectoriesAtomically(sources, destinations);
            } catch (IOException error) {
                for (File file : metadataFiles) file.delete();
                throw error;
            }
        }
    }

    public void restore(List<String> ids) throws IOException {
        restore(ids, null);
    }

    public void restore(List<String> ids, Map<String, Integer> expected) throws IOException {
        try (RootWriteLock ignored = RootWriteLock.acquire(paths, "android")) {
            validateExpectedSize(ids, expected);
            ArrayList<File> sources = new ArrayList<>();
            ArrayList<File> targets = new ArrayList<>();
            ArrayList<String> trashJson = new ArrayList<>();
            ArrayList<String> capsuleJson = new ArrayList<>();
            for (String id : ids) {
                File source = requireTrash(id);
                JSONObject trash = ensureMetadata(source);
                validateTrashRevision(id, trash, expected);
                File target = new File(
                        resolveRestoreFolder(
                                trash.optString("originalFolder", PathPolicy.INBOX)),
                        Ids.normalized(id));
                if (target.exists()) throw new IOException("恢复目标已存在: " + id);
                sources.add(source);
                targets.add(target);
                trashJson.add(CapsuleStore.prettyJson(trash));
                capsuleJson.add(CapsuleStore.readUtf8(new File(source, "capsule.json")));
            }
            int restored = 0;
            try {
                for (int index = 0; index < sources.size(); index++) {
                    File metadataFile = new File(sources.get(index), "trash.json");
                    if (!metadataFile.delete()) throw new IOException("无法清理回收站元数据");
                    if (!sources.get(index).renameTo(targets.get(index))) {
                        AtomicFiles.writeUtf8(metadataFile, trashJson.get(index));
                        throw new IOException("无法恢复胶囊: " + ids.get(index));
                    }
                    capsules.touchCapsule(targets.get(index));
                    restored++;
                }
            } catch (IOException error) {
                for (int index = restored; index >= 0; index--) {
                    if (index >= targets.size() || !targets.get(index).isDirectory()) continue;
                    try {
                        AtomicFiles.writeUtf8(
                                new File(targets.get(index), "capsule.json"),
                                capsuleJson.get(index));
                        if (!targets.get(index).renameTo(sources.get(index))) {
                            throw new IOException("恢复回滚失败: " + ids.get(index));
                        }
                        AtomicFiles.writeUtf8(
                                new File(sources.get(index), "trash.json"),
                                trashJson.get(index));
                    } catch (IOException rollbackError) {
                        error.addSuppressed(rollbackError);
                    }
                }
                for (int index = 0; index < sources.size(); index++) {
                    File metadataFile = new File(sources.get(index), "trash.json");
                    if (sources.get(index).isDirectory() && !metadataFile.isFile()) {
                        try {
                            AtomicFiles.writeUtf8(metadataFile, trashJson.get(index));
                        } catch (IOException rollbackError) {
                            error.addSuppressed(rollbackError);
                        }
                    }
                }
                throw error;
            }
        }
    }

    public void purge(List<String> ids) throws IOException {
        purge(ids, null);
    }

    public void purge(List<String> ids, Map<String, Integer> expected) throws IOException {
        try (RootWriteLock ignored = RootWriteLock.acquire(paths, "android")) {
            validateExpectedSize(ids, expected);
            ArrayList<File> sources = new ArrayList<>();
            for (String id : ids) {
                File source = requireTrash(id);
                validateTrashRevision(id, ensureMetadata(source), expected);
                sources.add(source);
            }
            File transaction = new File(paths.staging(), "purge-" + Ids.newId());
            if (!transaction.mkdir()) throw new IOException("无法创建永久删除事务");
            ArrayList<File> staged = new ArrayList<>();
            int moved = 0;
            try {
                for (File source : sources) {
                    File target = new File(transaction, source.getName());
                    if (!source.renameTo(target)) {
                        throw new IOException("无法暂存永久删除: " + source.getName());
                    }
                    staged.add(target);
                    moved++;
                }
            } catch (IOException error) {
                for (int index = moved - 1; index >= 0; index--) {
                    if (!staged.get(index).renameTo(sources.get(index))) {
                        error.addSuppressed(new IOException(
                                "永久删除回滚失败: " + sources.get(index).getName()));
                    }
                }
                CapsuleStore.deleteEmptyTree(transaction);
                throw error;
            }
            for (File directory : staged) {
                try {
                    CapsuleStore.deleteTree(directory);
                } catch (IOException ignoredDeleteFailure) {
                    // The purge commit already moved the item out of the visible trash.
                    // Recovery tooling can safely remove this hidden staging residue later.
                }
            }
            CapsuleStore.deleteEmptyTree(transaction);
        }
    }

    private void validateTrashRevision(
            String id, JSONObject trash, Map<String, Integer> expected) throws IOException {
        if (expected == null) return;
        String key = id.toLowerCase(java.util.Locale.ROOT);
        if (expected.size() == 0 || !expected.containsKey(key)) {
            throw new IOException("缺少回收站版本: " + id);
        }
        int actual = trash.optInt("revision", -1);
        int wanted = expected.get(key);
        if (actual != wanted) {
            throw new IOException("VERSION_CONFLICT " + id
                    + " expected=" + wanted + " actual=" + actual);
        }
    }

    private void validateExpectedSize(
            List<String> ids, Map<String, Integer> expected) throws IOException {
        if (expected != null && expected.size() != ids.size()) {
            throw new IOException("缺少完整的回收站版本快照");
        }
    }

    private File requireTrash(String id) throws IOException {
        if (!Ids.isUuid(id)) throw new IOException("无效胶囊 UUID");
        File directory = findTrashDirectory(id);
        if (directory == null || !directory.isDirectory()) {
            throw new IOException("回收站中找不到胶囊: " + id);
        }
        paths.assertInsideRoot(directory);
        JSONObject capsule = CapsuleStore.readJson(new File(directory, "capsule.json"));
        if (!id.equalsIgnoreCase(capsule.optString("id"))) {
            throw new IOException("回收站胶囊 UUID 不一致");
        }
        return directory;
    }

    private File findTrashDirectory(String id) {
        File[] children = paths.trash().listFiles(File::isDirectory);
        if (children == null) return null;
        for (File child : children) {
            if (child.getName().equalsIgnoreCase(id)
                    || child.getName().toLowerCase(java.util.Locale.ROOT)
                            .startsWith(id.toLowerCase(java.util.Locale.ROOT) + "-")) {
                return child;
            }
        }
        return null;
    }

    private JSONObject ensureMetadata(File directory) throws IOException {
        File file = new File(directory, "trash.json");
        if (file.isFile()) return validateMetadata(directory, CapsuleStore.readJson(file));
        JSONObject capsule = CapsuleStore.readJson(new File(directory, "capsule.json"));
        String id = capsule.optString("id", "");
        if (!Ids.isUuid(id) || !directoryNameMatchesId(directory.getName(), id)) {
            throw new IOException("回收站胶囊目录与 UUID 不一致");
        }
        JSONObject metadata = metadata(id, PathPolicy.INBOX,
                capsule.optInt("revision", 1) + 1, TimeFormat.utcNow());
        AtomicFiles.writeUtf8(file, CapsuleStore.prettyJson(metadata));
        return metadata;
    }

    private JSONObject metadataForScan(File directory) throws IOException {
        File file = new File(directory, "trash.json");
        if (file.isFile()) return validateMetadata(directory, CapsuleStore.readJson(file));
        JSONObject capsule = CapsuleStore.readJson(new File(directory, "capsule.json"));
        String id = capsule.optString("id", "");
        if (!Ids.isUuid(id) || !directoryNameMatchesId(directory.getName(), id)) {
            throw new IOException("回收站胶囊目录与 UUID 不一致");
        }
        return metadata(id, PathPolicy.INBOX,
                capsule.optInt("revision", 1) + 1, TimeFormat.utcNow());
    }

    private JSONObject validateMetadata(File directory, JSONObject trash) throws IOException {
        JSONObject capsule = CapsuleStore.readJson(new File(directory, "capsule.json"));
        String id = capsule.optString("id", "");
        String trashId = trash.optString("capsuleId", "");
        int capsuleRevision = capsule.optInt("revision", -1);
        int trashRevision = trash.optInt("revision", -1);
        String originalFolder = trash.optString("originalFolder", "");
        if (trash.optInt("schemaVersion", -1) != 1
                || !Ids.isUuid(id)
                || !directoryNameMatchesId(directory.getName(), id)
                || !id.equalsIgnoreCase(trashId)
                || trashRevision <= capsuleRevision
                || trash.optString("trashedAt", "").trim().isEmpty()
                || !(PathPolicy.INBOX.equals(originalFolder)
                        || PathPolicy.ARCHIVE.equals(originalFolder)
                        || PathPolicy.isSafeRelativeFolder(originalFolder))) {
            throw new IOException("回收站元数据与胶囊不一致");
        }
        return trash;
    }

    static boolean directoryNameMatchesId(String directoryName, String id) {
        String name = directoryName.toLowerCase(java.util.Locale.ROOT);
        String normalized = id.toLowerCase(java.util.Locale.ROOT);
        return name.equals(normalized) || name.startsWith(normalized + "-");
    }

    private JSONObject metadata(CapsuleRecord record, String originalFolder) throws IOException {
        return metadata(record.id, originalFolder, record.revision + 1, TimeFormat.utcNow());
    }

    private static JSONObject metadata(
            String id, String originalFolder, int revision, String trashedAt) throws IOException {
        try {
            JSONObject value = new JSONObject();
            value.put("schemaVersion", 1);
            value.put("capsuleId", id);
            value.put("trashedAt", trashedAt);
            value.put("originalFolder",
                    originalFolder == null || originalFolder.isEmpty()
                            ? PathPolicy.INBOX : originalFolder);
            value.put("revision", revision);
            return value;
        } catch (JSONException error) {
            throw new IOException("无法生成回收站元数据", error);
        }
    }

    private File resolveRestoreFolder(String original) throws IOException {
        if (PathPolicy.INBOX.equals(original)) return paths.inbox();
        if (PathPolicy.ARCHIVE.equals(original)) return paths.archive();
        if (PathPolicy.isSafeRelativeFolder(original)) {
            File folder = paths.resolveUserFolder(original);
            if (folder.isDirectory()) return folder;
        }
        return paths.inbox();
    }
}
