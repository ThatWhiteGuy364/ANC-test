// Drafted using JuneAI, a creation of WhiteLabs, owned and ran by ThatWhiteGuy364

#include <jni.h>
#include <oboe/Oboe.h>
#include <vector>
#include <complex>
#include <cmath>
#include <atomic>
#include <mutex>
#include <string>
#include <ctime>
#include <cstdio>
#include <unistd.h>
#include <android/log.h>

#define LOG_TAG "WhiteLabsANC"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)

// ─── Constants ───────────────────────────────────────────────────────────────

static constexpr int   kFilterSize  = 128;
static constexpr float kMu          = 0.005f;
static constexpr int   kFftSize     = 256;
static constexpr int   kLogInterval = 4800;
static constexpr int   kRingBufSize = 2048;

// ─── Lock-free Ring Buffer ────────────────────────────────────────────────────

struct RingBuffer {
    float data[kRingBufSize] = {};
    std::atomic<int> head{0};
    std::atomic<int> tail{0};

    void push(float v) {
        int h = head.load(std::memory_order_relaxed);
        data[h] = v;
        head.store((h + 1) % kRingBufSize, std::memory_order_release);
    }

    float pop() {
        int t = tail.load(std::memory_order_relaxed);
        int h = head.load(std::memory_order_acquire);
        if (t == h) return 0.0f;
        float v = data[t];
        tail.store((t + 1) % kRingBufSize, std::memory_order_release);
        return v;
    }

    int available() {
        int h = head.load(std::memory_order_acquire);
        int t = tail.load(std::memory_order_relaxed);
        return (h - t + kRingBufSize) % kRingBufSize;
    }
};

static RingBuffer antiNoiseBuf;

// ─── LMS State ────────────────────────────────────────────────────────────────

static float weights[kFilterSize] = {};
static float lmsRingBuf[kFilterSize] = {};

// ─── FFT State ────────────────────────────────────────────────────────────────

static std::vector<float> latestMagnitudes(kFftSize / 2, 0.0f);
static std::mutex fftMutex;

// ─── Logging ──────────────────────────────────────────────────────────────────

static std::atomic<int>  logFd{-1};
static std::mutex        logMutex;
static std::atomic<bool> loggingEnabled{false};
static int               logFrameCounter = 0;

static void writeLog(const char* line) {
    if (!loggingEnabled.load()) return;
    int fd = logFd.load();
    if (fd < 0) return;
    std::time_t t = std::time(nullptr);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    char buf[256];
    int len = std::snprintf(buf, sizeof(buf), "[%s] %s\n", ts, line);
    if (len > 0) {
        std::lock_guard<std::mutex> lock(logMutex);
        ::write(fd, buf, static_cast<size_t>(len));
    }
}

// ─── FFT ─────────────────────────────────────────────────────────────────────

static void computeFFT(std::vector<std::complex<float>>& data) {
    int n = static_cast<int>(data.size());
    if (n <= 1) return;
    std::vector<std::complex<float>> even(n / 2), odd(n / 2);
    for (int i = 0; i < n / 2; ++i) { even[i] = data[i * 2]; odd[i] = data[i * 2 + 1]; }
    computeFFT(even);
    computeFFT(odd);
    for (int k = 0; k < n / 2; ++k) {
        auto tw = std::polar<float>(1.0f, -2.0f * float(M_PI) * float(k) / float(n)) * odd[k];
        data[k]         = even[k] + tw;
        data[k + n / 2] = even[k] - tw;
    }
}

// ─── Input Callback: Mic → LMS → Ring Buffer ─────────────────────────────────

class InputCallback : public oboe::AudioStreamDataCallback {
public:
    oboe::DataCallbackResult onAudioReady(oboe::AudioStream* stream,
                                          void* audioData,
                                          int32_t numFrames) override {
        auto* in = static_cast<float*>(audioData);
        float sumError = 0.0f;
        float sumPower = 0.0f;

        for (int i = 0; i < numFrames; ++i) {
            float noise = in[i];
            for (int j = kFilterSize - 1; j > 0; --j) lmsRingBuf[j] = lmsRingBuf[j - 1];
            lmsRingBuf[0] = noise;

            float antiNoise = 0.0f;
            for (int j = 0; j < kFilterSize; ++j) antiNoise += weights[j] * lmsRingBuf[j];

            float error = noise - antiNoise;
            sumError += error * error;
            sumPower += noise * noise;

            for (int j = 0; j < kFilterSize; ++j)
                weights[j] += kMu * error * lmsRingBuf[j];

            antiNoiseBuf.push(-antiNoise);
        }

        std::vector<std::complex<float>> fftData(kFftSize, {0.0f, 0.0f});
        for (int i = 0; i < kFftSize && i < numFrames; ++i)
            fftData[i] = {in[i], 0.0f};
        computeFFT(fftData);
        {
            std::lock_guard<std::mutex> lock(fftMutex);
            for (int i = 0; i < kFftSize / 2; ++i)
                latestMagnitudes[i] = std::abs(fftData[i]);
        }

        if (loggingEnabled.load()) {
            logFrameCounter += numFrames;
            if (logFrameCounter >= kLogInterval) {
                logFrameCounter = 0;
                char buf[128];
                std::snprintf(buf, sizeof(buf),
                    "frames=%d  input_rms=%.5f  error_rms=%.5f  ring_avail=%d",
                    numFrames,
                    std::sqrt(sumPower / float(numFrames)),
                    std::sqrt(sumError / float(numFrames)),
                    antiNoiseBuf.available());
                writeLog(buf);
            }
        }

        return oboe::DataCallbackResult::Continue;
    }
};

// ─── Output Callback: Ring Buffer → Speaker ───────────────────────────────────

class OutputCallback : public oboe::AudioStreamDataCallback {
public:
    oboe::DataCallbackResult onAudioReady(oboe::AudioStream* stream,
                                          void* audioData,
                                          int32_t numFrames) override {
        auto* out = static_cast<float*>(audioData);
        for (int i = 0; i < numFrames; ++i)
            out[i] = antiNoiseBuf.pop();
        return oboe::DataCallbackResult::Continue;
    }
};

// ─── Singletons ───────────────────────────────────────────────────────────────

static InputCallback  inputCb;
static OutputCallback outputCb;
static std::shared_ptr<oboe::AudioStream> inputStream;
static std::shared_ptr<oboe::AudioStream> outputStream;

// ─── JNI ─────────────────────────────────────────────────────────────────────

extern "C" JNIEXPORT void JNICALL
Java_com_whitelabs_anc_AncService_startEngine(JNIEnv*, jobject) {
    oboe::Result r;

    r = oboe::AudioStreamBuilder()
        .setDirection(oboe::Direction::Output)
        ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
        ->setSharingMode(oboe::SharingMode::Exclusive)
        ->setFormat(oboe::AudioFormat::Float)
        ->setChannelCount(oboe::ChannelCount::Mono)
        ->setDataCallback(&outputCb)
        ->openStream(outputStream);

    if (r != oboe::Result::OK) {
        LOGE("Output openStream failed: %s", oboe::convertToText(r));
        writeLog("Output openStream failed");
        return;
    }

    r = oboe::AudioStreamBuilder()
        .setDirection(oboe::Direction::Input)
        ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
        ->setSharingMode(oboe::SharingMode::Shared)
        ->setFormat(oboe::AudioFormat::Float)
        ->setChannelCount(oboe::ChannelCount::Mono)
        ->setSampleRate(outputStream->getSampleRate())
        ->setFramesPerDataCallback(outputStream->getFramesPerDataCallback())
        ->setDataCallback(&inputCb)
        ->openStream(inputStream);

    if (r != oboe::Result::OK) {
        LOGE("Input openStream failed: %s", oboe::convertToText(r));
        writeLog("Input openStream failed");
        outputStream->close();
        outputStream.reset();
        return;
    }

    outputStream->requestStart();
    inputStream->requestStart();
    writeLog("Engine started — dual stream OK");
    LOGI("ANC engine started. SR=%d", outputStream->getSampleRate());
}

extern "C" JNIEXPORT void JNICALL
Java_com_whitelabs_anc_AncService_stopEngine(JNIEnv*, jobject) {
    if (inputStream)  { inputStream->requestStop();  inputStream->close();  inputStream.reset(); }
    if (outputStream) { outputStream->requestStop(); outputStream->close(); outputStream.reset(); }
    writeLog("Engine stopped");
}

extern "C" JNIEXPORT jfloatArray JNICALL
Java_com_whitelabs_anc_MainActivity_getFftData(JNIEnv* env, jobject) {
    std::lock_guard<std::mutex> lock(fftMutex);
    jfloatArray result = env->NewFloatArray(static_cast<jsize>(latestMagnitudes.size()));
    env->SetFloatArrayRegion(result, 0, static_cast<jsize>(latestMagnitudes.size()), latestMagnitudes.data());
    return result;
}

extern "C" JNIEXPORT void JNICALL
Java_com_whitelabs_anc_MainActivity_enableLogging(JNIEnv*, jobject, jint fd) {
    int old = logFd.exchange(static_cast<int>(fd));
    if (old >= 0) ::close(old);
    loggingEnabled.store(true);
    logFrameCounter = 0;
    writeLog("=== Logging session started ===");
}

extern "C" JNIEXPORT void JNICALL
Java_com_whitelabs_anc_MainActivity_disableLogging(JNIEnv*, jobject) {
    writeLog("=== Logging session ended ===");
    loggingEnabled.store(false);
    int fd = logFd.exchange(-1);
    if (fd >= 0) ::close(fd);
}
