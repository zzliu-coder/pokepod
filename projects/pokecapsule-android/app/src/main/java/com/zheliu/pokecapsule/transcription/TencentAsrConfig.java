package com.zheliu.pokecapsule.transcription;

import android.content.Context;
import android.content.SharedPreferences;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import android.util.Base64;

import java.nio.charset.StandardCharsets;
import java.security.KeyStore;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;

public final class TencentAsrConfig {
    private static final String ALIAS = "pokecapsule.tencent.asr";
    private static final String PREFS = "cloud_asr";
    private static final Pattern ID = Pattern.compile(
            "(?im)^\\s*SecretId\\b[ \\t]*(?:[:=：][ \\t]*)?[\"'`]?([A-Za-z0-9]+)");
    private static final Pattern KEY = Pattern.compile(
            "(?im)^\\s*SecretKey\\b[ \\t]*(?:[:=：][ \\t]*)?[\"'`]?([A-Za-z0-9]+)");

    public final String secretId;
    public final String secretKey;

    TencentAsrConfig(String secretId, String secretKey) {
        this.secretId = secretId;
        this.secretKey = secretKey;
    }

    public static TencentAsrConfig parse(String text) {
        Matcher id = ID.matcher(text == null ? "" : text);
        Matcher key = KEY.matcher(text == null ? "" : text);
        if (!id.find() || !key.find()) {
            throw new IllegalArgumentException("密钥文件缺少 SecretId 或 SecretKey");
        }
        return new TencentAsrConfig(id.group(1), key.group(1));
    }

    public static void save(Context context, TencentAsrConfig credentials) throws Exception {
        Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
        cipher.init(Cipher.ENCRYPT_MODE, getOrCreateKey());
        byte[] plain = (credentials.secretId + "\n" + credentials.secretKey)
                .getBytes(StandardCharsets.UTF_8);
        byte[] encrypted = cipher.doFinal(plain);
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
                .putString("iv", Base64.encodeToString(cipher.getIV(), Base64.NO_WRAP))
                .putString("data", Base64.encodeToString(encrypted, Base64.NO_WRAP))
                .apply();
    }

    public static TencentAsrConfig load(Context context) throws Exception {
        SharedPreferences prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        String iv = prefs.getString("iv", "");
        String data = prefs.getString("data", "");
        if (iv.isEmpty() || data.isEmpty()) throw new IllegalStateException("尚未配置腾讯语音识别");
        Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
        cipher.init(Cipher.DECRYPT_MODE, getOrCreateKey(),
                new GCMParameterSpec(128, Base64.decode(iv, Base64.NO_WRAP)));
        String[] values = new String(
                cipher.doFinal(Base64.decode(data, Base64.NO_WRAP)),
                StandardCharsets.UTF_8).split("\\n", 2);
        if (values.length != 2) throw new IllegalStateException("腾讯语音识别配置损坏");
        return new TencentAsrConfig(values[0], values[1]);
    }

    public static boolean isConfigured(Context context) {
        try {
            load(context);
            return true;
        } catch (Exception ignored) {
            return false;
        }
    }

    public String exportForTransfer() {
        return "SecretId=" + secretId + "\nSecretKey=" + secretKey + "\n";
    }

    private static SecretKey getOrCreateKey() throws Exception {
        KeyStore store = KeyStore.getInstance("AndroidKeyStore");
        store.load(null);
        java.security.Key existing = store.getKey(ALIAS, null);
        if (existing instanceof SecretKey) return (SecretKey) existing;
        KeyGenerator generator = KeyGenerator.getInstance(
                KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore");
        generator.init(new KeyGenParameterSpec.Builder(
                ALIAS, KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .build());
        return generator.generateKey();
    }
}
