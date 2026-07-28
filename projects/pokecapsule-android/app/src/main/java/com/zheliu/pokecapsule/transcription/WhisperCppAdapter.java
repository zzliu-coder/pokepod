package com.zheliu.pokecapsule.transcription;

import java.io.File;
import java.io.IOException;

public final class WhisperCppAdapter implements WhisperAdapter {
    @Override public String transcribe(File model, File audio) throws IOException {
        float[] samples = AudioDecoder.decode16kMono(audio);
        if (samples.length == 0) throw new IOException("解码后没有 PCM 样本");
        try {
            String result = WhisperNative.transcribe(model.getAbsolutePath(), samples, 2);
            if (result == null || result.trim().isEmpty()) {
                throw new IOException("Whisper 返回空文本");
            }
            return result.trim();
        } catch (UnsatisfiedLinkError error) {
            throw new IOException("Whisper 原生库未加载", error);
        } catch (IllegalStateException error) {
            throw new IOException(error.getMessage(), error);
        }
    }
}
