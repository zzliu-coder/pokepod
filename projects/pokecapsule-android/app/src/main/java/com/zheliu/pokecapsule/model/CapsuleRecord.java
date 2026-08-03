package com.zheliu.pokecapsule.model;

import com.zheliu.pokecapsule.core.ProcessingState;
import com.zheliu.pokecapsule.core.PathPolicy;
import com.zheliu.pokecapsule.core.TimeFormat;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

public final class CapsuleRecord {
    public final File directory;
    public final String id;
    public final String title;
    public final String createdAt;
    public final String updatedAt;
    public final int revision;
    public final boolean favorite;
    public final List<String> tags;
    public final ProcessingState status;
    public final int processingRevision;
    public final long durationMs;
    public final String error;
    public final boolean readOnly;
    public final String relativeFolder;
    public final String rawText;
    public final String polishedText;
    public final String finalText;
    public final boolean trashed;
    public final String trashedAt;
    public final String originalFolder;

    public CapsuleRecord(
            File directory,
            String id,
            String title,
            String createdAt,
            String updatedAt,
            int revision,
            boolean favorite,
            List<String> tags,
            ProcessingState status,
            int processingRevision,
            long durationMs,
            String error,
            boolean readOnly,
            String relativeFolder,
            String rawText,
            String polishedText,
            String finalText,
            boolean trashed,
            String trashedAt,
            String originalFolder) {
        this.directory = directory;
        this.id = id;
        this.title = title;
        this.createdAt = createdAt;
        this.updatedAt = updatedAt;
        this.revision = revision;
        this.favorite = favorite;
        this.tags = Collections.unmodifiableList(new ArrayList<>(tags));
        this.status = status;
        this.processingRevision = processingRevision;
        this.durationMs = durationMs;
        this.error = error;
        this.readOnly = readOnly;
        this.relativeFolder = relativeFolder;
        this.rawText = rawText;
        this.polishedText = polishedText;
        this.finalText = finalText;
        this.trashed = trashed;
        this.trashedAt = trashedAt;
        this.originalFolder = originalFolder;
    }

    public CapsuleRecord(
            File directory,
            String id,
            String title,
            String createdAt,
            String updatedAt,
            int revision,
            boolean favorite,
            List<String> tags,
            ProcessingState status,
            long durationMs,
            String error,
            boolean readOnly,
            String relativeFolder,
            String rawText,
            String polishedText,
            String finalText,
            boolean trashed,
            String trashedAt,
            String originalFolder) {
        this(directory, id, title, createdAt, updatedAt, revision, favorite, tags,
                status, revision, durationMs, error, readOnly, relativeFolder,
                rawText, polishedText, finalText, trashed, trashedAt, originalFolder);
    }

    public CapsuleRecord(
            File directory,
            String id,
            String title,
            String createdAt,
            String updatedAt,
            boolean favorite,
            List<String> tags,
            ProcessingState status,
            long durationMs,
            String error,
            boolean readOnly,
            String relativeFolder,
            String rawText,
            String polishedText) {
        this(directory, id, title, createdAt, updatedAt, 1, favorite, tags, status,
                1, durationMs, error, readOnly, relativeFolder, rawText, polishedText,
                "", false, "", "");
    }

    public static CapsuleRecord fromJson(
            File directory,
            JSONObject capsule,
            JSONObject processing,
            String relativeFolder,
            String rawText,
            String polishedText) {
        return fromJson(directory, capsule, processing, relativeFolder, rawText,
                polishedText, "", false, "", "", -1);
    }

    public static CapsuleRecord fromJson(
            File directory,
            JSONObject capsule,
            JSONObject processing,
            String relativeFolder,
            String rawText,
            String polishedText,
            String finalText,
            boolean trashed,
            String trashedAt,
            String originalFolder,
            int trashRevision) {
        ArrayList<String> tags = new ArrayList<>();
        JSONArray values = capsule.optJSONArray("tags");
        if (values != null) {
            for (int index = 0; index < values.length(); index++) {
                String tag = values.optString(index, "");
                if (!tag.isEmpty()) tags.add(tag);
            }
        }
        int capsuleVersion = capsule.optInt("schemaVersion", -1);
        int processingVersion = processing.optInt("schemaVersion", -1);
        String wireStatus = processing.optString("status", "");
        int processingRevision = processing.optInt("revision", -1);
        long durationMs = processing.optLong("durationMs", -1);
        boolean processingValid = hasValidProcessingState(
                wireStatus, processingRevision, durationMs);
        ProcessingState state = processingValid
                ? ProcessingState.fromWire(wireStatus)
                : ProcessingState.FAILED;
        String processingError = processing.isNull("error")
                ? "" : processing.optString("error", "");
        if (!processingValid && processingError.isEmpty()) {
            processingError = "处理状态文件损坏";
        }
        return new CapsuleRecord(
                directory,
                capsule.optString("id", directory.getName()),
                capsule.optString("title", "未命名胶囊"),
                capsule.optString("createdAt", ""),
                capsule.optString("updatedAt", ""),
                trashed && trashRevision >= 0
                        ? trashRevision : capsule.optInt("revision", 1),
                capsule.optBoolean("favorite", false),
                tags,
                state,
                processingRevision,
                Math.max(0, durationMs),
                processingError,
                capsuleVersion != 1 || processingVersion != 1 || !processingValid,
                relativeFolder,
                rawText,
                polishedText,
                finalText,
                trashed,
                trashedAt,
                originalFolder);
    }

    static boolean hasValidProcessingState(String status, int revision, long durationMs) {
        if (status == null || status.isEmpty() || revision < 0 || durationMs < 0) return false;
        try {
            ProcessingState.fromWire(status);
            return true;
        } catch (IllegalArgumentException ignored) {
            return false;
        }
    }

    public String displayLine() {
        StringBuilder output = new StringBuilder();
        output.append(previewText());
        output.append("\n").append(metadataText());
        String tags = tagsText();
        if (!tags.isEmpty()) output.append("\n").append(tags);
        return output.toString();
    }

    public String previewText() {
        String finalValue = clean(finalText);
        String raw = clean(rawText);
        String polished = clean(polishedText);
        if (!finalValue.isEmpty()) return finalValue;
        if (isPlausible(polished)
                && (raw.isEmpty() || isPlausibleCorrection(polished, raw))) {
            return polished;
        }
        if (isPlausible(raw)) return raw;
        if (!polished.isEmpty() || !raw.isEmpty()) return "转写结果异常，请播放录音";
        switch (status) {
            case RECORDING: return "正在录音…";
            case RECORDED:
            case QUEUED: return "等待自动转写";
            case TRANSCRIBING: return "正在转写…";
            case FAILED: return error == null || error.isEmpty() ? "转写失败" : "转写失败，可稍后重试";
            default: return title == null || title.isEmpty() ? "语音胶囊" : title;
        }
    }

    public String metadataText() {
        long seconds = Math.max(0, durationMs / 1000);
        StringBuilder output = new StringBuilder();
        output.append(TimeFormat.localDisplay(createdAt))
                .append(" · ").append(displayFolder(relativeFolder))
                .append(" · ").append(seconds).append("秒");
        String state = visibleState();
        if (!state.isEmpty()) output.append(" · ").append(state);
        if (readOnly) output.append(" · 只读");
        if (trashed) output.append(" · 已删除");
        return output.toString();
    }

    private static String displayFolder(String folder) {
        if (PathPolicy.INBOX.equals(folder)) return "收件箱";
        if (PathPolicy.ARCHIVE.equals(folder)) return "归档";
        return folder;
    }

    public String tagsText() {
        StringBuilder output = new StringBuilder();
        for (int index = 0; index < tags.size(); index++) {
            if (index > 0) output.append("  ");
            output.append("#").append(tags.get(index));
        }
        return output.toString();
    }

    private String visibleState() {
        switch (status) {
            case RECORDING: return "录音中";
            case RECORDED:
            case QUEUED: return "等待自动转写";
            case TRANSCRIBING: return "转写中";
            case FAILED: return "转写失败";
            default: return "";
        }
    }

    private static String clean(String value) {
        if (value == null) return "";
        return value.trim().replaceAll("\\s+", " ");
    }

    private boolean isPlausible(String value) {
        if (value.isEmpty() || durationMs < 2_000) return false;
        int characters = value.codePointCount(0, value.length());
        int limit = Math.max(24, (int) Math.ceil(durationMs / 1000.0 * 10));
        return characters <= limit;
    }

    private static boolean isPlausibleCorrection(String polished, String raw) {
        int polishedLength = polished.codePointCount(0, polished.length());
        int rawLength = raw.codePointCount(0, raw.length());
        int limit = Math.max(rawLength + 20, rawLength * 2);
        return polishedLength <= limit;
    }
}
