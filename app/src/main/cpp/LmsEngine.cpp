// Drafted using JuneAI, a creation of WhiteLabs, owned and ran by ThatWhiteGuy364

#include <jni.h>
#include <oboe/Oboe.h>
#include <vector>
#include <complex>
#include <cmath>
#include <atomic>
#include <mutex>
#include <android/log.h>

#define LOG_TAG "WhiteLabsANC"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ─── Constants ───────────────────────────────────────────────────────────────

static constexpr int kFilterSize = 128;
static constexpr float kMu      = 0.01f;
static constexpr int kFftSize   = 256;

// ─── Global State ─────────────────────────────────────────────────────────────

static float weights[kFilterSize] = {0.0f};
static float ringBuf[kFilterSize] = {0.0f};

static std::vector<float> latestMagnitudes(kFftSize / 2, 0.0f);
static std::mutex fftMutex;

// ─── FFT (Radix-2 Cooley-Tukey) ──────────────────────────────────────────────

static void computeFFT(std::vector<std::complex<float>>& data) {
    int n = static_cast<int>(data.size());
    if (n <= 1) return;

    std::vector<std::complex<float>> even(n / 2), odd(n / 2);
    for (int i = 0; i < n / 2; ++i) {
        even[i] = data[i * 2];
        odd[i]  = data[i * 2 + 1];
    }

    computeFFT(even);
    computeFFT(odd);

    for (int k = 0; k < n / 2; ++k) {
        std::complex<float> t = std::polar<float>(1.0f, -2.0f * float(M_PI) * float(k) / float(n)) * odd[k];
        data[k]         = even[k] + t;
        data[k + n / 2] = even[k] - t;
    }
}

// ─── Mono ANC Engine ─────────────────────────────────────────────────────────

class AncEngine : public oboe::AudioStreamDataCallback {
public:
    oboe::DataCallbackResult onAudioReady(oboe::AudioStream* audioStream,
                                          void* audioData,
                                          int32_t numFrames) override {
        auto* floatData = static_cast<float*>(audioData);

        for (int i = 0; i < numFrames; ++i) {
            float noise = floatData[i];

            for (int j = kFilterSize - 1; j > 0; --j) ringBuf[j] = ringBuf[j - 1];
            ringBuf[0] = noise;

            float antiNoise = 0.0f;
            for (int j = 0; j < kFilterSize; ++j) antiNoise += weights[j] * ringBuf[j];

            float error = noise - antiNoise;

            for (int j = 0; j < kFilterSize; ++j)
                weights[j] += kMu * error * ringBuf[j];

            floatData[i] = -antiNoise;
        }

        std::vector<std::complex<float>> fftData(kFftSize, {0.0f, 0.0f});
        for (int i = 0; i < kFftSize && i < numFrames; ++i)
            fftData[i] = {floatData[i], 0.0f};

        computeFFT(fftData);

        {
            std::lock_guard<std::mutex> lock(fftMutex);
            for (int i = 0; i < kFftSize / 2; ++i)
                latestMagnitudes[i] = std::abs(fftData[i]);
        }

        return oboe::DataCallbackResult::Continue;
    }
};

// ─── Singleton Engine & Stream ────────────────────────────────────────────────

static AncEngine engine;
static std::shared_ptr<oboe::AudioStream> stream;

// ─── JNI Exports ─────────────────────────────────────────────────────────────

extern "C" JNIEXPORT void JNICALL
Java_com_whitelabs_anc_AncService_startEngine(JNIEnv* env, jobject thiz) {
    oboe::AudioStreamBuilder builder;
    oboe::Result result = builder
        .setDirection(oboe::Direction::Input)
        ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
        ->setSharingMode(oboe::SharingMode::Shared)
        ->setFormat(oboe::AudioFormat::Float)
        ->setChannelCount(oboe::ChannelCount::Mono)
        ->setDataCallback(&engine)
        ->openStream(stream);

    if (result != oboe::Result::OK) {
        LOGE("openStream failed: %s", oboe::convertToText(result));
        return;
    }

    result = stream->requestStart();
    if (result != oboe::Result::OK) {
        LOGE("requestStart failed: %s", oboe::convertToText(result));
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_whitelabs_anc_AncService_stopEngine(JNIEnv* env, jobject thiz) {
    if (stream) {
        stream->requestStop();
        stream->close();
        stream.reset();
    }
}

extern "C" JNIEXPORT jfloatArray JNICALL
Java_com_whitelabs_anc_MainActivity_getFftData(JNIEnv* env, jobject thiz) {
    std::lock_guard<std::mutex> lock(fftMutex);
    jfloatArray result = env->NewFloatArray(static_cast<jsize>(latestMagnitudes.size()));
    env->SetFloatArrayRegion(result, 0, static_cast<jsize>(latestMagnitudes.size()), latestMagnitudes.data());
    return result;
}
