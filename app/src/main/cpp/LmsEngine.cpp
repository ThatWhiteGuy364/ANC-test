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

// ─── Constants ───────────────────────────────────────────────────────────────

static constexpr int   kFilterSize   = 64;
static constexpr float kMu           = 0.005f;
static constexpr int   kFftSize      = 256;
static constexpr int   kLogInterval  = 4800;
static constexpr int   kFramesPerCb  = 96;

// ─── LMS State ────────────────────────────────────────────────────────────────

static float weights[kFilterSize]    = {};
static float lmsRingBuf[kFilterSize] = {};

// ─── FFT State ────────────────────────────────────────────────────────────────

static std::vector<float> latestMagnitudes(kFftSize / 2, 0.0f);
static std::mutex          fftMutex;

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

// ─── Duplex Callback ─────────────────────────────────────────────────────────
//
// Runs on the OUTPUT stream thread. Synchronously reads from the input stream
// with timeout=0, processes LMS in-place, writes anti-noise to the output
// buffer — all in one callback with no ring buffer latency.

class DuplexCallback : public oboe::AudioStreamDataCallback {
public:
    oboe::AudioStream* inputStream = nullptr;

    oboe::DataCallbackResult onAudioReady(oboe::AudioStream* /*outputStream*/,
                                          void* audioData,
                                          int32_t numFrames) override {
        auto* out = static_cast<float*>(audioData);

        float inBuf[kFramesPerCb];
        std::memset(inBuf, 0, sizeof(float) * numFrames);

        if (inputStream) {
            auto res = inputStream->read(inBuf, numFrames, 0 /*timeoutNanos=0*/);
            if (!res) {
                std::memset(out, 0, sizeof(float) * numFrames);
                return oboe::DataCallbackResult::Continue;
            }
        }

        float sumError = 0.0f;
        float sumPower = 0.0f;

        for (int i = 0; i < numFrames; ++i) {
            float noise = inBuf[i];

            for (int j = kFilterSize - 1; j > 0; --j) lmsRingBuf[j] = lmsRingBuf[j - 1];
            lmsRingBuf[0] = noise;

            float antiNoise = 0.0f;
            for (int j = 0; j < kFilterSize; ++j) antiNoise += weights[j] * lmsRingBuf[j];

            float error = noise - antiNoise;
            sumError += error * error;
            sumPower += noise * noise;

            for (int j = 0; j < kFilterSize; ++j)
                weights[j] += kMu * error * lmsRingBuf[j];

            out[i] = -antiNoise;
        }

        // FFT on output for visualiser
        std::vector<std::complex<float>> fftData(kFftSize, {0.0f, 0.0f});
        for (int i = 0; i < kFftSize && i < numFrames; ++i)
            fftData[i] = {inBuf[i], 0.0f};
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
                    "frames=%d  input_rms=%.5f  error_rms=%.5f",
                    numFrames,
                    std::sqrt(sumPower / float(numFrames)),
                    std::sqrt(sumError / float(numFrames)));
                writeLog(buf);
            }
        }

        return oboe::DataCallbackResult::Continue;
    }
};

// ─── Singletons ───────────────────────────────────────────────────────────────

static DuplexCallback duplexCb;
static std::shared_ptr<oboe::AudioStream> inputStream;
static std::shared_ptr<oboe::AudioStream> outputStream;

// ─── JNI ─────────────────────────────────────────────────────────────────────

extern "C" JNIEXPORT void JNICALL
Java_com_whitelabs_anc_AncService_startEngine(JNIEnv*, jobject) {
    oboe::Result r;

    // Output stream first — drives the callback thread
    r = oboe::AudioStreamBuilder()
        .setDirection(oboe::Direction::Output)
        ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
        ->setSharingMode(oboe::SharingMode::Exclusive)
        ->setFormat(oboe::AudioFormat::Float)
        ->setChannelCount(oboe::ChannelCount::Mono)
        ->setFramesPerDataCallback(kFramesPerCb)
        ->setDataCallback(&duplexCb)
        ->openStream(outputStream);

    if (r != oboe::Result::OK) {
        LOGE("Output openStream failed: %s", oboe::convertToText(r));
        writeLog("Output openStream failed");
        return;
    }

    int sampleRate = outputStream->getSampleRate();
    LOGI("Output SR=%d  framesPerCb=%d", sampleRate, kFramesPerCb);

    // Input stream — matched sample rate, no callback (read() driven by output)
    r = oboe::AudioStreamBuilder()
        .setDirection(oboe::Direction::Input)
        ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
        ->setSharingMode(oboe::SharingMode::Shared)
        ->setFormat(oboe::AudioFormat::Float)
        ->setChannelCount(oboe::ChannelCount::Mono)
        ->setSampleRate(sampleRate)
        ->setBufferCapacityInFrames(kFramesPerCb * 2)
        ->openStream(inputStream);

    if (r != oboe::Result::OK) {
        LOGE("Input openStream failed: %s", oboe::convertToText(r));
        writeLog("Input openStream failed");
        outputStream->close();
        outputStream.reset();
        return;
    }

    // Wire input stream pointer into the callback before starting
    duplexCb.inputStream = inputStream.get();

    inputStream->requestStart();
    outputStream->requestStart();

    writeLog("Engine started — duplex single-callback OK");
    LOGI("ANC engine running");
}

extern "C" JNIEXPORT void JNICALL
Java_com_whitelabs_anc_AncService_stopEngine(JNIEnv*, jobject) {
    duplexCb.inputStream = nullptr;
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
