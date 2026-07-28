package com.zheliu.pokecapsule.command;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

import com.zheliu.pokecapsule.core.Ids;
import com.zheliu.pokecapsule.core.TimeFormat;
import com.zheliu.pokecapsule.service.TranscriptionScheduler;
import com.zheliu.pokecapsule.storage.AtomicFiles;
import com.zheliu.pokecapsule.storage.CapsuleStore;
import com.zheliu.pokecapsule.storage.MaintenanceSession;
import com.zheliu.pokecapsule.storage.PokePaths;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public final class CommandReceiver extends BroadcastReceiver {
    public static final String EXTRA_COMMAND_FILE = "commandFile";
    private static final ExecutorService EXECUTOR = Executors.newSingleThreadExecutor();

    @Override public void onReceive(Context context, Intent intent) {
        PendingResult pending = goAsync();
        String fileName = intent == null ? null : intent.getStringExtra(EXTRA_COMMAND_FILE);
        EXECUTOR.execute(() -> {
            try {
                process(context.getApplicationContext(), fileName);
            } finally {
                pending.finish();
            }
        });
    }

    static void process(Context context, String fileName) {
        PokePaths paths = new PokePaths();
        String commandId = commandId(fileName);
        JSONObject result = new JSONObject();
        File commandFile = null;
        File inflightFile = null;
        try {
            paths.ensureBase();
            if (commandId == null) throw new Exception("命令文件名必须是 UUID.json");
            commandFile = new File(paths.commands(), fileName);
            paths.assertInsideRoot(commandFile);
            File results = new File(paths.commands(), "results");
            File existingResult = new File(results, commandId + ".json");
            if (existingResult.isFile()) {
                commandFile.delete();
                return;
            }
            JSONObject command = CapsuleStore.readJson(commandFile);
            if (command.optInt("schemaVersion", -1) != 1) throw new Exception("不支持的命令协议");
            String jsonId = command.optString("transactionId", command.optString("commandId"));
            if (!commandId.equalsIgnoreCase(jsonId)) {
                throw new Exception("transactionId 与文件名不一致");
            }
            File inflight = new File(paths.commands(), "inflight");
            if (!inflight.isDirectory() && !inflight.mkdirs()) {
                throw new Exception("无法创建命令事务目录");
            }
            inflightFile = new File(inflight, commandId + ".json");
            if (inflightFile.exists()) {
                throw new Exception("上次执行结果不确定，已拒绝重复执行；请重新同步检查");
            }
            AtomicFiles.writeUtf8(inflightFile, command.toString(2) + "\n");
            execute(paths, new CapsuleStore(paths), command);
            result.put("schemaVersion", 1);
            result.put("commandId", commandId);
            result.put("transactionId", commandId);
            result.put("ok", true);
            result.put("success", true);
            result.put("message", "committed");
            result.put("completedAt", TimeFormat.utcNow());
            TranscriptionScheduler.scheduleAutomatic(context);
        } catch (Exception error) {
            try {
                result.put("schemaVersion", 1);
                result.put("commandId", commandId == null ? JSONObject.NULL : commandId);
                result.put("transactionId", commandId == null ? JSONObject.NULL : commandId);
                result.put("ok", false);
                result.put("success", false);
                result.put("message", safeMessage(error));
                result.put("completedAt", TimeFormat.utcNow());
            } catch (Exception ignored) {
            }
        }
        try {
            File results = new File(paths.commands(), "results");
            if (!results.isDirectory() && !results.mkdirs()) return;
            String outputName = (commandId == null ? "invalid-" + System.currentTimeMillis() : commandId)
                    + ".json";
            AtomicFiles.writeUtf8(new File(results, outputName), result.toString(2) + "\n");
            if (commandFile != null) commandFile.delete();
            if (inflightFile != null) inflightFile.delete();
        } catch (Exception ignored) {
        }
    }

    private static void execute(PokePaths paths, CapsuleStore store, JSONObject command) throws Exception {
        String operation = command.getString("operation");
        List<String> ids = ids(command.optJSONArray("capsuleIds"));
        switch (operation) {
            case "beginMaintenance":
                MaintenanceSession.begin(paths, command.optString("maintenanceId"));
                return;
            case "endMaintenance":
                MaintenanceSession.end(paths, command.optString("maintenanceId"));
                return;
            case "rescan":
                store.scan();
                return;
            default:
                MaintenanceSession.requireOwner(
                        paths, command.optString("maintenanceId"));
                break;
        }
        switch (operation) {
            case "mkdir":
            case "createFolder":
                store.createFolder(command.optString("folderPath", command.optString("folder")));
                return;
            case "rmdir":
            case "deleteFolderToInbox":
                store.deleteFolderMovingContentsToInbox(
                        command.optString("folderPath", command.optString("folder")));
                return;
            case "renameFolder":
                store.renameFolder(command.getString("folderPath"), command.getString("newFolderPath"));
                return;
            case "move":
            case "moveCapsules":
                requireIds(ids);
                store.moveCapsules(ids, command.getString("destination"));
                return;
            case "copy":
            case "copyCapsules":
                requireIds(ids);
                store.copyCapsules(ids, command.getString("destination"));
                return;
            case "delete":
            case "deleteCapsules":
                requireIds(ids);
                store.deleteCapsules(ids);
                return;
            case "favorite":
            case "setFavorite":
                requireIds(ids);
                store.setFavorite(ids, command.has("favorite")
                        ? command.getBoolean("favorite") : command.getBoolean("value"));
                return;
            case "tag_add":
            case "addTags":
                requireIds(ids);
                for (String tag : strings(command.optJSONArray("tags"), command.optString("tag"))) {
                    store.addTag(ids, tag);
                }
                return;
            case "tag_remove":
            case "removeTags":
                requireIds(ids);
                for (String tag : strings(command.optJSONArray("tags"), command.optString("tag"))) {
                    store.removeTag(ids, tag);
                }
                return;
            case "tag_rename":
            case "renameTag":
            case "mergeTag": {
                List<String> tags = strings(command.optJSONArray("tags"), null);
                if (tags.size() != 2) throw new Exception("标签改名需要两个标签");
                store.renameTag(tags.get(0), tags.get(1));
                return;
            }
            case "deleteTag": {
                List<String> tags = strings(command.optJSONArray("tags"), null);
                if (tags.size() != 1) throw new Exception("删除标签需要一个标签");
                store.deleteTag(tags.get(0));
                return;
            }
            case "commitImport":
                requireIds(ids);
                if (ids.size() != 1) throw new Exception("每次只提交一个导入胶囊");
                store.commitImportedCapsule(
                        command.getString("stagedPath"),
                        ids.get(0),
                        command.getString("destination"));
                return;
            case "commitCorrection":
                requireIds(ids);
                if (ids.size() != 1) throw new Exception("每次只提交一个校对结果");
                store.commitCorrection(
                        command.getString("stagedPath"),
                        ids.get(0),
                        command.optInt("expectedRevision", -1));
                return;
            default:
                throw new Exception("未知命令: " + operation);
        }
    }

    private static List<String> strings(JSONArray array, String fallback) throws Exception {
        ArrayList<String> result = new ArrayList<>();
        if (array != null) {
            for (int index = 0; index < array.length(); index++) result.add(array.getString(index));
        } else if (fallback != null && !fallback.isEmpty()) {
            result.add(fallback);
        }
        if (result.isEmpty()) throw new Exception("缺少参数");
        return result;
    }

    private static List<String> ids(JSONArray array) throws Exception {
        ArrayList<String> result = new ArrayList<>();
        if (array == null) return result;
        for (int index = 0; index < array.length(); index++) {
            String id = array.getString(index);
            if (!Ids.isUuid(id)) throw new Exception("无效胶囊 UUID");
            result.add(id);
        }
        return result;
    }

    private static void requireIds(List<String> ids) throws Exception {
        if (ids.isEmpty() || ids.size() > 500) throw new Exception("胶囊数量必须为 1–500");
    }

    private static String commandId(String fileName) {
        if (fileName == null || !fileName.endsWith(".json")
                || fileName.contains("/") || fileName.contains("\\") || fileName.startsWith(".")) {
            return null;
        }
        String value = fileName.substring(0, fileName.length() - 5);
        return Ids.isUuid(value) ? value : null;
    }

    private static String safeMessage(Throwable error) {
        String message = error.getMessage();
        if (message == null || message.isEmpty()) message = error.getClass().getSimpleName();
        return message.length() > 500 ? message.substring(0, 500) : message;
    }
}
