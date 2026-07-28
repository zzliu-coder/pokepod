package com.zheliu.pokecapsule.model;

import com.zheliu.pokecapsule.core.ProcessingState;

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
    public final boolean favorite;
    public final List<String> tags;
    public final ProcessingState status;
    public final long durationMs;
    public final String error;
    public final boolean readOnly;

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
            boolean readOnly) {
        this.directory = directory;
        this.id = id;
        this.title = title;
        this.createdAt = createdAt;
        this.updatedAt = updatedAt;
        this.favorite = favorite;
        this.tags = Collections.unmodifiableList(new ArrayList<>(tags));
        this.status = status;
        this.durationMs = durationMs;
        this.error = error;
        this.readOnly = readOnly;
    }

    public static CapsuleRecord fromJson(File directory, JSONObject capsule, JSONObject processing) {
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
        ProcessingState state;
        try {
            state = ProcessingState.fromWire(processing.optString("status", "failed"));
        } catch (IllegalArgumentException ignored) {
            state = ProcessingState.FAILED;
        }
        return new CapsuleRecord(
                directory,
                capsule.optString("id", directory.getName()),
                capsule.optString("title", "未命名胶囊"),
                capsule.optString("createdAt", ""),
                capsule.optString("updatedAt", ""),
                capsule.optBoolean("favorite", false),
                tags,
                state,
                processing.optLong("durationMs", 0),
                processing.optString("error", ""),
                capsuleVersion != 1 || processingVersion != 1);
    }

    public String displayLine() {
        long seconds = Math.max(0, durationMs / 1000);
        String mark = favorite ? "★ " : "";
        String suffix = readOnly ? " · 新协议只读" : "";
        return mark + title + "\n" + createdAt + " · " + seconds + "秒 · " + status.wireValue() + suffix;
    }
}
