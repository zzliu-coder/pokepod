#include <jni.h>
#include <atomic>
#include <string>
#include <vector>

#include "whisper.h"

namespace {
std::atomic<bool> cancel_requested{false};

bool should_abort(void *) {
    return cancel_requested.load(std::memory_order_relaxed);
}

std::string jstring_to_utf8(JNIEnv *env, jstring value) {
    if (value == nullptr) return {};
    const char *chars = env->GetStringUTFChars(value, nullptr);
    std::string result(chars == nullptr ? "" : chars);
    if (chars != nullptr) env->ReleaseStringUTFChars(value, chars);
    return result;
}

void throw_illegal_state(JNIEnv *env, const std::string &message) {
    jclass type = env->FindClass("java/lang/IllegalStateException");
    env->ThrowNew(type, message.c_str());
}
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_zheliu_pokecapsule_transcription_WhisperNative_transcribe(
        JNIEnv *env,
        jclass,
        jstring model_path,
        jfloatArray samples,
        jint thread_count) {
    const std::string path = jstring_to_utf8(env, model_path);
    if (path.empty() || samples == nullptr) {
        throw_illegal_state(env, "model path and PCM samples are required");
        return nullptr;
    }

    const jsize sample_count = env->GetArrayLength(samples);
    if (sample_count <= 0) {
        throw_illegal_state(env, "PCM input is empty");
        return nullptr;
    }

    std::vector<float> pcm(static_cast<size_t>(sample_count));
    env->GetFloatArrayRegion(samples, 0, sample_count, pcm.data());
    if (env->ExceptionCheck()) return nullptr;

    whisper_context_params context_params = whisper_context_default_params();
    context_params.use_gpu = false;
    context_params.flash_attn = false;

    whisper_context *context = whisper_init_from_file_with_params(path.c_str(), context_params);
    if (context == nullptr) {
        throw_illegal_state(env, "unable to load Whisper model");
        return nullptr;
    }

    whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    params.n_threads = thread_count < 1 ? 1 : (thread_count > 2 ? 2 : thread_count);
    params.language = "zh";
    params.translate = false;
    params.no_timestamps = true;
    params.print_progress = false;
    params.print_realtime = false;
    params.print_timestamps = false;
    params.single_segment = false;
    params.abort_callback = should_abort;
    params.abort_callback_user_data = nullptr;

    const int result = whisper_full(context, params, pcm.data(), static_cast<int>(pcm.size()));
    if (result != 0) {
        whisper_free(context);
        throw_illegal_state(env, "Whisper transcription failed: " + std::to_string(result));
        return nullptr;
    }

    std::string text;
    const int segment_count = whisper_full_n_segments(context);
    for (int index = 0; index < segment_count; ++index) {
        const char *segment = whisper_full_get_segment_text(context, index);
        if (segment != nullptr) text += segment;
    }
    whisper_free(context);
    return env->NewStringUTF(text.c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_com_zheliu_pokecapsule_transcription_WhisperNative_prepareCurrent(
        JNIEnv *,
        jclass) {
    cancel_requested.store(false, std::memory_order_relaxed);
}

extern "C" JNIEXPORT void JNICALL
Java_com_zheliu_pokecapsule_transcription_WhisperNative_cancelCurrent(
        JNIEnv *,
        jclass) {
    cancel_requested.store(true, std::memory_order_relaxed);
}
