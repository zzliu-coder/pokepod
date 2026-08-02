package com.zheliu.pokecapsule.storage;

import android.content.Context;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;
import java.util.TimeZone;

public final class DiagnosticLog {
    private static final String TAG = "PokeCapsule";
    private static final long MAX_BYTES = 256 * 1024;

    private DiagnosticLog() {}

    public static synchronized void record(
            Context context, String stage, String code, Throwable error) {
        String message = error == null ? "" : safe(error.getMessage());
        String line = utcNow() + "\t" + safe(stage) + "\t" + safe(code)
                + "\t" + message + "\n";
        Log.w(TAG, line.trim());
        try {
            File directory = new File(context.getFilesDir(), "diagnostics");
            if (!directory.isDirectory() && !directory.mkdirs()) return;
            File log = new File(directory, "transcription.log");
            if (log.isFile() && log.length() > MAX_BYTES) {
                File previous = new File(directory, "transcription.previous.log");
                if (previous.exists()) previous.delete();
                log.renameTo(previous);
            }
            try (FileOutputStream output = new FileOutputStream(log, true)) {
                output.write(line.getBytes(StandardCharsets.UTF_8));
            }
        } catch (Exception ignored) {
            // Diagnostics must never interrupt capture or transcription.
        }
    }

    private static String safe(String value) {
        if (value == null) return "";
        String clean = value.replace('\n', ' ').replace('\r', ' ').replace('\t', ' ');
        return clean.length() > 1_000 ? clean.substring(0, 1_000) : clean;
    }

    private static String utcNow() {
        SimpleDateFormat format =
                new SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss'Z'", Locale.US);
        format.setTimeZone(TimeZone.getTimeZone("UTC"));
        return format.format(new Date());
    }
}
