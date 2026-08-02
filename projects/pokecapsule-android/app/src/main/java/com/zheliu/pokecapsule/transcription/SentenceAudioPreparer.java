package com.zheliu.pokecapsule.transcription;

import android.media.MediaCodec;
import android.media.MediaExtractor;
import android.media.MediaFormat;
import android.media.MediaMuxer;

import java.io.Closeable;
import java.io.File;
import java.io.IOException;
import java.nio.ByteBuffer;

public final class SentenceAudioPreparer {
    /*
     * Tencent rejects audio beyond 60 seconds. Keep a generous AAC-frame margin:
     * the source recording is preserved, while only this temporary upload copy is shortened.
     */
    private static final long UPLOAD_COPY_END_US = 58_500_000L;
    private static final long MAX_VALIDATED_OUTPUT_US = 59_000_000L;
    private static final long MAX_AUTOMATIC_REPAIR_US =
            SentenceAudioPolicy.MAX_AUTOMATIC_REPAIR_DURATION_MS * 1_000L;

    private SentenceAudioPreparer() {}

    public static PreparedAudio prepare(File source, long durationMs, File cacheDirectory)
            throws IOException {
        if (!source.isFile() || source.length() == 0) {
            throw new IOException("录音文件不存在或为空");
        }
        long mediaDurationUs = inspectAudioDurationUs(source, true);
        long trustedDurationUs = Math.max(durationMs * 1_000L, mediaDurationUs);
        if (trustedDurationUs < SentenceAudioPolicy.SAFE_CAPTURE_DURATION_MS * 1_000L) {
            return new PreparedAudio(source, false);
        }
        if (trustedDurationUs <= 0 || trustedDurationUs > MAX_AUTOMATIC_REPAIR_US) {
            throw new IOException("录音超过自动修复范围，已保留原音，请拆分后再转写");
        }
        if (!cacheDirectory.isDirectory() && !cacheDirectory.mkdirs()) {
            throw new IOException("无法创建转写暂存目录");
        }
        File clipped = File.createTempFile("pokecapsule-asr-", ".m4a", cacheDirectory);
        try {
            copyFirstAudioTrack(source, clipped);
            validateOutput(clipped);
            return new PreparedAudio(clipped, true);
        } catch (Exception error) {
            clipped.delete();
            if (error instanceof IOException) throw (IOException) error;
            throw new IOException("无法准备 60 秒以内的转写副本", error);
        }
    }

    private static void copyFirstAudioTrack(File source, File destination) throws Exception {
        MediaExtractor extractor = new MediaExtractor();
        MediaMuxer muxer = null;
        boolean muxerStarted = false;
        int writtenSamples = 0;
        Exception failure = null;
        try {
            extractor.setDataSource(source.getAbsolutePath());
            int sourceTrack = findAudioTrack(extractor);
            if (sourceTrack < 0) throw new IOException("录音中没有可识别的音轨");
            extractor.selectTrack(sourceTrack);
            MediaFormat format = extractor.getTrackFormat(sourceTrack);
            int maximumInputSize = format.containsKey(MediaFormat.KEY_MAX_INPUT_SIZE)
                    ? format.getInteger(MediaFormat.KEY_MAX_INPUT_SIZE)
                    : 64 * 1024;
            ByteBuffer buffer = ByteBuffer.allocateDirect(Math.max(64 * 1024, maximumInputSize));
            muxer = new MediaMuxer(
                    destination.getAbsolutePath(), MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4);
            int destinationTrack = muxer.addTrack(format);
            muxer.start();
            muxerStarted = true;

            MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
            long firstSourceTimeUs = Long.MIN_VALUE;
            long previousOutputTimeUs = -1;
            while (true) {
                buffer.clear();
                int size = extractor.readSampleData(buffer, 0);
                if (size < 0) break;
                long sourceTimeUs = extractor.getSampleTime();
                if (firstSourceTimeUs == Long.MIN_VALUE) firstSourceTimeUs = sourceTimeUs;
                long outputTimeUs = sourceTimeUs - firstSourceTimeUs;
                if (outputTimeUs < 0) {
                    throw new IOException("录音时间戳无效，无法生成安全副本");
                }
                if (outputTimeUs >= UPLOAD_COPY_END_US) break;
                if (writtenSamples > 0 && outputTimeUs <= previousOutputTimeUs) {
                    throw new IOException("录音时间戳不连续，无法生成安全副本");
                }
                int flags = (extractor.getSampleFlags() & MediaExtractor.SAMPLE_FLAG_SYNC) != 0
                        ? MediaCodec.BUFFER_FLAG_SYNC_FRAME
                        : 0;
                buffer.position(0);
                buffer.limit(size);
                info.set(0, size, outputTimeUs, flags);
                muxer.writeSampleData(destinationTrack, buffer, info);
                previousOutputTimeUs = outputTimeUs;
                writtenSamples++;
                if (!extractor.advance()) break;
            }
            if (writtenSamples == 0) throw new IOException("录音中没有可复制的音频样本");
        } catch (Exception error) {
            failure = error;
        } finally {
            extractor.release();
            if (muxer != null) {
                if (muxerStarted) {
                    try {
                        muxer.stop();
                    } catch (RuntimeException error) {
                        if (failure == null) {
                            failure = new IOException("转写副本封装失败", error);
                        }
                    }
                }
                try {
                    muxer.release();
                } catch (RuntimeException error) {
                    if (failure == null) {
                        failure = new IOException("转写副本资源释放失败", error);
                    }
                }
            }
        }
        if (failure != null) throw failure;
    }

    private static void validateOutput(File output) throws IOException {
        if (!output.isFile() || output.length() == 0) {
            throw new IOException("无法生成转写副本");
        }
        long durationUs = inspectAudioDurationUs(output, false);
        if (durationUs <= 0 || durationUs > MAX_VALIDATED_OUTPUT_US) {
            throw new IOException("转写副本时长校验失败");
        }
    }

    private static long inspectAudioDurationUs(File file, boolean trustDeclaredDuration)
            throws IOException {
        MediaExtractor extractor = new MediaExtractor();
        try {
            extractor.setDataSource(file.getAbsolutePath());
            int audioTrack = findAudioTrack(extractor);
            if (audioTrack < 0) throw new IOException("录音中没有可识别的音轨");
            MediaFormat format = extractor.getTrackFormat(audioTrack);
            long declaredDurationUs = 0;
            if (trustDeclaredDuration && format.containsKey(MediaFormat.KEY_DURATION)) {
                declaredDurationUs = Math.max(0, format.getLong(MediaFormat.KEY_DURATION));
            }
            extractor.selectTrack(audioTrack);
            long firstTimeUs = Long.MIN_VALUE;
            long previousTimeUs = Long.MIN_VALUE;
            long lastTimeUs = Long.MIN_VALUE;
            long largestStepUs = 0;
            while (extractor.getSampleTrackIndex() >= 0) {
                long sampleTimeUs = extractor.getSampleTime();
                if (firstTimeUs == Long.MIN_VALUE) firstTimeUs = sampleTimeUs;
                if (previousTimeUs != Long.MIN_VALUE && sampleTimeUs > previousTimeUs) {
                    largestStepUs = Math.max(largestStepUs, sampleTimeUs - previousTimeUs);
                }
                previousTimeUs = sampleTimeUs;
                lastTimeUs = sampleTimeUs;
                if (!extractor.advance()) break;
            }
            long sampledDurationUs =
                    firstTimeUs == Long.MIN_VALUE || lastTimeUs < firstTimeUs
                            ? 0
                            : lastTimeUs - firstTimeUs + Math.max(1_000L, largestStepUs);
            return Math.max(declaredDurationUs, sampledDurationUs);
        } catch (RuntimeException error) {
            throw new IOException("无法读取录音时长", error);
        } finally {
            extractor.release();
        }
    }

    private static int findAudioTrack(MediaExtractor extractor) {
        for (int index = 0; index < extractor.getTrackCount(); index++) {
            MediaFormat format = extractor.getTrackFormat(index);
            String mime = format.getString(MediaFormat.KEY_MIME);
            if (mime != null && mime.startsWith("audio/")) return index;
        }
        return -1;
    }

    public static final class PreparedAudio implements Closeable {
        public final File file;
        private final boolean temporary;

        PreparedAudio(File file, boolean temporary) {
            this.file = file;
            this.temporary = temporary;
        }

        @Override public void close() {
            if (temporary) file.delete();
        }
    }
}
