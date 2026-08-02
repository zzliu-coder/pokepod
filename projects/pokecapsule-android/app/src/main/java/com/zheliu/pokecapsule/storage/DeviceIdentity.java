package com.zheliu.pokecapsule.storage;

import android.content.Context;
import android.os.Build;

import org.json.JSONObject;

import java.io.File;
import java.io.IOException;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;
import java.util.TimeZone;
import java.util.UUID;

public final class DeviceIdentity {
    private DeviceIdentity() {}

    public static JSONObject ensure(Context context, PokePaths paths) throws IOException {
        paths.ensureBase();
        File target = paths.deviceIdentity();
        if (target.isFile()) {
            try {
                JSONObject existing = CapsuleStore.readJson(target);
                if (existing.optInt("schemaVersion") == 1
                        && !existing.optString("deviceId").trim().isEmpty()) {
                    return existing;
                }
            } catch (Exception ignored) {
                // Replace a partial identity file with one complete atomic record.
            }
        }

        try {
            JSONObject value = new JSONObject();
            value.put("schemaVersion", 1);
            value.put("deviceId", UUID.randomUUID().toString().toLowerCase(Locale.ROOT));
            value.put("displayName", displayName());
            value.put("platform", "android");
            value.put("manufacturer", Build.MANUFACTURER);
            value.put("model", Build.MODEL);
            value.put("androidVersion", Build.VERSION.RELEASE);
            value.put("createdAt", now());
            AtomicFiles.writeUtf8(target, value.toString(2) + "\n");
            return value;
        } catch (Exception error) {
            throw new IOException("无法创建设备身份", error);
        }
    }

    static String displayName() {
        String model = Build.MODEL == null ? "" : Build.MODEL.trim();
        String manufacturer = Build.MANUFACTURER == null ? "" : Build.MANUFACTURER.trim();
        if ("V2303A".equalsIgnoreCase(model)) return "Vivo X Fold3";
        if (model.toLowerCase(Locale.ROOT).contains("poke3")) return "Poke3";
        if (model.isEmpty()) return manufacturer.isEmpty() ? "Android 设备" : manufacturer;
        if (manufacturer.isEmpty()
                || model.toLowerCase(Locale.ROOT).startsWith(manufacturer.toLowerCase(Locale.ROOT))) {
            return model;
        }
        return manufacturer + " " + model;
    }

    private static String now() {
        SimpleDateFormat formatter = new SimpleDateFormat(
                "yyyy-MM-dd'T'HH:mm:ss.SSS'Z'", Locale.US);
        formatter.setTimeZone(TimeZone.getTimeZone("UTC"));
        return formatter.format(new Date());
    }
}
