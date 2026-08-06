package com.zheliu.pokecapsule.transcription;

import android.util.Base64;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;
import java.util.TimeZone;

import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;

public final class TencentAsrClient {
    private static final String HOST = "asr.tencentcloudapi.com";
    private static final String SERVICE = "asr";
    private static final String ACTION = "SentenceRecognition";
    private static final String VERSION = "2019-06-14";
    private static final String REGION = "ap-guangzhou";

    public String transcribe(File audio, TencentAsrConfig credentials) throws Exception {
        if (!audio.isFile() || audio.length() == 0 || audio.length() > 3 * 1024 * 1024) {
            throw new IllegalArgumentException("录音为空或超过腾讯一句话识别 3MB 限制");
        }
        byte[] bytes = readAll(audio);
        JSONObject request = new JSONObject();
        request.put("ProjectId", 0);
        request.put("SubServiceType", 2);
        request.put("EngSerViceType", "16k_zh");
        request.put("SourceType", 1);
        request.put("VoiceFormat", voiceFormatFor(audio));
        request.put("Data", Base64.encodeToString(bytes, Base64.NO_WRAP));
        request.put("DataLen", bytes.length);
        request.put("FilterDirty", 0);
        request.put("FilterModal", 0);
        request.put("FilterPunc", 0);
        request.put("ConvertNumMode", 1);
        String payload = request.toString();
        long timestamp = System.currentTimeMillis() / 1000L;

        HttpURLConnection connection = (HttpURLConnection)
                new URL("https://" + HOST).openConnection();
        connection.setConnectTimeout(20_000);
        connection.setReadTimeout(45_000);
        connection.setRequestMethod("POST");
        connection.setDoOutput(true);
        connection.setRequestProperty("Content-Type", "application/json; charset=utf-8");
        connection.setRequestProperty("Host", HOST);
        connection.setRequestProperty("X-TC-Action", ACTION);
        connection.setRequestProperty("X-TC-Version", VERSION);
        connection.setRequestProperty("X-TC-Region", REGION);
        connection.setRequestProperty("X-TC-Timestamp", Long.toString(timestamp));
        connection.setRequestProperty("Authorization",
                authorization(payload, timestamp, credentials));
        try (OutputStream output = connection.getOutputStream()) {
            output.write(payload.getBytes(StandardCharsets.UTF_8));
        }
        int code = connection.getResponseCode();
        InputStream stream = code >= 200 && code < 300
                ? connection.getInputStream() : connection.getErrorStream();
        String body = new String(readAll(stream), StandardCharsets.UTF_8);
        JSONObject response = new JSONObject(body).getJSONObject("Response");
        if (response.has("Error")) {
            JSONObject error = response.getJSONObject("Error");
            throw new AsrException(error.optString("Code", "TencentError"),
                    error.optString("Message", "腾讯语音识别失败"));
        }
        String result = response.optString("Result", "").trim();
        if (result.isEmpty()) throw new AsrException("EmptyResult", "腾讯语音识别返回空文本");
        return result;
    }

    static String authorization(
            String payload, long timestamp, TencentAsrConfig credentials) throws Exception {
        String date = utcDate(timestamp);
        String canonicalHeaders = "content-type:application/json; charset=utf-8\n"
                + "host:" + HOST + "\n"
                + "x-tc-action:" + ACTION.toLowerCase(Locale.ROOT) + "\n";
        String signedHeaders = "content-type;host;x-tc-action";
        String canonicalRequest = "POST\n/\n\n" + canonicalHeaders + "\n"
                + signedHeaders + "\n" + sha256Hex(payload.getBytes(StandardCharsets.UTF_8));
        String scope = date + "/" + SERVICE + "/tc3_request";
        String stringToSign = "TC3-HMAC-SHA256\n" + timestamp + "\n" + scope + "\n"
                + sha256Hex(canonicalRequest.getBytes(StandardCharsets.UTF_8));
        byte[] secretDate = hmac(("TC3" + credentials.secretKey)
                .getBytes(StandardCharsets.UTF_8), date);
        byte[] secretService = hmac(secretDate, SERVICE);
        byte[] secretSigning = hmac(secretService, "tc3_request");
        String signature = hex(hmac(secretSigning, stringToSign));
        return "TC3-HMAC-SHA256 Credential=" + credentials.secretId + "/" + scope
                + ", SignedHeaders=" + signedHeaders + ", Signature=" + signature;
    }

    static String voiceFormatFor(File audio) {
        String name = audio.getName().toLowerCase(Locale.ROOT);
        if (name.endsWith(".m4a")) return "m4a";
        if (name.endsWith(".wav")) return "wav";
        throw new IllegalArgumentException("腾讯一句话识别不支持此音频格式");
    }

    private static String utcDate(long timestamp) {
        SimpleDateFormat format = new SimpleDateFormat("yyyy-MM-dd", Locale.US);
        format.setTimeZone(TimeZone.getTimeZone("UTC"));
        return format.format(new Date(timestamp * 1000L));
    }

    private static byte[] hmac(byte[] key, String value) throws Exception {
        Mac mac = Mac.getInstance("HmacSHA256");
        mac.init(new SecretKeySpec(key, "HmacSHA256"));
        return mac.doFinal(value.getBytes(StandardCharsets.UTF_8));
    }

    private static String sha256Hex(byte[] value) throws Exception {
        return hex(MessageDigest.getInstance("SHA-256").digest(value));
    }

    private static String hex(byte[] value) {
        StringBuilder result = new StringBuilder(value.length * 2);
        for (byte item : value) result.append(String.format(Locale.US, "%02x", item & 0xff));
        return result.toString();
    }

    private static byte[] readAll(File file) throws Exception {
        try (InputStream input = new FileInputStream(file)) {
            return readAll(input);
        }
    }

    private static byte[] readAll(InputStream input) throws Exception {
        if (input == null) return new byte[0];
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        byte[] buffer = new byte[16 * 1024];
        int count;
        while ((count = input.read(buffer)) >= 0) output.write(buffer, 0, count);
        return output.toByteArray();
    }

    public static final class AsrException extends Exception {
        public final String code;
        AsrException(String code, String message) {
            super(code + ": " + message);
            this.code = code;
        }
    }
}
