package com.zheliu.pokecapsule.transcription;

import android.media.MediaCodec;
import android.media.MediaExtractor;
import android.media.MediaFormat;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

public final class AudioDecoder {
    private static final long TIMEOUT_US = 10_000;

    private AudioDecoder() {}

    public static float[] decode16kMono(File audio) throws IOException {
        MediaExtractor extractor = new MediaExtractor();
        MediaCodec codec = null;
        try {
            extractor.setDataSource(audio.getAbsolutePath());
            int track = findAudioTrack(extractor);
            if (track < 0) throw new IOException("录音没有音频轨");
            extractor.selectTrack(track);
            MediaFormat sourceFormat = extractor.getTrackFormat(track);
            String mime = sourceFormat.getString(MediaFormat.KEY_MIME);
            if (mime == null) throw new IOException("音频编码未知");
            codec = MediaCodec.createDecoderByType(mime);
            codec.configure(sourceFormat, null, null, 0);
            codec.start();

            ByteArrayOutputStream pcmBytes = new ByteArrayOutputStream();
            MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
            boolean inputEnded = false;
            boolean outputEnded = false;
            int sampleRate = sourceFormat.containsKey(MediaFormat.KEY_SAMPLE_RATE)
                    ? sourceFormat.getInteger(MediaFormat.KEY_SAMPLE_RATE) : 16000;
            int channelCount = sourceFormat.containsKey(MediaFormat.KEY_CHANNEL_COUNT)
                    ? sourceFormat.getInteger(MediaFormat.KEY_CHANNEL_COUNT) : 1;

            while (!outputEnded) {
                if (!inputEnded) {
                    int inputIndex = codec.dequeueInputBuffer(TIMEOUT_US);
                    if (inputIndex >= 0) {
                        ByteBuffer input = codec.getInputBuffer(inputIndex);
                        if (input == null) throw new IOException("无法取得解码输入缓冲区");
                        int size = extractor.readSampleData(input, 0);
                        if (size < 0) {
                            codec.queueInputBuffer(inputIndex, 0, 0, 0,
                                    MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                            inputEnded = true;
                        } else {
                            codec.queueInputBuffer(inputIndex, 0, size,
                                    extractor.getSampleTime(), 0);
                            extractor.advance();
                        }
                    }
                }

                int outputIndex = codec.dequeueOutputBuffer(info, TIMEOUT_US);
                if (outputIndex == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                    MediaFormat outputFormat = codec.getOutputFormat();
                    sampleRate = outputFormat.getInteger(MediaFormat.KEY_SAMPLE_RATE);
                    channelCount = outputFormat.getInteger(MediaFormat.KEY_CHANNEL_COUNT);
                } else if (outputIndex >= 0) {
                    ByteBuffer output = codec.getOutputBuffer(outputIndex);
                    if (output != null && info.size > 0) {
                        byte[] bytes = new byte[info.size];
                        output.position(info.offset);
                        output.limit(info.offset + info.size);
                        output.get(bytes);
                        pcmBytes.write(bytes, 0, bytes.length);
                    }
                    outputEnded = (info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0;
                    codec.releaseOutputBuffer(outputIndex, false);
                }
            }
            return convertPcm16(pcmBytes.toByteArray(), sampleRate, channelCount);
        } finally {
            extractor.release();
            if (codec != null) {
                try {
                    codec.stop();
                } catch (IllegalStateException ignored) {
                }
                codec.release();
            }
        }
    }

    private static int findAudioTrack(MediaExtractor extractor) {
        for (int index = 0; index < extractor.getTrackCount(); index++) {
            String mime = extractor.getTrackFormat(index).getString(MediaFormat.KEY_MIME);
            if (mime != null && mime.startsWith("audio/")) return index;
        }
        return -1;
    }

    private static float[] convertPcm16(byte[] bytes, int sampleRate, int channels) throws IOException {
        if (channels < 1 || sampleRate < 1 || bytes.length < 2) {
            throw new IOException("解码后的 PCM 参数无效");
        }
        ByteBuffer buffer = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN);
        int frameCount = bytes.length / 2 / channels;
        float[] mono = new float[frameCount];
        for (int frame = 0; frame < frameCount; frame++) {
            float sum = 0;
            for (int channel = 0; channel < channels; channel++) {
                sum += buffer.getShort() / 32768.0f;
            }
            mono[frame] = sum / channels;
        }
        if (sampleRate == 16000) return mono;
        int outputCount = Math.max(1, Math.round(mono.length * 16000f / sampleRate));
        float[] result = new float[outputCount];
        for (int index = 0; index < outputCount; index++) {
            float source = index * (sampleRate / 16000f);
            int left = Math.min(mono.length - 1, (int) source);
            int right = Math.min(mono.length - 1, left + 1);
            float fraction = source - left;
            result[index] = mono[left] * (1f - fraction) + mono[right] * fraction;
        }
        return result;
    }
}
