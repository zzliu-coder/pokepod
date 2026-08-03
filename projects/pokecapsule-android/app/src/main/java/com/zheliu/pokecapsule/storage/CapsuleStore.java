package com.zheliu.pokecapsule.storage;

import com.zheliu.pokecapsule.core.Ids;
import com.zheliu.pokecapsule.core.PathPolicy;
import com.zheliu.pokecapsule.core.ProcessingState;
import com.zheliu.pokecapsule.core.TimeFormat;
import com.zheliu.pokecapsule.model.CapsuleRecord;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

public final class CapsuleStore {
    private final PokePaths paths;
    private final FolderTransactions folderTransactions;
    private final TextVersionWriter textVersionWriter = new TextVersionWriter();

    public CapsuleStore(PokePaths paths) {
        this.paths = paths;
        this.folderTransactions = new FolderTransactions(paths);
    }

    public PokePaths paths() {
        return paths;
    }

    public synchronized List<CapsuleRecord> scan() throws IOException {
        paths.ensureBase();
        ArrayList<CapsuleRecord> result = new ArrayList<>();
        scanFolder(paths.inbox(), 0, result);
        scanFolder(paths.archive(), 0, result);
        File[] rootFolders = paths.root().listFiles(File::isDirectory);
        if (rootFolders != null) {
            for (File folder : rootFolders) {
                String name = folder.getName();
                if (name.startsWith(".") || name.equals(PathPolicy.INBOX)
                        || name.equals(PathPolicy.ARCHIVE)) continue;
                scanFolder(folder, 1, result);
            }
        }
        Collections.sort(result, (left, right) -> right.createdAt.compareTo(left.createdAt));
        return result;
    }

    public synchronized CapsuleRecord readCapsule(File directory) throws IOException {
        return readCapsuleDirectory(directory, true, null);
    }

    synchronized CapsuleRecord readTrashedCapsule(
            File directory, JSONObject trash) throws IOException {
        return readCapsuleDirectory(directory, false, trash);
    }

    private CapsuleRecord readCapsuleDirectory(
            File directory, boolean requireUuidDirectory, JSONObject trash) throws IOException {
        paths.assertInsideRoot(directory);
        if (requireUuidDirectory && !Ids.isUuid(directory.getName())) {
            throw new IOException("胶囊目录名不是 UUID");
        }
        JSONObject capsule = readJson(new File(directory, "capsule.json"));
        String id = capsule.optString("id");
        if (!Ids.isUuid(id)
                || (requireUuidDirectory && !directory.getName().equalsIgnoreCase(id))) {
            throw new IOException("胶囊 UUID 与目录不一致");
        }
        JSONObject processing;
        try {
            processing = readJson(new File(directory, "processing.json"));
            if (!id.equalsIgnoreCase(processing.optString("capsuleId"))) {
                processing = damagedProcessing(
                        processing, id, "转写状态 UUID 与胶囊不一致");
            }
        } catch (IOException error) {
            processing = damagedProcessing(null, id, error.getMessage());
        }
        File parent = directory.getParentFile();
        String relativeFolder = parent == null ? "" :
                paths.root().toURI().relativize(parent.toURI()).getPath();
        if (relativeFolder.endsWith("/")) {
            relativeFolder = relativeFolder.substring(0, relativeFolder.length() - 1);
        }
        return CapsuleRecord.fromJson(
                directory,
                capsule,
                processing,
                relativeFolder,
                readOptionalText(new File(directory, "raw.txt")),
                readOptionalText(new File(directory, "polished.md")),
                readOptionalText(new File(directory, "final.md")),
                trash != null,
                trash == null ? "" : trash.optString("trashedAt", ""),
                trash == null ? "" : trash.optString("originalFolder", ""),
                trash == null ? -1 : trash.optInt("revision", -1));
    }

    private static JSONObject damagedProcessing(
            JSONObject source, String id, String reason) throws IOException {
        JSONObject value = source == null ? new JSONObject() : source;
        try {
            value.put("schemaVersion", -1);
            value.put("capsuleId", id);
            if (!value.has("revision")) value.put("revision", -1);
            if (!value.has("durationMs")) value.put("durationMs", 0);
            value.put("status", ProcessingState.FAILED.wireValue());
            value.put("error", reason == null || reason.trim().isEmpty()
                    ? "处理状态文件损坏" : reason);
            return value;
        } catch (JSONException error) {
            throw new IOException("无法生成只读恢复记录", error);
        }
    }

    public synchronized File beginRecording(String id) throws IOException {
        if (!Ids.isUuid(id)) throw new IOException("无效录音 UUID");
        paths.ensureBase();
        File directory = new File(paths.staging(), id);
        if (directory.exists()) throw new IOException("暂存胶囊已存在");
        if (!directory.mkdirs()) throw new IOException("无法创建录音暂存目录");
        JSONObject processing = processingJson(id, 0, ProcessingState.RECORDING, 1);
        AtomicFiles.writeUtf8(new File(directory, "processing.json"), pretty(processing));
        return directory;
    }

    public synchronized File commitRecording(File stagingDirectory, long durationMs) throws IOException {
        paths.assertInsideRoot(stagingDirectory);
        String id = stagingDirectory.getName();
        File audio = new File(stagingDirectory, "audio.m4a");
        if (!Ids.isUuid(id) || !audio.isFile() || audio.length() < 256 || durationMs <= 0) {
            throw new IOException("录音为空或未完整停止");
        }
        String now = TimeFormat.utcNow();
        JSONObject capsule = new JSONObject();
        JSONObject processing = processingJson(id, durationMs, ProcessingState.QUEUED, 2);
        try {
            capsule.put("schemaVersion", 1);
            capsule.put("id", id);
            capsule.put("title", "语音 " + now.replace('T', ' ').replace("Z", ""));
            capsule.put("createdAt", now);
            capsule.put("updatedAt", now);
            capsule.put("revision", 1);
            capsule.put("favorite", false);
            capsule.put("tags", new JSONArray());
            capsule.put("language", "zh");
            capsule.put("contentHash", JSONObject.NULL);
        } catch (JSONException error) {
            throw new IOException("无法生成胶囊元数据", error);
        }
        AtomicFiles.writeUtf8(new File(stagingDirectory, "capsule.json"), pretty(capsule));
        AtomicFiles.writeUtf8(new File(stagingDirectory, "processing.json"), pretty(processing));
        File target = new File(paths.inbox(), id);
        if (target.exists() || !stagingDirectory.renameTo(target)) {
            throw new IOException("无法将录音提交到 Inbox");
        }
        return target;
    }

    public synchronized void recoverInterruptedWork() throws IOException {
        paths.ensureBase();
        File[] staged = paths.staging().listFiles(File::isDirectory);
        if (staged != null) {
            for (File directory : staged) {
                if (directory.getName().startsWith("purge-")) {
                    try (RootWriteLock ignored = RootWriteLock.acquire(paths, "android-recovery")) {
                        deleteTree(directory);
                    } catch (IOException ignored) {
                        // An active purge owns the lock, or residue needs a later pass.
                    }
                    continue;
                }
                File audio = new File(directory, "audio.m4a");
                if (Ids.isUuid(directory.getName()) && audio.isFile() && audio.length() >= 256) {
                    File marker = new File(directory, "interrupted.txt");
                    AtomicFiles.writeUtf8(marker, "录音进程异常结束；请在应用内恢复或导出此音频。\n");
                }
            }
        }
        for (CapsuleRecord record : scan()) {
            // A recovery record represents processing metadata that was missing,
            // malformed, from a newer schema, or bound to another capsule. Keep
            // the original files untouched so startup can never turn a readable
            // recovery view into an implicit repair.
            if (record.readOnly) continue;
            if (record.status == ProcessingState.TRANSCRIBING) {
                updateProcessing(record.directory, ProcessingState.QUEUED,
                        "transcription", "进程中断，已重新排队", false);
            } else {
                repairDerivedFilePointers(record);
            }
        }
    }

    private void repairDerivedFilePointers(CapsuleRecord record) throws IOException {
        File raw = new File(record.directory, "raw.txt");
        File polished = new File(record.directory, "polished.md");
        JSONObject processing = readJson(new File(record.directory, "processing.json"));
        boolean needsRaw = raw.isFile() && processing.isNull("rawTextFile");
        boolean needsPolished = polished.isFile() && processing.isNull("polishedTextFile");
        if (!needsRaw && !needsPolished) return;
        try (RootWriteLock ignored = RootWriteLock.acquire(paths, "android")) {
            if (needsRaw) processing.put("rawTextFile", "raw.txt");
            if (needsPolished) processing.put("polishedTextFile", "polished.md");
            processing.put("revision", processing.optInt("revision", 0) + 1);
            AtomicFiles.writeUtf8(
                    new File(record.directory, "processing.json"),
                    pretty(processing));
        } catch (JSONException error) {
            throw new IOException("无法修复派生文件索引", error);
        }
    }

    public synchronized void createFolder(String relative) throws IOException {
        try (RootWriteLock ignored = RootWriteLock.acquire(paths, "android")) {
            folderTransactions.create(relative);
        }
    }

    public synchronized void deleteFolderMovingContentsToInbox(String relative) throws IOException {
        deleteFolderMovingContentsToInbox(relative, null, null);
    }

    public synchronized void deleteFolderMovingContentsToInbox(
            String relative,
            List<String> expectedIds,
            Map<String, Integer> expected) throws IOException {
        if (!PathPolicy.isSafeRelativeFolder(relative) || relative.isEmpty()) {
            throw new IOException("不能删除此目录");
        }
        try (RootWriteLock ignored = RootWriteLock.acquire(paths, "android")) {
            File folder = paths.resolveUserFolder(relative);
            if (!folder.isDirectory()) throw new IOException("目录不存在");
            assertFolderTreeContainsOnlyCapsules(folder);
            ArrayList<File> capsules = new ArrayList<>();
            collectCapsuleDirectories(folder, capsules);
            ArrayList<String> ids = new ArrayList<>();
            for (File capsule : capsules) ids.add(capsule.getName());
            assertSameIds(ids, expectedIds);
            validateExpectedRevisionsLocked(ids, expected);
            ArrayList<File> destinations = new ArrayList<>();
            for (File capsule : capsules) {
                destinations.add(new File(paths.inbox(), capsule.getName()));
            }
            moveDirectoriesAtomically(capsules, destinations);
            deleteEmptyTree(folder);
        }
    }

    public synchronized void renameFolder(String oldRelative, String newRelative) throws IOException {
        try (RootWriteLock ignored = RootWriteLock.acquire(paths, "android")) {
            folderTransactions.rename(oldRelative, newRelative);
        }
    }

    public synchronized void moveCapsules(List<String> ids, String destination) throws IOException {
        moveCapsules(ids, destination, null);
    }

    public synchronized void moveCapsules(
            List<String> ids, String destination, Map<String, Integer> expected) throws IOException {
        File target = destination.equals(PathPolicy.INBOX) ? paths.inbox()
                : destination.equals(PathPolicy.ARCHIVE) ? paths.archive()
                : paths.resolveUserFolder(destination);
        if (!target.isDirectory()) throw new IOException("目标目录不存在");
        try (RootWriteLock ignored = RootWriteLock.acquire(paths, "android")) {
            validateExpectedRevisionsLocked(ids, expected);
            ArrayList<File> sources = new ArrayList<>();
            ArrayList<File> destinations = new ArrayList<>();
            ArrayList<String> originals = new ArrayList<>();
            for (String id : ids) {
                File source = requireCapsule(id);
                File destinationDirectory = new File(target, source.getName());
                if (source.getCanonicalFile().equals(destinationDirectory.getCanonicalFile())) continue;
                sources.add(source);
                destinations.add(destinationDirectory);
                originals.add(readUtf8(new File(source, "capsule.json")));
            }
            moveDirectoriesAtomically(sources, destinations);
            try {
                for (File directory : destinations) touchCapsule(directory);
            } catch (IOException error) {
                for (int index = destinations.size() - 1; index >= 0; index--) {
                    if (destinations.get(index).exists()) {
                        try {
                            AtomicFiles.writeUtf8(
                                    new File(destinations.get(index), "capsule.json"),
                                    originals.get(index));
                        } catch (IOException rollbackError) {
                            error.addSuppressed(rollbackError);
                        }
                        if (!destinations.get(index).renameTo(sources.get(index))) {
                            error.addSuppressed(new IOException(
                                    "移动回滚失败: " + sources.get(index).getName()));
                        }
                    }
                }
                throw error;
            }
        }
    }

    public synchronized List<String> copyCapsules(List<String> ids, String destination) throws IOException {
        return copyCapsules(ids, destination, null);
    }

    public synchronized List<String> copyCapsules(
            List<String> ids, String destination, Map<String, Integer> expected) throws IOException {
        File target = destination.equals(PathPolicy.INBOX) ? paths.inbox()
                : destination.equals(PathPolicy.ARCHIVE) ? paths.archive()
                : paths.resolveUserFolder(destination);
        if (!target.isDirectory()) throw new IOException("目标目录不存在");
        ArrayList<String> created = new ArrayList<>();
        try (RootWriteLock lock = RootWriteLock.acquire(paths, "android")) {
            validateExpectedRevisionsLocked(ids, expected);
            File transaction = new File(paths.staging(), lock.transactionId());
            if (!transaction.mkdir()) throw new IOException("无法创建复制事务");
            ArrayList<File> stagedCopies = new ArrayList<>();
            ArrayList<File> destinations = new ArrayList<>();
            int committed = 0;
            try {
                for (String id : ids) {
                    File source = requireCapsule(id);
                    String newId = Ids.newCopyId(id);
                    File stagedCopy = new File(transaction, newId);
                    File destinationDirectory = new File(target, newId);
                    if (destinationDirectory.exists()) throw new IOException("复制 UUID 冲突");
                    copyTree(source, stagedCopy);
                    rewriteCopiedMetadata(stagedCopy, newId);
                    stagedCopies.add(stagedCopy);
                    destinations.add(destinationDirectory);
                    created.add(newId);
                }
                for (int index = 0; index < stagedCopies.size(); index++) {
                    moveDirectory(stagedCopies.get(index), destinations.get(index));
                    committed++;
                }
            } catch (IOException error) {
                for (int index = committed - 1; index >= 0; index--) {
                    if (!destinations.get(index).renameTo(stagedCopies.get(index))) {
                        error.addSuppressed(new IOException("复制回滚失败: " + created.get(index)));
                    }
                }
                throw error;
            } finally {
                deleteEmptyTree(transaction);
            }
        }
        return created;
    }

    public synchronized void deleteCapsules(List<String> ids) throws IOException {
        deleteCapsules(ids, null);
    }

    public synchronized void deleteCapsules(
            List<String> ids, Map<String, Integer> expected) throws IOException {
        new TrashStore(paths, this).moveToTrash(ids, expected);
    }

    public synchronized List<CapsuleRecord> scanTrash() throws IOException {
        return new TrashStore(paths, this).scan();
    }

    public synchronized void restoreCapsules(List<String> ids) throws IOException {
        restoreCapsules(ids, null);
    }

    public synchronized void restoreCapsules(
            List<String> ids, Map<String, Integer> expected) throws IOException {
        new TrashStore(paths, this).restore(ids, expected);
    }

    public synchronized void purgeCapsules(List<String> ids) throws IOException {
        purgeCapsules(ids, null);
    }

    public synchronized void purgeCapsules(
            List<String> ids, Map<String, Integer> expected) throws IOException {
        new TrashStore(paths, this).purge(ids, expected);
    }

    public synchronized void setFavorite(List<String> ids, boolean favorite) throws IOException {
        setFavorite(ids, favorite, null);
    }

    public synchronized void setFavorite(
            List<String> ids, boolean favorite, Map<String, Integer> expected) throws IOException {
        mutateCapsules(ids, capsule -> capsule.put("favorite", favorite), expected);
    }

    public synchronized void setTitle(String id, String input) throws IOException {
        setTitle(id, input, -1);
    }

    public synchronized void setTitle(
            String id, String input, int expectedRevision) throws IOException {
        String title = input == null ? "" : input.trim();
        if (title.isEmpty() || title.length() > 200) throw new IOException("标题长度不合法");
        Map<String, Integer> expected = expectedRevision < 0
                ? null
                : Collections.singletonMap(
                        id.toLowerCase(java.util.Locale.ROOT), expectedRevision);
        mutateCapsules(
                Collections.singletonList(id), capsule -> capsule.put("title", title), expected);
    }

    public synchronized void addTag(List<String> ids, String input) throws IOException {
        addTags(ids, Collections.singletonList(input), null);
    }

    public synchronized void addTags(
            List<String> ids, List<String> inputs, Map<String, Integer> expected) throws IOException {
        ArrayList<String> normalized = new ArrayList<>();
        for (String input : inputs) {
            String tag = PathPolicy.normalizeTag(input);
            if (!PathPolicy.isValidTag(tag)) throw new IOException("标签名称不合法");
            normalized.add(tag);
        }
        mutateCapsules(ids, capsule -> {
            JSONArray tags = capsule.optJSONArray("tags");
            if (tags == null) tags = new JSONArray();
            for (String tag : normalized) {
                boolean exists = false;
                for (int index = 0; index < tags.length(); index++) {
                    if (tag.equalsIgnoreCase(tags.optString(index))) {
                        exists = true;
                        break;
                    }
                }
                if (!exists) tags.put(tag);
            }
            capsule.put("tags", tags);
        }, expected);
    }

    public synchronized void removeTag(List<String> ids, String input) throws IOException {
        removeTags(ids, Collections.singletonList(input), null);
    }

    public synchronized void removeTags(
            List<String> ids, List<String> inputs, Map<String, Integer> expected) throws IOException {
        HashSet<String> normalized = new HashSet<>();
        for (String input : inputs) {
            String tag = PathPolicy.normalizeTag(input);
            if (!PathPolicy.isValidTag(tag)) throw new IOException("标签名称不合法");
            normalized.add(tag.toLowerCase(java.util.Locale.ROOT));
        }
        mutateCapsules(ids, capsule -> {
            JSONArray old = capsule.optJSONArray("tags");
            JSONArray replacement = new JSONArray();
            if (old != null) {
                for (int index = 0; index < old.length(); index++) {
                    String value = old.optString(index);
                    if (!normalized.contains(value.toLowerCase(java.util.Locale.ROOT))) {
                        replacement.put(value);
                    }
                }
            }
            capsule.put("tags", replacement);
        }, expected);
    }

    public synchronized void renameTag(String oldInput, String newInput) throws IOException {
        renameTag(oldInput, newInput, null, null);
    }

    public synchronized void renameTag(
            String oldInput,
            String newInput,
            List<String> expectedIds,
            Map<String, Integer> expected) throws IOException {
        String oldTag = PathPolicy.normalizeTag(oldInput);
        String newTag = PathPolicy.normalizeTag(newInput);
        if (!PathPolicy.isValidTag(oldTag) || !PathPolicy.isValidTag(newTag)) {
            throw new IOException("标签名称不合法");
        }
        try (RootWriteLock ignored = RootWriteLock.acquire(paths, "android")) {
            ArrayList<String> affected = new ArrayList<>();
            for (CapsuleRecord record : scan()) {
                for (String tag : record.tags) {
                    if (oldTag.equalsIgnoreCase(tag)) {
                        affected.add(record.id);
                        break;
                    }
                }
            }
            assertSameIds(affected, expectedIds);
            validateExpectedRevisionsLocked(affected, expected);
            if (affected.isEmpty()) return;
            mutateCapsulesLocked(affected, capsule -> {
                JSONArray old = capsule.optJSONArray("tags");
                JSONArray replacement = new JSONArray();
                Set<String> seen = new HashSet<>();
                if (old != null) {
                    for (int index = 0; index < old.length(); index++) {
                        String value = old.optString(index);
                        String candidate = oldTag.equalsIgnoreCase(value) ? newTag : value;
                        if (seen.add(candidate.toLowerCase(java.util.Locale.ROOT))) {
                            replacement.put(candidate);
                        }
                    }
                }
                capsule.put("tags", replacement);
            });
        }
    }

    public synchronized void deleteTag(String input) throws IOException {
        deleteTag(input, null, null);
    }

    public synchronized void deleteTag(
            String input,
            List<String> expectedIds,
            Map<String, Integer> expected) throws IOException {
        String tag = PathPolicy.normalizeTag(input);
        if (!PathPolicy.isValidTag(tag)) throw new IOException("标签名称不合法");
        try (RootWriteLock ignored = RootWriteLock.acquire(paths, "android")) {
            ArrayList<String> affected = new ArrayList<>();
            for (CapsuleRecord record : scan()) {
                for (String value : record.tags) {
                    if (tag.equalsIgnoreCase(value)) {
                        affected.add(record.id);
                        break;
                    }
                }
            }
            assertSameIds(affected, expectedIds);
            validateExpectedRevisionsLocked(affected, expected);
            if (!affected.isEmpty()) {
                final String target = tag;
                mutateCapsulesLocked(affected, capsule -> {
                    JSONArray old = capsule.optJSONArray("tags");
                    JSONArray replacement = new JSONArray();
                    if (old != null) {
                        for (int index = 0; index < old.length(); index++) {
                            String value = old.optString(index);
                            if (!target.equalsIgnoreCase(value)) replacement.put(value);
                        }
                    }
                    capsule.put("tags", replacement);
                });
            }
        }
    }

    public synchronized void commitImportedCapsule(
            String stagedRelative,
            String id,
            String destination) throws IOException {
        if (!Ids.isUuid(id) || stagedRelative == null || stagedRelative.contains("..")
                || stagedRelative.startsWith("/") || stagedRelative.contains("\\")) {
            throw new IOException("导入暂存路径不合法");
        }
        File staged = new File(paths.root(), stagedRelative);
        paths.assertInsideRoot(staged);
        String stagingRoot = paths.staging().getCanonicalPath();
        String stagedPath = staged.getCanonicalPath();
        if (!stagedPath.startsWith(stagingRoot + File.separator)
                || !staged.isDirectory() || !id.equalsIgnoreCase(staged.getName())) {
            throw new IOException("导入目录不在暂存区");
        }
        CapsuleRecord record = readCapsule(staged);
        if (!id.equalsIgnoreCase(record.id)) throw new IOException("导入 UUID 不一致");
        File targetFolder = destination.equals(PathPolicy.INBOX) ? paths.inbox()
                : destination.equals(PathPolicy.ARCHIVE) ? paths.archive()
                : paths.resolveUserFolder(destination);
        if (!targetFolder.isDirectory()) throw new IOException("导入目标目录不存在");
        try (RootWriteLock ignored = RootWriteLock.acquire(paths, "android")) {
            File target = new File(targetFolder, id);
            if (target.exists()) throw new IOException("导入 UUID 已存在");
            if (!staged.renameTo(target)) throw new IOException("无法提交导入胶囊");
        }
    }

    public synchronized void commitCorrection(
            String stagedRelative,
            String id,
            int expectedRevision) throws IOException {
        if (!Ids.isUuid(id) || stagedRelative == null || stagedRelative.contains("..")
                || stagedRelative.startsWith("/") || stagedRelative.contains("\\")) {
            throw new IOException("校对暂存路径不合法");
        }
        File stagedDirectory = new File(paths.root(), stagedRelative);
        paths.assertInsideRoot(stagedDirectory);
        String stagingRoot = paths.staging().getCanonicalPath();
        String stagedPath = stagedDirectory.getCanonicalPath();
        if (!stagedPath.startsWith(stagingRoot + File.separator)
                || !stagedDirectory.isDirectory() || !id.equalsIgnoreCase(stagedDirectory.getName())) {
            throw new IOException("校对文件不在暂存区");
        }
        File stagedText = new File(stagedDirectory, "polished.md");
        if (!stagedText.isFile() || stagedText.length() == 0 || stagedText.length() > 1024 * 1024) {
            throw new IOException("校对文本为空或过大");
        }
        File directory = requireCapsule(id);
        File raw = new File(directory, "raw.txt");
        if (!raw.isFile()) throw new IOException("胶囊尚无原始转写");

        try (RootWriteLock ignored = RootWriteLock.acquire(paths, "android")) {
            JSONObject processing = readJson(new File(directory, "processing.json"));
            if (expectedRevision >= 0 && processing.optInt("revision", -1) != expectedRevision) {
                throw new IOException("胶囊状态已变化，请同步后重试");
            }
            String text = readUtf8(stagedText);
            try {
                processing.put("status", ProcessingState.READY.wireValue());
                processing.put("revision", processing.optInt("revision", 0) + 1);
                processing.put("polishedTextFile", "polished.md");
                processing.put("errorStage", JSONObject.NULL);
                processing.put("error", JSONObject.NULL);
            } catch (JSONException error) {
                throw new IOException("无法更新校对状态", error);
            }
            File polishedFile = new File(directory, "polished.md");
            boolean hadPolished = polishedFile.isFile();
            String previousPolished = hadPolished ? readUtf8(polishedFile) : null;
            AtomicFiles.writeUtf8(polishedFile, text);
            try {
                AtomicFiles.writeUtf8(
                        new File(directory, "processing.json"), pretty(processing));
            } catch (IOException error) {
                try {
                    if (hadPolished) AtomicFiles.writeUtf8(polishedFile, previousPolished);
                    else if (polishedFile.exists() && !polishedFile.delete()) {
                        throw new IOException("无法移除未提交的校对文字");
                    }
                } catch (IOException rollback) {
                    error.addSuppressed(rollback);
                }
                throw error;
            }
            deleteEmptyTree(stagedDirectory);
        }
    }

    public synchronized void commitFinalText(
            String stagedRelative,
            String id,
            int expectedRevision) throws IOException {
        if (!Ids.isUuid(id) || stagedRelative == null || stagedRelative.contains("..")
                || stagedRelative.startsWith("/") || stagedRelative.contains("\\")) {
            throw new IOException("最终文字暂存路径不合法");
        }
        File stagedDirectory = new File(paths.root(), stagedRelative);
        paths.assertInsideRoot(stagedDirectory);
        String stagingRoot = paths.staging().getCanonicalPath();
        if (!stagedDirectory.getCanonicalPath().startsWith(stagingRoot + File.separator)
                || !stagedDirectory.isDirectory()
                || !id.equalsIgnoreCase(stagedDirectory.getName())) {
            throw new IOException("最终文字不在暂存区");
        }
        File stagedText = new File(stagedDirectory, "final.md");
        if (!stagedText.isFile() || stagedText.length() > 1024 * 1024) {
            throw new IOException("最终文字文件缺失或过大");
        }
        File directory = requireCapsule(id);
        try (RootWriteLock ignored = RootWriteLock.acquire(paths, "android")) {
            JSONObject capsule = readJson(new File(directory, "capsule.json"));
            if (expectedRevision >= 0 && capsule.optInt("revision", -1) != expectedRevision) {
                throw new IOException("胶囊已变化，请解决同步冲突后重试");
            }
            writeFinalTextLocked(directory, capsule, readUtf8(stagedText));
            deleteEmptyTree(stagedDirectory);
        }
    }

    public synchronized void setFinalText(String id, String text) throws IOException {
        setFinalText(id, text, -1);
    }

    public synchronized void setFinalText(
            String id, String text, int expectedRevision) throws IOException {
        if (text == null || text.getBytes(StandardCharsets.UTF_8).length > 1024 * 1024) {
            throw new IOException("最终文字过大");
        }
        File directory = requireCapsule(id);
        try (RootWriteLock ignored = RootWriteLock.acquire(paths, "android")) {
            JSONObject capsule = readJson(new File(directory, "capsule.json"));
            if (expectedRevision >= 0
                    && capsule.optInt("revision", -1) != expectedRevision) {
                throw new IOException("VERSION_CONFLICT " + id
                        + " expected=" + expectedRevision
                        + " actual=" + capsule.optInt("revision", -1));
            }
            writeFinalTextLocked(directory, capsule, text);
        }
    }

    private void writeFinalTextLocked(
            File directory, JSONObject capsule, String text) throws IOException {
        textVersionWriter.writeFinal(directory, capsule, text);
    }

    public synchronized void validateExpectedRevisions(
            List<String> ids, Map<String, Integer> expected) throws IOException {
        validateExpectedRevisionsLocked(ids, expected);
    }

    void validateExpectedRevisionsLocked(
            List<String> ids, Map<String, Integer> expected) throws IOException {
        if (expected == null) return;
        if (expected.size() != ids.size()) {
            throw new IOException("缺少完整的胶囊版本快照");
        }
        for (String id : ids) {
            Integer value = expected.get(id.toLowerCase(java.util.Locale.ROOT));
            if (value == null) throw new IOException("缺少胶囊版本: " + id);
            File directory = requireCapsule(id);
            JSONObject capsule = readJson(new File(directory, "capsule.json"));
            int actual = capsule.optInt("revision", -1);
            if (actual != value) {
                throw new IOException("VERSION_CONFLICT " + id
                        + " expected=" + value + " actual=" + actual);
            }
        }
    }

    public synchronized Set<String> allTags() throws IOException {
        Set<String> tags = new HashSet<>();
        for (CapsuleRecord record : scan()) tags.addAll(record.tags);
        return tags;
    }

    public synchronized List<String> allUserFolders() throws IOException {
        paths.ensureBase();
        ArrayList<String> folders = new ArrayList<>();
        File[] first = paths.root().listFiles(File::isDirectory);
        if (first != null) {
            for (File folder : first) {
                String name = folder.getName();
                if (name.startsWith(".") || name.equals(PathPolicy.INBOX)
                        || name.equals(PathPolicy.ARCHIVE)) continue;
                folders.add(name);
                File[] second = folder.listFiles(File::isDirectory);
                if (second != null) {
                    for (File child : second) {
                        if (!child.getName().startsWith(".") && !Ids.isUuid(child.getName())) {
                            folders.add(name + "/" + child.getName());
                        }
                    }
                }
            }
        }
        Collections.sort(folders);
        return folders;
    }

    public synchronized void updateProcessing(
            File directory,
            ProcessingState next,
            String errorStage,
            String errorMessage,
            boolean incrementAttempt) throws IOException {
        JSONObject processing = readJson(new File(directory, "processing.json"));
        try {
            ProcessingState current = ProcessingState.fromWire(processing.getString("status"));
            if (current != next && !current.canTransitionTo(next)) {
                throw new IOException("非法状态变化: " + current + " -> " + next);
            }
            processing.put("status", next.wireValue());
            processing.put("revision", processing.optInt("revision", 0) + 1);
            processing.put("errorStage", errorStage == null ? JSONObject.NULL : errorStage);
            processing.put("error", errorMessage == null ? JSONObject.NULL : errorMessage);
            if (next == ProcessingState.RAW_READY) {
                processing.put("rawTextFile", "raw.txt");
            }
            if (incrementAttempt) processing.put("attempts", processing.optInt("attempts", 0) + 1);
        } catch (JSONException error) {
            throw new IOException("处理状态 JSON 损坏", error);
        }
        AtomicFiles.writeUtf8(new File(directory, "processing.json"), pretty(processing));
    }

    public synchronized void requeueFailedTranscriptions(
            List<String> ids, Map<String, Integer> expectedRevisions) throws IOException {
        try (RootWriteLock ignored = RootWriteLock.acquire(paths, "android")) {
            if (expectedRevisions == null || expectedRevisions.size() != ids.size()) {
                throw new IOException("缺少完整的转写状态版本快照");
            }
            ArrayList<File> files = new ArrayList<>();
            ArrayList<String> originals = new ArrayList<>();
            ArrayList<String> updates = new ArrayList<>();
            for (String id : ids) {
                File directory = requireCapsule(id);
                File file = new File(directory, "processing.json");
                String original = readUtf8(file);
                JSONObject processing = readJson(file);
                int actualRevision = processing.optInt("revision", -1);
                Integer expectedRevision = expectedRevisions.get(
                        id.toLowerCase(java.util.Locale.ROOT));
                if (expectedRevision == null || actualRevision != expectedRevision) {
                    throw new IOException("VERSION_CONFLICT " + id
                            + " expected=" + expectedRevision + " actual=" + actualRevision);
                }
                ProcessingState current = ProcessingState.fromWire(
                        processing.optString("status", "failed"));
                if (current != ProcessingState.FAILED) {
                    throw new IOException("只有失败的胶囊可以重新排队");
                }
                try {
                    processing.put("status", ProcessingState.QUEUED.wireValue());
                    processing.put("revision", actualRevision + 1);
                    processing.put("errorStage", JSONObject.NULL);
                    processing.put("error", JSONObject.NULL);
                    processing.put("attempts", 0);
                } catch (JSONException error) {
                    throw new IOException("无法重置转写次数", error);
                }
                files.add(file);
                originals.add(original);
                updates.add(pretty(processing));
            }
            int written = 0;
            try {
                for (; written < files.size(); written++) {
                    AtomicFiles.writeUtf8(files.get(written), updates.get(written));
                }
            } catch (IOException error) {
                for (int index = written - 1; index >= 0; index--) {
                    try {
                        AtomicFiles.writeUtf8(files.get(index), originals.get(index));
                    } catch (IOException rollbackError) {
                        error.addSuppressed(rollbackError);
                    }
                }
                throw error;
            }
        }
    }

    public static JSONObject readJson(File file) throws IOException {
        if (!file.isFile()) throw new IOException("缺少文件: " + file.getName());
        try (FileInputStream input = new FileInputStream(file);
             ByteArrayOutputStream output = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[8192];
            int count;
            while ((count = input.read(buffer)) >= 0) output.write(buffer, 0, count);
            return new JSONObject(output.toString(StandardCharsets.UTF_8.name()));
        } catch (JSONException error) {
            throw new IOException("JSON 损坏: " + file.getName(), error);
        }
    }

    static String readUtf8(File file) throws IOException {
        try (FileInputStream input = new FileInputStream(file);
             ByteArrayOutputStream output = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[8192];
            int count;
            while ((count = input.read(buffer)) >= 0) output.write(buffer, 0, count);
            return output.toString(StandardCharsets.UTF_8.name());
        }
    }

    static String readOptionalText(File file) {
        if (!file.isFile()) return "";
        try {
            return readUtf8(file);
        } catch (IOException ignored) {
            return "";
        }
    }

    private void scanFolder(File folder, int userDepth, List<CapsuleRecord> result) {
        File[] children = folder.listFiles(File::isDirectory);
        if (children == null) return;
        for (File child : children) {
            String name = child.getName();
            if (name.startsWith(".")) continue;
            if (Ids.isUuid(name)) {
                try {
                    result.add(readCapsule(child));
                } catch (IOException ignored) {
                    // Corrupt capsules remain on disk and are surfaced by recovery tooling.
                }
            } else if (userDepth < 2) {
                scanFolder(child, userDepth + 1, result);
            }
        }
    }

    private File requireCapsule(String id) throws IOException {
        File source = paths.findCapsuleById(id);
        if (source == null) throw new IOException("找不到胶囊: " + id);
        CapsuleRecord record = readCapsule(source);
        if (record.readOnly) throw new IOException("新协议胶囊只读");
        return source;
    }

    File requireActiveCapsule(String id) throws IOException {
        return requireCapsule(id);
    }

    void touchCapsule(File directory) throws IOException {
        File file = new File(directory, "capsule.json");
        JSONObject capsule = readJson(file);
        try {
            capsule.put("revision", capsule.optInt("revision", 0) + 1);
            capsule.put("updatedAt", TimeFormat.utcNow());
        } catch (JSONException error) {
            throw new IOException("无法更新胶囊版本", error);
        }
        AtomicFiles.writeUtf8(file, pretty(capsule));
    }

    private void mutateCapsules(List<String> ids, JsonMutation mutation) throws IOException {
        mutateCapsules(ids, mutation, null);
    }

    private void mutateCapsules(
            List<String> ids,
            JsonMutation mutation,
            Map<String, Integer> expected) throws IOException {
        try (RootWriteLock ignored = RootWriteLock.acquire(paths, "android")) {
            validateExpectedRevisionsLocked(ids, expected);
            mutateCapsulesLocked(ids, mutation);
        }
    }

    private void mutateCapsulesLocked(
            List<String> ids, JsonMutation mutation) throws IOException {
        ArrayList<File> files = new ArrayList<>();
        ArrayList<String> originals = new ArrayList<>();
        ArrayList<String> updates = new ArrayList<>();
        for (String id : ids) {
            File directory = requireCapsule(id);
            File file = new File(directory, "capsule.json");
            String original = readUtf8(file);
            JSONObject capsule;
            try {
                capsule = new JSONObject(original);
                mutation.apply(capsule);
                capsule.put("revision", capsule.optInt("revision", 0) + 1);
                capsule.put("updatedAt", TimeFormat.utcNow());
            } catch (JSONException error) {
                throw new IOException("无法修改元数据", error);
            }
            files.add(file);
            originals.add(original);
            updates.add(pretty(capsule));
        }
        int committed = 0;
        try {
            for (int index = 0; index < files.size(); index++) {
                AtomicFiles.writeUtf8(files.get(index), updates.get(index));
                committed++;
            }
        } catch (IOException error) {
            for (int index = committed - 1; index >= 0; index--) {
                try {
                    AtomicFiles.writeUtf8(files.get(index), originals.get(index));
                } catch (IOException rollbackError) {
                    error.addSuppressed(rollbackError);
                }
            }
            throw error;
        }
    }

    private void assertSameIds(
            List<String> actual, List<String> expectedIds) throws IOException {
        if (expectedIds == null) return;
        HashSet<String> left = new HashSet<>();
        HashSet<String> right = new HashSet<>();
        for (String id : actual) left.add(id.toLowerCase(java.util.Locale.ROOT));
        for (String id : expectedIds) right.add(id.toLowerCase(java.util.Locale.ROOT));
        if (!left.equals(right)) {
            throw new IOException("VERSION_CONFLICT 操作范围已变化，请同步后重试");
        }
    }

    private static JSONObject processingJson(
            String id, long durationMs, ProcessingState status, int revision) throws IOException {
        try {
            JSONObject processing = new JSONObject();
            processing.put("schemaVersion", 1);
            processing.put("capsuleId", id);
            processing.put("revision", revision);
            processing.put("durationMs", durationMs);
            processing.put("status", status.wireValue());
            processing.put("audioFile", "audio.m4a");
            processing.put("rawTextFile", JSONObject.NULL);
            processing.put("polishedTextFile", JSONObject.NULL);
            processing.put("errorStage", JSONObject.NULL);
            processing.put("error", JSONObject.NULL);
            processing.put("attempts", 0);
            processing.put("engine", "tencent-asr");
            processing.put("model", "16k_zh");
            return processing;
        } catch (JSONException error) {
            throw new IOException("无法生成处理状态", error);
        }
    }

    private static void rewriteCopiedMetadata(File directory, String newId) throws IOException {
        JSONObject capsule = readJson(new File(directory, "capsule.json"));
        JSONObject processing = readJson(new File(directory, "processing.json"));
        String now = TimeFormat.utcNow();
        try {
            capsule.put("id", newId);
            capsule.put("createdAt", now);
            capsule.put("updatedAt", now);
            capsule.put("revision", 1);
            processing.put("capsuleId", newId);
            processing.put("revision", processing.optInt("revision", 0) + 1);
        } catch (JSONException error) {
            throw new IOException("无法重写复制元数据", error);
        }
        AtomicFiles.writeUtf8(new File(directory, "capsule.json"), pretty(capsule));
        AtomicFiles.writeUtf8(new File(directory, "processing.json"), pretty(processing));
    }

    private static void collectCapsuleDirectories(File folder, List<File> result) {
        File[] children = folder.listFiles(File::isDirectory);
        if (children == null) return;
        for (File child : children) {
            if (Ids.isUuid(child.getName())) result.add(child);
            else if (!child.getName().startsWith(".")) collectCapsuleDirectories(child, result);
        }
    }

    private void assertFolderTreeContainsOnlyCapsules(File folder) throws IOException {
        File[] children = folder.listFiles();
        if (children == null) throw new IOException("无法读取目录内容");
        for (File child : children) {
            if (!child.isDirectory()) {
                throw new IOException("目录含有非胶囊文件，已停止删除: " + child.getName());
            }
            if (Ids.isUuid(child.getName())) {
                readCapsule(child);
            } else {
                if (child.getName().startsWith(".")) {
                    throw new IOException("目录含有隐藏内容，已停止删除: " + child.getName());
                }
                assertFolderTreeContainsOnlyCapsules(child);
            }
        }
    }

    static void moveDirectoriesAtomically(
            List<File> sources,
            List<File> destinations) throws IOException {
        if (sources.size() != destinations.size()) throw new IOException("移动事务参数不一致");
        for (File destination : destinations) {
            if (destination.exists()) throw new IOException("目标已存在: " + destination.getName());
        }
        int committed = 0;
        try {
            for (int index = 0; index < sources.size(); index++) {
                moveDirectory(sources.get(index), destinations.get(index));
                committed++;
            }
        } catch (IOException error) {
            for (int index = committed - 1; index >= 0; index--) {
                if (!destinations.get(index).renameTo(sources.get(index))) {
                    error.addSuppressed(new IOException(
                            "移动回滚失败: " + destinations.get(index).getName()));
                }
            }
            throw error;
        }
    }

    private static void moveDirectory(File source, File destination) throws IOException {
        if (destination.exists()) throw new IOException("目标已存在: " + destination.getName());
        if (!source.renameTo(destination)) throw new IOException("移动失败: " + source.getName());
    }

    private static void copyTree(File source, File destination) throws IOException {
        if (source.isDirectory()) {
            if (!destination.mkdir()) throw new IOException("无法创建复制目录");
            File[] children = source.listFiles();
            if (children != null) {
                for (File child : children) copyTree(child, new File(destination, child.getName()));
            }
            return;
        }
        try (FileInputStream input = new FileInputStream(source);
             FileOutputStream output = new FileOutputStream(destination)) {
            byte[] buffer = new byte[64 * 1024];
            int count;
            while ((count = input.read(buffer)) >= 0) output.write(buffer, 0, count);
            output.flush();
            output.getFD().sync();
        }
    }

    static void deleteTree(File file) throws IOException {
        if (file.isDirectory()) {
            File[] children = file.listFiles();
            if (children != null) {
                for (File child : children) deleteTree(child);
            }
        }
        if (file.exists() && !file.delete()) throw new IOException("无法删除: " + file.getName());
    }

    static void deleteEmptyTree(File file) throws IOException {
        deleteTree(file);
    }

    private interface JsonMutation {
        void apply(JSONObject object) throws JSONException;
    }

    static String prettyJson(JSONObject object) throws IOException {
        return pretty(object);
    }

    private static String pretty(JSONObject object) throws IOException {
        try {
            return object.toString(2) + "\n";
        } catch (JSONException error) {
            throw new IOException("无法序列化 JSON", error);
        }
    }
}
