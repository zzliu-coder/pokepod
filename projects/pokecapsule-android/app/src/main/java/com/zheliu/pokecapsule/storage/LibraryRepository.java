package com.zheliu.pokecapsule.storage;

import android.content.Context;
import android.os.Handler;
import android.os.Looper;

import com.zheliu.pokecapsule.core.PathPolicy;
import com.zheliu.pokecapsule.model.CapsuleRecord;

import java.io.IOException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/** Async boundary between Android screens and the file-protocol store. */
public final class LibraryRepository implements AutoCloseable {
    public interface Result<T> {
        void onSuccess(T value);
    }

    public interface ErrorListener {
        void onError(String message);
    }

    public static final class Snapshot {
        public final List<CapsuleRecord> active;
        public final List<CapsuleRecord> trash;

        Snapshot(List<CapsuleRecord> active, List<CapsuleRecord> trash) {
            this.active = Collections.unmodifiableList(new ArrayList<>(active));
            this.trash = Collections.unmodifiableList(new ArrayList<>(trash));
        }
    }

    public static final class MenuData {
        public final Snapshot snapshot;
        public final List<String> folders;
        public final List<String> tags;

        MenuData(Snapshot snapshot, List<String> folders, List<String> tags) {
            this.snapshot = snapshot;
            this.folders = Collections.unmodifiableList(new ArrayList<>(folders));
            this.tags = Collections.unmodifiableList(new ArrayList<>(tags));
        }
    }

    private final Context context;
    private final CapsuleStore store;
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private final Handler main = new Handler(Looper.getMainLooper());
    private final ErrorListener errors;

    public LibraryRepository(Context context, ErrorListener errors) {
        this.context = context.getApplicationContext();
        this.store = new CapsuleStore(new PokePaths());
        this.errors = errors;
    }

    public void load(Result<Snapshot> result) {
        submit(() -> {
            DeviceIdentity.ensure(context, store.paths());
            return new Snapshot(store.scan(), store.scanTrash());
        }, result);
    }

    public void loadMenu(Result<MenuData> result) {
        submit(() -> {
            List<CapsuleRecord> active = store.scan();
            List<CapsuleRecord> trash = store.scanTrash();
            ArrayList<String> folders = new ArrayList<>(store.allUserFolders());
            ArrayList<String> tags = new ArrayList<>(store.allTags());
            Collections.sort(folders);
            Collections.sort(tags);
            return new MenuData(new Snapshot(active, trash), folders, tags);
        }, result);
    }

    public void loadFolders(Result<List<String>> result) {
        submit(() -> {
            ArrayList<String> values = new ArrayList<>(store.allUserFolders());
            Collections.sort(values);
            return values;
        }, result);
    }

    public void loadTags(Result<List<String>> result) {
        submit(() -> {
            ArrayList<String> values = new ArrayList<>(store.allTags());
            Collections.sort(values);
            return values;
        }, result);
    }

    public void loadRecord(String id, Result<CapsuleRecord> result) {
        submit(() -> {
            java.io.File directory = store.paths().findCapsuleById(id);
            if (directory == null) throw new IOException("胶囊已移动或不存在");
            return store.readCapsule(directory);
        }, result);
    }

    public void createFolder(String path, Runnable success) {
        mutate(() -> store.createFolder(path), success);
    }

    public void deleteFolder(String path, Runnable success) {
        mutate(() -> {
            List<CapsuleRecord> affected = new ArrayList<>();
            for (CapsuleRecord record : store.scan()) {
                if (record.relativeFolder.equals(path)
                        || record.relativeFolder.startsWith(path + "/")) {
                    affected.add(record);
                }
            }
            store.deleteFolderMovingContentsToInbox(
                    path, ids(affected), expected(affected));
        }, success);
    }

    public void renameTag(String oldTag, String newTag, Runnable success) {
        mutate(() -> {
            String normalizedOld = PathPolicy.normalizeTag(oldTag);
            List<CapsuleRecord> affected = new ArrayList<>();
            for (CapsuleRecord record : store.scan()) {
                for (String tag : record.tags) {
                    if (normalizedOld.equalsIgnoreCase(tag)) {
                        affected.add(record);
                        break;
                    }
                }
            }
            store.renameTag(oldTag, newTag, ids(affected), expected(affected));
        }, success);
    }

    public void move(List<CapsuleRecord> records, String destination, Runnable success) {
        mutate(() -> store.moveCapsules(ids(records), destination, expected(records)), success);
    }

    public void move(CapsuleRecord record, String destination, Runnable success) {
        mutate(() -> store.moveCapsules(
                Collections.singletonList(record.id), destination, expected(record)), success);
    }

    public void copy(List<CapsuleRecord> records, String destination, Runnable success) {
        mutate(() -> store.copyCapsules(ids(records), destination, expected(records)), success);
    }

    public void restore(List<CapsuleRecord> records, Runnable success) {
        mutate(() -> store.restoreCapsules(ids(records), expected(records)), success);
    }

    public void addTag(List<CapsuleRecord> records, String tag, Runnable success) {
        mutate(() -> store.addTags(
                ids(records), Collections.singletonList(tag), expected(records)), success);
    }

    public void addTag(CapsuleRecord record, String tag, Runnable success) {
        mutate(() -> store.addTags(
                Collections.singletonList(record.id), Collections.singletonList(tag),
                expected(record)), success);
    }

    public void removeTag(List<CapsuleRecord> records, String tag, Runnable success) {
        mutate(() -> store.removeTags(
                ids(records), Collections.singletonList(tag), expected(records)), success);
    }

    public void removeTag(CapsuleRecord record, String tag, Runnable success) {
        mutate(() -> store.removeTags(
                Collections.singletonList(record.id), Collections.singletonList(tag),
                expected(record)), success);
    }

    public void setFavorite(
            List<CapsuleRecord> records, boolean favorite, Runnable success) {
        mutate(() -> store.setFavorite(ids(records), favorite, expected(records)), success);
    }

    public void setFavorite(CapsuleRecord record, boolean favorite, Runnable success) {
        mutate(() -> store.setFavorite(
                Collections.singletonList(record.id), favorite, expected(record)), success);
    }

    public void setTitle(CapsuleRecord record, String title, Runnable success) {
        mutate(() -> store.setTitle(record.id, title, record.revision), success);
    }

    public void setFinalText(CapsuleRecord record, String text, Runnable success) {
        mutate(() -> store.setFinalText(record.id, text, record.revision), success);
    }

    public void requeue(CapsuleRecord record, Runnable success) {
        mutate(() -> store.requeueFailedTranscriptions(
                Collections.singletonList(record.id),
                Collections.singletonMap(
                        record.id.toLowerCase(java.util.Locale.ROOT),
                        record.processingRevision)), success);
    }

    public void delete(List<CapsuleRecord> records, Runnable success) {
        mutate(() -> store.deleteCapsules(ids(records), expected(records)), success);
    }

    public void purge(List<CapsuleRecord> records, Runnable success) {
        mutate(() -> store.purgeCapsules(ids(records), expected(records)), success);
    }

    private <T> void submit(Work<T> work, Result<T> result) {
        worker.execute(() -> {
            try {
                T value = work.run();
                main.post(() -> result.onSuccess(value));
            } catch (Exception error) {
                report(error);
            }
        });
    }

    private void mutate(Mutation mutation, Runnable success) {
        worker.execute(() -> {
            try {
                mutation.run();
                main.post(success);
            } catch (Exception error) {
                report(error);
            }
        });
    }

    private void report(Exception error) {
        String message = error.getMessage();
        main.post(() -> errors.onError(message == null ? error.getClass().getSimpleName() : message));
    }

    private static java.util.Map<String, Integer> expected(CapsuleRecord record) {
        return Collections.singletonMap(
                record.id.toLowerCase(java.util.Locale.ROOT), record.revision);
    }

    private static java.util.Map<String, Integer> expected(List<CapsuleRecord> records) {
        java.util.HashMap<String, Integer> values = new java.util.HashMap<>();
        for (CapsuleRecord record : records) {
            values.put(record.id.toLowerCase(java.util.Locale.ROOT), record.revision);
        }
        return values;
    }

    private static List<String> ids(List<CapsuleRecord> records) {
        ArrayList<String> values = new ArrayList<>();
        for (CapsuleRecord record : records) values.add(record.id);
        return values;
    }

    @Override public void close() {
        worker.shutdownNow();
    }

    private interface Work<T> {
        T run() throws IOException;
    }

    private interface Mutation {
        void run() throws Exception;
    }
}
