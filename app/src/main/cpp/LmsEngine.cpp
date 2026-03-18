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
static constexpr int   kSampleRate   = 48000;

// Half a buffer period in nanoseconds — long enough to wait for mic data
// without overrunning the output callback deadline
static constexpr int64_t kReadTimeoutNs =
    static_cast<int64_t>(kFramesPerCb) * 1000000000LL / kSampleRate / 2;

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
            auto res = inputStream->read(inBuf, numFrames, kReadTimeoutNs);
            if (!res || res.value() == 0) {
                // No mic data yet — output silence, keep going
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

    // Start input first and let it buffer before output begins
    r = oboe::AudioStreamBuilder()
        .setDirection(oboe::Direction::Input)
        ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
        ->setSharingMode(oboe::SharingMode::Shared)
        ->setFormat(oboe::AudioFormat::Float)
        ->setChannelCount(oboe::ChannelCount::Mono)
        ->setSampleRate(kSampleRate)
        ->setBufferCapacityInFrames(kFramesPerCb * 4)
        ->openStream(inputStream);

    if (r != oboe::Result::OK) {
        LOGE("Input openStream failed: %s", oboe::convertToText(r));
        writeLog("Input openStream failed");
        return;
    }

    inputStream->requestStart();

    // Brief warm-up: let input buffer accumulate ~10ms of mic data
    // before the output callback begins pulling from it
    usleep(10000);

    r = oboe::AudioStreamBuilder()
        .setDirection(oboe::Direction::Output)
        ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
        ->setSharingMode(oboe::SharingMode::Exclusive)
        ->setFormat(oboe::AudioFormat::Float)
        ->setChannelCount(oboe::ChannelCount::Mono)
        ->setSampleRate(kSampleRate)
        ->setFramesPerDataCallback(kFramesPerCb)
        ->setDataCallback(&duplexCb)
        ->openStream(outputStream);

    if (r != oboe::Result::OK) {
        LOGE("Output openStream failed: %s", oboe::convertToText(r));
        writeLog("Output openStream failed");
        inputStream->requestStop();
        inputStream->close();
        inputStream.reset();
        return;
    }

    duplexCb.inputStream = inputStream.get();
    outputStream->requestStart();

    writeLog("Engine started — duplex OK");
    LOGI("ANC engine running. SR=%d  framesPerCb=%d  readTimeout=%lldns",
         kSampleRate, kFramesPerCb, (long long)kReadTimeoutNs);
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
