// Drafted using JuneAI, a creation of WhiteLabs, owned and ran by ThatWhiteGuy364

#include <jni.h>
#include <oboe/Oboe.h>
#include <vector>
#include <complex>
#include <cmath>

// ─── Constants ───────────────────────────────────────────────────────────────

static constexpr int kFilterSize = 128;
static constexpr float kMu = 0.01f;
static constexpr float kMuHigh = 0.001f;
static constexpr int kFftSize = 256;

// ─── Global State ─────────────────────────────────────────────────────────────

// Per-channel weights and ring buffers (Stereo: Left=0, Right=1)
static float weightsL[kFilterSize] = {0.0f};
static float weightsR[kFilterSize] = {0.0f};
static float bufferL[kFilterSize]  = {0.0f};
static float bufferR[kFilterSize]  = {0.0f};

// Latest FFT magnitude spectrum for the VisualizerView
static std::vector<float> latestMagnitudes(kFftSize, 0.0f);

static float mu = kMu;

// ─── FFT (Radix-2 Cooley-Tukey) ──────────────────────────────────────────────

static void computeFFT(std::vector<std::complex<float>>& data) {
    int n = data.size();
    if (n <= 1) return;

    std::vector<std::complex<float>> even(n / 2), odd(n / 2);
    for (int i = 0; i < n / 2; ++i) {
        even[i] = data[i * 2];
        odd[i]  = data[i * 2 + 1];
    }

    computeFFT(even);
    computeFFT(odd);

    for (int k = 0; k < n / 2; ++k) {
        std::complex<float> t = std::polar(1.0f, -2.0f * M_PI * k / n) * odd[k];
        data[k]         = even[k] + t;
        data[k + n / 2] = even[k] - t;
    }
}

// ─── Stereo ANC Engine ───────────────────────────────────────────────────────

class StereoAncEngine : public oboe::AudioStreamDataCallback {
public:
    oboe::DataCallbackResult onAudioReady(oboe::AudioStream* stream,
                                          void* audioData,
                                          int32_t numFrames) override {
        auto* floatData = static_cast<float*>(audioData);
        int channelCount = stream->getChannelCount();

        for (int i = 0; i < numFrames; ++i) {
            for (int ch = 0; ch < channelCount; ++ch) {
                int idx = i * channelCount + ch;
                float noise = floatData[idx];

                float* weights = (ch == 0) ? weightsL : weightsR;
                float* buffer  = (ch == 0) ? bufferL  : bufferR;

                // Shift buffer
                for (int j = kFilterSize - 1; j > 0; --j) buffer[j] = buffer[j - 1];
                buffer[0] = noise;

                // Estimate anti-noise
                float antiNoise = 0.0f;
                for (int j = 0; j < kFilterSize; ++j) antiNoise += weights[j] * buffer[j];

                // Error: residual noise after cancellation
                float error = noise - antiNoise;

                // Dynamic mu: slow down for high-frequency content
                float currentMu = (ch == 0 && i % 2 == 0) ? kMuHigh : kMu;

                // Update LMS weights
                for (int j = 0; j < kFilterSize; ++j)
                    weights[j] += currentMu * error * buffer[j];

                // Inject inverted anti-noise signal
                floatData[idx] = -antiNoise;
            }
        }

        // ── FFT for VisualizerView (mono mix) ──
        std::vector<std::complex<float>> fftData(kFftSize, {0.0f, 0.0f});
        for (int i = 0; i < kFftSize && i < numFrames; ++i) {
            float sample = floatData[i * channelCount];
            fftData[i] = {sample, 0.0f};
        }
        computeFFT(fftData);
        for (int i = 0; i < kFftSize / 2; ++i) {
            latestMagnitudes[i] = std::abs(fftData[i]);
        }

        return oboe::DataCallbackResult::Continue;
    }
};

// ─── Singleton Engine & Stream ────────────────────────────────────────────────

static StereoAncEngine engine;
static std::shared_ptr<oboe::AudioStream> stream;

// ─── JNI Exports ─────────────────────────────────────────────────────────────

extern "C" JNIEXPORT void JNICALL
Java_com_whitelabs_anc_AncService_startEngine(JNIEnv* env, jobject thiz) {
    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Input)
           .setPerformanceMode(oboe::PerformanceMode::LowLatency)
           .setSharingMode(oboe::SharingMode::Exclusive)
           .setFormat(oboe::AudioFormat::Float)
           .setChannelCount(oboe::ChannelCount::Stereo)
           .setDataCallback(&engine)
           .openStream(stream);
    stream->requestStart();
}

extern "C" JNIEXPORT void JNICALL
Java_com_whitelabs_anc_AncService_stopEngine(JNIEnv* env, jobject thiz) {
    if (stream) {
        stream->requestStop();
        stream->close();
    }
}

extern "C" JNIEXPORT jfloatArray JNICALL
Java_com_whitelabs_anc_AncService_getFftData(JNIEnv* env, jobject thiz) {
    jfloatArray result = env->NewFloatArray(latestMagnitudes.size());
    env->SetFloatArrayRegion(result, 0, latestMagnitudes.size(), latestMagnitudes.data());
    return result;
}
