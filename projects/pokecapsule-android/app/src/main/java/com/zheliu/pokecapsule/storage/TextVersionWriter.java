package com.zheliu.pokecapsule.storage;

import com.zheliu.pokecapsule.core.TimeFormat;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.File;
import java.io.IOException;

/** Atomic derived-text writer. CapsuleStore remains the public locking facade. */
final class TextVersionWriter {
    void writeFinal(File directory, JSONObject capsule, String text) throws IOException {
        File finalFile = new File(directory, "final.md");
        File capsuleFile = new File(directory, "capsule.json");
        boolean hadFinal = finalFile.isFile();
        String oldFinal = hadFinal ? CapsuleStore.readUtf8(finalFile) : null;
        String oldCapsule = CapsuleStore.readUtf8(capsuleFile);
        try {
            capsule.put("finalTextFile", "final.md");
            capsule.put("revision", capsule.optInt("revision", 0) + 1);
            capsule.put("updatedAt", TimeFormat.utcNow());
        } catch (JSONException error) {
            throw new IOException("无法更新最终文字元数据", error);
        }
        try {
            AtomicFiles.writeUtf8(finalFile, text);
            AtomicFiles.writeUtf8(capsuleFile, CapsuleStore.prettyJson(capsule));
        } catch (IOException error) {
            try {
                if (hadFinal) AtomicFiles.writeUtf8(finalFile, oldFinal);
                else if (finalFile.exists() && !finalFile.delete()) {
                    throw new IOException("无法移除未提交的最终文字");
                }
                AtomicFiles.writeUtf8(capsuleFile, oldCapsule);
            } catch (IOException rollback) {
                error.addSuppressed(rollback);
            }
            throw error;
        }
    }
}
