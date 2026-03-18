// Drafted using JuneAI, a creation of WhiteLabs, owned and ran by ThatWhiteGuy364

#include <jni.h>
#include <oboe/Oboe.h>
#include <vector>
#include <complex>
#include <cmath>
#include <atomic>
#include <mutex>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <android/log.h>

#define LOG_TAG "WhiteLabsANC"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)

// ─── Constants ───────────────────────────────────────────────────────────────

static constexpr int   kFilterSize   = 128;
static constexpr float kMu           = 0.01f;
static constexpr int   kFftSize      = 256;
static constexpr int   kLogInterval  = 4800;
static constexpr int   kSampleRate   = 48000;
static constexpr int   kFramesPerCb  = 96;

// Ring buffer holds 4 callback periods — absorbs clock drift with ~8ms latency
static constexpr int   kRingSize     = kFramesPerCb * 4;

// ─── Power-of-2 Lock-free Ring Buffer ────────────────────────────────────────
// Single-producer (input callback) / single-consumer (output callback)

struct SPSCRing {
    static constexpr int kSize = kRingSize;
    float    data[kSize] = {};
    std::atomic<int> writePos{0};
    std::atomic<int> readPos{0};

    int available() const {
        return (writePos.load(std::memory_order_acquire) -
                readPos.load(std::memory_order_relaxed) + kSize) % kSize;
    }

    void push(float v) {
        int w = writePos.load(std::memory_order_relaxed);
        data[w] = v;
        writePos.store((w + 1) % kSize, std::memory_order_release);
    }

    float pop() {
        int r = readPos.load(std::memory_order_relaxed);
        if (r == writePos.load(std::memory_order_acquire)) return 0.0f;
        float v = data[r];
        readPos.store((r + 1) % kSize, std::memory_order_release);
        return v;
    }

    void reset() { writePos.store(0); readPos.store(0); }
};

static SPSCRing antiNoiseBuf;

// ─── LMS State ────────────────────────────────────────────────────────────────

static float weights[kFilterSize]    = {};
static float lmsRingBuf[kFilterSize] = {};

// ─── FFT State ────────────────────────────────────────────────────────────────

static std::vector<float> latestMagnitudes(kFftSize / 2, 0.0f);
static std::mutex          fftMutex;

// ─── Diagnostics ─────────────────────────────────────────────────────────────

static std::atomic<int> inCallbacks{0};
static std::atomic<int> outCallbacks{0};
static std::atomic<int> starvedCallbacks{0};
static int              logFrameCounter = 0;

// ─── Logging ──────────────────────────────────────────────────────────────────

static std::atomic<int>  logFd{-1};
static std::mutex        logMutex;
static std::atomic<bool> loggingEnabled{false};

static void writeLog(const char* line) {
    LOGI("%s", line);
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
    float sumError = 0.0f;
    float sumPower = 0.0f;

    oboe::DataCallbackResult onAudioReady(oboe::AudioStream*,
                                          void* audioData,
                                          int32_t numFrames) override {
        auto* in = static_cast<float*>(audioData);
        inCallbacks.fetch_add(1, std::memory_order_relaxed);

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

        // FFT on mic signal for visualiser
        std::vector<std::complex<float>> fftData(kFftSize, {0.0f, 0.0f});
        for (int i = 0; i < kFftSize && i < numFrames; ++i)
            fftData[i] = {in[i], 0.0f};
        computeFFT(fftData);
        {
            std::lock_guard<std::mutex> lock(fftMutex);
            for (int i = 0; i < kFftSize / 2; ++i)
                latestMagnitudes[i] = std::abs(fftData[i]);
        }

        logFrameCounter += numFrames;
        if (logFrameCounter >= kLogInterval) {
            logFrameCounter = 0;
            char buf[160];
            int oc = outCallbacks.load();
            int sc = starvedCallbacks.load();
            std::snprintf(buf, sizeof(buf),
                "in=%d  out=%d  starved=%d  ring=%d  input_rms=%.5f  error_rms=%.5f",
                inCallbacks.load(), oc, sc, antiNoiseBuf.available(),
                std::sqrt(sumPower / float(kLogInterval)),
                std::sqrt(sumError / float(kLogInterval)));
            sumError = 0.0f;
            sumPower = 0.0f;
            writeLog(buf);
        }

        return oboe::DataCallbackResult::Continue;
    }
};

// ─── Output Callback: Ring Buffer → Speaker ───────────────────────────────────

class OutputCallback : public oboe::AudioStreamDataCallback {
public:
    oboe::DataCallbackResult onAudioReady(oboe::AudioStream*,
                                          void* audioData,
                                          int32_t numFrames) override {
        auto* out = static_cast<float*>(audioData);
        outCallbacks.fetch_add(1, std::memory_order_relaxed);

        int avail = antiNoiseBuf.available();
        if (avail < numFrames) {
            starvedCallbacks.fetch_add(1, std::memory_order_relaxed);
            std::memset(out, 0, sizeof(float) * numFrames);
            return oboe::DataCallbackResult::Continue;
        }

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
    std::memset(weights,    0, sizeof(weights));
    std::memset(lmsRingBuf, 0, sizeof(lmsRingBuf));
    antiNoiseBuf.reset();
    inCallbacks.store(0);
    outCallbacks.store(0);
    starvedCallbacks.store(0);
    logFrameCounter = 0;
    inputCb.sumError = 0.0f;
    inputCb.sumPower = 0.0f;

    writeLog("startEngine called");

    // ── Input stream (callback-driven) ──
    oboe::Result r = oboe::AudioStreamBuilder()
        .setDirection(oboe::Direction::Input)
        ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
        ->setSharingMode(oboe::SharingMode::Shared)
        ->setFormat(oboe::AudioFormat::Float)
        ->setChannelCount(oboe::ChannelCount::Mono)
        ->setSampleRate(kSampleRate)
        ->setFramesPerDataCallback(kFramesPerCb)
        ->setBufferCapacityInFrames(kFramesPerCb * 2)
        ->setDataCallback(&inputCb)
        ->openStream(inputStream);

    if (r != oboe::Result::OK) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "Input openStream FAILED: %s", oboe::convertToText(r));
        LOGE("%s", buf); writeLog(buf);
        return;
    }
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Input opened OK  SR=%d", inputStream->getSampleRate());
        writeLog(buf);
    }

    // ── Output stream (callback-driven) — try Exclusive, fall back to Shared ──
    auto tryOutput = [&](oboe::SharingMode mode) {
        return oboe::AudioStreamBuilder()
            .setDirection(oboe::Direction::Output)
            ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
            ->setSharingMode(mode)
            ->setFormat(oboe::AudioFormat::Float)
            ->setChannelCount(oboe::ChannelCount::Mono)
            ->setSampleRate(kSampleRate)
            ->setFramesPerDataCallback(kFramesPerCb)
            ->setBufferCapacityInFrames(kFramesPerCb * 2)
            ->setDataCallback(&outputCb)
            ->openStream(outputStream);
    };

    r = tryOutput(oboe::SharingMode::Exclusive);
    if (r != oboe::Result::OK) {
        LOGW("Exclusive output unavailable, retrying Shared");
        r = tryOutput(oboe::SharingMode::Shared);
    }

    if (r != oboe::Result::OK) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "Output openStream FAILED: %s", oboe::convertToText(r));
        LOGE("%s", buf); writeLog(buf);
        inputStream->close(); inputStream.reset();
        return;
    }
    {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "Output opened OK  SR=%d  sharing=%s",
            outputStream->getSampleRate(),
            outputStream->getSharingMode() == oboe::SharingMode::Exclusive ? "Exclusive" : "Shared");
        writeLog(buf);
    }

    // Start input first, pre-fill ring buffer with 2 callback periods of silence
    // so output never starves on the very first callbacks
    inputStream->requestStart();
    for (int i = 0; i < kFramesPerCb * 2; ++i) antiNoiseBuf.push(0.0f);
    usleep(10000); // 10ms: let input accumulate real data before output starts

    outputStream->requestStart();
    writeLog("Both streams started — engine running");
}

extern "C" JNIEXPORT void JNICALL
Java_com_whitelabs_anc_AncService_stopEngine(JNIEnv*, jobject) {
    if (outputStream) { outputStream->requestStop(); outputStream->close(); outputStream.reset(); }
    if (inputStream)  { inputStream->requestStop();  inputStream->close();  inputStream.reset(); }
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
