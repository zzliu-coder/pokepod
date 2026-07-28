package com.zheliu.pokecapsule.service;

import android.annotation.SuppressLint;
import android.media.AudioFormat;
import android.media.AudioRecord;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaFormat;
import android.media.MediaMuxer;
import android.media.MediaRecorder;
import android.os.Process;

import com.zheliu.pokecapsule.core.AdaptiveVoiceGain;

import java.io.File;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

final class EnhancedAudioRecorder {
    private static final int SAMPLE_RATE = 16_000;
    private static final int CHANNELS = 1;
    private static final int BIT_RATE = 32_000;
    private static final int SAMPLES_PER_BLOCK = 320;
    private static final String MIME = MediaFormat.MIMETYPE_AUDIO_AAC;

    private final File output;
    private final File original;
    private final CountDownLatch ready = new CountDownLatch(1);
    private final CountDownLatch finished = new CountDownLatch(1);
    private final AtomicInteger latestPeak = new AtomicInteger();

    private volatile boolean stopRequested;
    private volatile Throwable failure;
    private volatile AudioRecord audioRecord;
    private Thread worker;

    EnhancedAudioRecorder(File output, File original) {
        this.output = output;
        this.original = original;
    }

    void start() throws Exception {
        worker = new Thread(this::recordLoop, "PokeCapsule-Audio");
        worker.start();
        if (!ready.await(5, TimeUnit.SECONDS)) {
            abort();
            throw new IllegalStateException("录音设备启动超时");
        }
        if (failure != null) throw new IllegalStateException(safeMessage(failure), failure);
    }

    void stop() throws Exception {
        stopRequested = true;
        stopAudioRecord();
        if (!finished.await(10, TimeUnit.SECONDS)) {
            throw new IllegalStateException("录音文件收尾超时");
        }
        if (failure != null) throw new IllegalStateException(safeMessage(failure), failure);
    }

    void abort() {
        stopRequested = true;
        stopAudioRecord();
        if (worker != null) {
            try {
                finished.await(2, TimeUnit.SECONDS);
            } catch (InterruptedException interrupted) {
                Thread.currentThread().interrupt();
            }
        }
    }

    int getMaxAmplitude() {
        return latestPeak.getAndSet(0);
    }

    private void recordLoop() {
        Process.setThreadPriority(Process.THREAD_PRIORITY_AUDIO);
        AudioRecord capture = null;
        MediaCodec encoder = null;
        MediaMuxer muxer = null;
        WavWriter wav = null;
        boolean muxerStarted = false;
        int trackIndex = -1;
        long submittedSamples = 0;
        try {
            capture = createAudioRecord();
            audioRecord = capture;
            encoder = createEncoder();
            muxer = new MediaMuxer(
                    output.getAbsolutePath(), MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4);
            wav = new WavWriter(original, SAMPLE_RATE, CHANNELS);
            AdaptiveVoiceGain voiceGain = new AdaptiveVoiceGain();
            short[] samples = new short[SAMPLES_PER_BLOCK];

            capture.startRecording();
            if (capture.getRecordingState() != AudioRecord.RECORDSTATE_RECORDING) {
                throw new IllegalStateException("麦克风未进入录音状态");
            }
            ready.countDown();

            while (!stopRequested) {
                int count = capture.read(samples, 0, samples.length);
                if (count < 0 && stopRequested) break;
                if (count < 0) throw new IllegalStateException("麦克风读取失败: " + count);
                if (count == 0) continue;
                wav.write(samples, count);
                int peak = voiceGain.process(samples, count);
                rememberPeak(peak);
                submittedSamples = queuePcm(encoder, samples, count, submittedSamples, false);
                MuxerState state = drainEncoder(encoder, muxer, trackIndex, muxerStarted, false);
                trackIndex = state.trackIndex;
                muxerStarted = state.started;
            }

            submittedSamples = queuePcm(
                    encoder, new short[0], 0, submittedSamples, true);
            MuxerState state = drainEncoder(
                    encoder, muxer, trackIndex, muxerStarted, true);
            muxerStarted = state.started;
        } catch (Throwable error) {
            failure = error;
            ready.countDown();
        } finally {
            if (capture != null) {
                try {
                    if (capture.getRecordingState() == AudioRecord.RECORDSTATE_RECORDING) {
                        capture.stop();
                    }
                } catch (RuntimeException ignored) {
                }
                capture.release();
            }
            audioRecord = null;
            if (encoder != null) {
                try {
                    encoder.stop();
                } catch (RuntimeException ignored) {
                }
                encoder.release();
            }
            if (muxer != null) {
                if (muxerStarted) {
                    try {
                        muxer.stop();
                    } catch (RuntimeException ignored) {
                    }
                }
                muxer.release();
            }
            if (wav != null) {
                try {
                    wav.close();
                } catch (Exception closeError) {
                    if (failure == null) failure = closeError;
                }
            }
            ready.countDown();
            finished.countDown();
        }
    }

    @SuppressLint("MissingPermission")
    private AudioRecord createAudioRecord() {
        int minimum = AudioRecord.getMinBufferSize(
                SAMPLE_RATE,
                AudioFormat.CHANNEL_IN_MONO,
                AudioFormat.ENCODING_PCM_16BIT);
        if (minimum <= 0) throw new IllegalStateException("设备不支持 16kHz 单声道录音");
        int bufferBytes = Math.max(minimum * 2, SAMPLES_PER_BLOCK * 8);
        AudioRecord preferred = new AudioRecord(
                MediaRecorder.AudioSource.VOICE_RECOGNITION,
                SAMPLE_RATE,
                AudioFormat.CHANNEL_IN_MONO,
                AudioFormat.ENCODING_PCM_16BIT,
                bufferBytes);
        if (preferred.getState() == AudioRecord.STATE_INITIALIZED) return preferred;
        preferred.release();

        AudioRecord fallback = new AudioRecord(
                MediaRecorder.AudioSource.MIC,
                SAMPLE_RATE,
                AudioFormat.CHANNEL_IN_MONO,
                AudioFormat.ENCODING_PCM_16BIT,
                bufferBytes);
        if (fallback.getState() != AudioRecord.STATE_INITIALIZED) {
            fallback.release();
            throw new IllegalStateException("麦克风初始化失败");
        }
        return fallback;
    }

    private MediaCodec createEncoder() throws Exception {
        MediaFormat format = MediaFormat.createAudioFormat(MIME, SAMPLE_RATE, CHANNELS);
        format.setInteger(
                MediaFormat.KEY_AAC_PROFILE,
                MediaCodecInfo.CodecProfileLevel.AACObjectLC);
        format.setInteger(MediaFormat.KEY_BIT_RATE, BIT_RATE);
        format.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, SAMPLES_PER_BLOCK * 2);
        MediaCodec value = MediaCodec.createEncoderByType(MIME);
        value.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
        value.start();
        return value;
    }

    private long queuePcm(
            MediaCodec encoder,
            short[] samples,
            int count,
            long submittedSamples,
            boolean endOfStream) {
        int inputIndex;
        int attempts = 0;
        do {
            inputIndex = encoder.dequeueInputBuffer(10_000);
            attempts++;
        } while (inputIndex < 0 && attempts < 200);
        if (inputIndex < 0) throw new IllegalStateException("AAC 输入等待超时");

        ByteBuffer input = encoder.getInputBuffer(inputIndex);
        if (input == null) throw new IllegalStateException("AAC 输入缓冲区不可用");
        input.clear();
        input.order(ByteOrder.LITTLE_ENDIAN);
        for (int index = 0; index < count; index++) input.putShort(samples[index]);
        long presentationTimeUs = submittedSamples * 1_000_000L / SAMPLE_RATE;
        int flags = endOfStream ? MediaCodec.BUFFER_FLAG_END_OF_STREAM : 0;
        encoder.queueInputBuffer(inputIndex, 0, count * 2, presentationTimeUs, flags);
        return submittedSamples + count;
    }

    private MuxerState drainEncoder(
            MediaCodec encoder,
            MediaMuxer muxer,
            int trackIndex,
            boolean muxerStarted,
            boolean waitForEnd) {
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        int idleAttempts = 0;
        while (true) {
            int outputIndex = encoder.dequeueOutputBuffer(info, waitForEnd ? 10_000 : 0);
            if (outputIndex == MediaCodec.INFO_TRY_AGAIN_LATER) {
                if (!waitForEnd) break;
                idleAttempts++;
                if (idleAttempts >= 500) throw new IllegalStateException("AAC 收尾等待超时");
                continue;
            }
            idleAttempts = 0;
            if (outputIndex == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                if (muxerStarted) throw new IllegalStateException("AAC 格式重复变化");
                trackIndex = muxer.addTrack(encoder.getOutputFormat());
                muxer.start();
                muxerStarted = true;
                continue;
            }
            if (outputIndex < 0) continue;

            ByteBuffer outputBuffer = encoder.getOutputBuffer(outputIndex);
            if (outputBuffer == null) throw new IllegalStateException("AAC 输出缓冲区不可用");
            if ((info.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0) info.size = 0;
            if (info.size > 0) {
                if (!muxerStarted) throw new IllegalStateException("AAC 轨道尚未建立");
                outputBuffer.position(info.offset);
                outputBuffer.limit(info.offset + info.size);
                muxer.writeSampleData(trackIndex, outputBuffer, info);
            }
            boolean ended = (info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0;
            encoder.releaseOutputBuffer(outputIndex, false);
            if (ended) break;
        }
        return new MuxerState(trackIndex, muxerStarted);
    }

    private void stopAudioRecord() {
        AudioRecord current = audioRecord;
        if (current == null) return;
        try {
            if (current.getRecordingState() == AudioRecord.RECORDSTATE_RECORDING) current.stop();
        } catch (RuntimeException ignored) {
        }
    }

    private void rememberPeak(int peak) {
        int current = latestPeak.get();
        while (peak > current && !latestPeak.compareAndSet(current, peak)) {
            current = latestPeak.get();
        }
    }

    private static String safeMessage(Throwable error) {
        String value = error.getMessage();
        return value == null || value.isEmpty() ? error.getClass().getSimpleName() : value;
    }

    private static final class MuxerState {
        final int trackIndex;
        final boolean started;

        MuxerState(int trackIndex, boolean started) {
            this.trackIndex = trackIndex;
            this.started = started;
        }
    }

    private static final class WavWriter implements AutoCloseable {
        private final RandomAccessFile file;
        private final int sampleRate;
        private final int channels;
        private long dataBytes;

        WavWriter(File output, int sampleRate, int channels) throws Exception {
            this.file = new RandomAccessFile(output, "rw");
            this.sampleRate = sampleRate;
            this.channels = channels;
            file.setLength(0);
            for (int index = 0; index < 44; index++) file.write(0);
        }

        void write(short[] samples, int count) throws Exception {
            for (int index = 0; index < count; index++) {
                short sample = samples[index];
                file.write(sample & 0xff);
                file.write((sample >>> 8) & 0xff);
            }
            dataBytes += count * 2L;
        }

        @Override public void close() throws Exception {
            file.seek(0);
            file.writeBytes("RIFF");
            writeLittleEndianInt(file, 36L + dataBytes);
            file.writeBytes("WAVE");
            file.writeBytes("fmt ");
            writeLittleEndianInt(file, 16);
            writeLittleEndianShort(file, 1);
            writeLittleEndianShort(file, channels);
            writeLittleEndianInt(file, sampleRate);
            writeLittleEndianInt(file, sampleRate * channels * 2L);
            writeLittleEndianShort(file, channels * 2);
            writeLittleEndianShort(file, 16);
            file.writeBytes("data");
            writeLittleEndianInt(file, dataBytes);
            file.close();
        }

        private static void writeLittleEndianInt(RandomAccessFile file, long value)
                throws Exception {
            file.write((int) (value & 0xff));
            file.write((int) ((value >>> 8) & 0xff));
            file.write((int) ((value >>> 16) & 0xff));
            file.write((int) ((value >>> 24) & 0xff));
        }

        private static void writeLittleEndianShort(RandomAccessFile file, int value)
                throws Exception {
            file.write(value & 0xff);
            file.write((value >>> 8) & 0xff);
        }
    }
}
