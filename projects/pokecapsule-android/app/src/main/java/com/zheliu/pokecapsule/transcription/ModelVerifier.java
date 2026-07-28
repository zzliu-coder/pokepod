package com.zheliu.pokecapsule.transcription;

import com.zheliu.pokecapsule.storage.PokePaths;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;

public final class ModelVerifier {
    public static final String FILE_NAME = "ggml-tiny-q5_1.bin";
    public static final String SHA256 =
            "818710568da3ca15689e31a743197b520007872ff9576237bda97bd1b469c3d7";

    private ModelVerifier() {}

    public static File modelFile(PokePaths paths) {
        return new File(new File(paths.root(), ".models"), FILE_NAME);
    }

    public static Verification verify(PokePaths paths) {
        File model = modelFile(paths);
        if (!model.isFile()) return new Verification(false, model, "模型未安装");
        try {
            String actual = sha256(model);
            if (!SHA256.equals(actual)) {
                return new Verification(false, model, "模型 SHA-256 不匹配");
            }
            return new Verification(true, model, "模型校验通过");
        } catch (IOException error) {
            return new Verification(false, model, "模型读取失败: " + error.getMessage());
        }
    }

    private static String sha256(File file) throws IOException {
        try {
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            try (FileInputStream input = new FileInputStream(file)) {
                byte[] buffer = new byte[1024 * 1024];
                int count;
                while ((count = input.read(buffer)) >= 0) digest.update(buffer, 0, count);
            }
            StringBuilder result = new StringBuilder();
            for (byte value : digest.digest()) result.append(String.format("%02x", value));
            return result.toString();
        } catch (NoSuchAlgorithmException impossible) {
            throw new IOException("系统缺少 SHA-256", impossible);
        }
    }

    public static final class Verification {
        public final boolean valid;
        public final File file;
        public final String message;

        Verification(boolean valid, File file, String message) {
            this.valid = valid;
            this.file = file;
            this.message = message;
        }
    }
}
