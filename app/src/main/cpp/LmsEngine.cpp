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

static constexpr int   kFilterSize   = 64;
static constexpr float kMu           = 0.005f;
static constexpr int   kFftSize      = 256;
static constexpr int   kLogInterval  = 4800;
static constexpr int   kFramesPerCb  = 96;
static constexpr int   kSampleRate   = 48000;
static constexpr int64_t kReadTimeoutNs = 1000000LL; // 1ms

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
    // Always write to logcat regardless of file toggle
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

// ─── Duplex Callback ─────────────────────────────────────────────────────────

class DuplexCallback : public oboe::AudioStreamDataCallback {
public:
    oboe::AudioStream* inputStream = nullptr;
    std::atomic<int>   callbackCount{0};
    std::atomic<int>   emptyReads{0};

    oboe::DataCallbackResult onAudioReady(oboe::AudioStream* /*outputStream*/,
                                          void* audioData,
                                          int32_t numFrames) override {
        auto* out = static_cast<float*>(audioData);
        float inBuf[kFramesPerCb] = {};

        int framesRead = 0;

        if (inputStream) {
            auto res = inputStream->read(inBuf, numFrames, kReadTimeoutNs);
            if (res) {
                framesRead = res.value();
            } else {
                LOGW("read() error: %s", oboe::convertToText(res.error()));
            }
        }

        int cc = callbackCount.fetch_add(1) + 1;

        if (framesRead == 0) {
            emptyReads.fetch_add(1);
            std::memset(out, 0, sizeof(float) * numFrames);
            if (cc % 500 == 0) {
                char buf[128];
                std::snprintf(buf, sizeof(buf),
                    "WARN: %d callbacks, %d empty reads",
                    cc, emptyReads.load());
                writeLog(buf);
            }
            return oboe::DataCallbackResult::Continue;
        }

        float sumError = 0.0f;
        float sumPower = 0.0f;

        for (int i = 0; i < framesRead; ++i) {
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
        // Zero remainder if framesRead < numFrames
        if (framesRead < numFrames)
            std::memset(out + framesRead, 0, sizeof(float) * (numFrames - framesRead));

        std::vector<std::complex<float>> fftData(kFftSize, {0.0f, 0.0f});
        for (int i = 0; i < kFftSize && i < framesRead; ++i)
            fftData[i] = {inBuf[i], 0.0f};
        computeFFT(fftData);
        {
            std::lock_guard<std::mutex> lock(fftMutex);
            for (int i = 0; i < kFftSize / 2; ++i)
                latestMagnitudes[i] = std::abs(fftData[i]);
        }

        logFrameCounter += framesRead;
        if (logFrameCounter >= kLogInterval) {
            logFrameCounter = 0;
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                "OK cb=%d  read=%d  input_rms=%.5f  error_rms=%.5f",
                cc, framesRead,
                std::sqrt(sumPower / float(framesRead)),
                std::sqrt(sumError / float(framesRead)));
            writeLog(buf);
        }

        return oboe::DataCallbackResult::Continue;
    }
};

// ─── Singletons ───────────────────────────────────────────────────────────────

static DuplexCallback duplexCb;
static std::shared_ptr<oboe::AudioStream> inputStream;
static std::shared_ptr<oboe::AudioStream> outputStream;

// ─── Stream open helper: tries preferred sharing mode, falls back ─────────────

static oboe::Result openOutputStream(oboe::SharingMode mode) {
    return oboe::AudioStreamBuilder()
        .setDirection(oboe::Direction::Output)
        ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
        ->setSharingMode(mode)
        ->setFormat(oboe::AudioFormat::Float)
        ->setChannelCount(oboe::ChannelCount::Mono)
        ->setSampleRate(kSampleRate)
        ->setFramesPerDataCallback(kFramesPerCb)
        ->setDataCallback(&duplexCb)
        ->openStream(outputStream);
}

// ─── JNI ─────────────────────────────────────────────────────────────────────

extern "C" JNIEXPORT void JNICALL
Java_com_whitelabs_anc_AncService_startEngine(JNIEnv*, jobject) {

    // Reset diagnostics
    duplexCb.callbackCount.store(0);
    duplexCb.emptyReads.store(0);
    std::memset(weights, 0, sizeof(weights));
    std::memset(lmsRingBuf, 0, sizeof(lmsRingBuf));

    writeLog("startEngine called");

    // ── Input stream ──
    oboe::Result r = oboe::AudioStreamBuilder()
        .setDirection(oboe::Direction::Input)
        ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
        ->setSharingMode(oboe::SharingMode::Shared)
        ->setFormat(oboe::AudioFormat::Float)
        ->setChannelCount(oboe::ChannelCount::Mono)
        ->setSampleRate(kSampleRate)
        ->setBufferCapacityInFrames(kFramesPerCb * 8)
        ->openStream(inputStream);

    if (r != oboe::Result::OK) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "Input openStream FAILED: %s", oboe::convertToText(r));
        LOGE("%s", buf);
        writeLog(buf);
        return;
    }

    int actualSR = inputStream->getSampleRate();
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "Input opened OK  SR=%d", actualSR);
        writeLog(buf);
    }

    inputStream->requestStart();
    writeLog("Input stream started — warming up 20ms");
    usleep(20000);

    // ── Output stream: try Exclusive, fall back to Shared ──
    r = openOutputStream(oboe::SharingMode::Exclusive);
    if (r != oboe::Result::OK) {
        LOGW("Exclusive output failed (%s), retrying Shared", oboe::convertToText(r));
        r = openOutputStream(oboe::SharingMode::Shared);
    }

    if (r != oboe::Result::OK) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "Output openStream FAILED: %s", oboe::convertToText(r));
        LOGE("%s", buf);
        writeLog(buf);
        inputStream->requestStop();
        inputStream->close();
        inputStream.reset();
        return;
    }

    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
            "Output opened OK  SR=%d  sharing=%s  framesPerCb=%d",
            outputStream->getSampleRate(),
            outputStream->getSharingMode() == oboe::SharingMode::Exclusive ? "Exclusive" : "Shared",
            kFramesPerCb);
        writeLog(buf);
    }

    duplexCb.inputStream = inputStream.get();
    outputStream->requestStart();
    writeLog("Output stream started — engine running");
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
