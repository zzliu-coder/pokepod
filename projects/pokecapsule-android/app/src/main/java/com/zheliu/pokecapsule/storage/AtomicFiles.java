package com.zheliu.pokecapsule.storage;

import android.util.AtomicFile;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;

public final class AtomicFiles {
    private AtomicFiles() {}

    public static void writeUtf8(File target, String content) throws IOException {
        File parent = target.getParentFile();
        if (parent == null || (!parent.isDirectory() && !parent.mkdirs())) {
            throw new IOException("无法创建父目录");
        }
        AtomicFile atomic = new AtomicFile(target);
        FileOutputStream stream = null;
        try {
            stream = atomic.startWrite();
            stream.write(content.getBytes(StandardCharsets.UTF_8));
            stream.flush();
            stream.getFD().sync();
            atomic.finishWrite(stream);
        } catch (IOException | RuntimeException error) {
            if (stream != null) atomic.failWrite(stream);
            throw error;
        }
    }
}
