#include <whisper/whisper.h>

#include <csignal>
#include <cmath>
#include <cstdlib>
#include <numbers>
#include <vector>

#include <unistd.h>

namespace {

void discardLog(enum ggml_log_level, const char *, void *) {}

void exitOnHardwareFault(int) { _exit(70); }

void installFaultBoundary()
{
    struct sigaction action {};
    action.sa_handler = exitOnHardwareFault;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    sigaction(SIGSEGV, &action, nullptr);
    sigaction(SIGBUS, &action, nullptr);
    sigaction(SIGABRT, &action, nullptr);
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 2) {
        return 2;
    }
    installFaultBoundary();
    whisper_log_set(discardLog, nullptr);
    auto parameters = whisper_context_default_params();
    parameters.use_gpu = true;
    parameters.gpu_device = 0;
    whisper_context *context =
        whisper_init_from_file_with_params(argv[1], parameters);
    if (context == nullptr) {
        return 3;
    }
    constexpr int sampleRate = WHISPER_SAMPLE_RATE;
    std::vector<float> samples(sampleRate);
    for (int index = 0; index < sampleRate; ++index) {
        const float time =
            static_cast<float>(index) / static_cast<float>(sampleRate);
        samples[index] =
            0.08F * std::sin(
                2.0F * std::numbers::pi_v<float> * 180.0F * time)
            + 0.03F * std::sin(
                2.0F * std::numbers::pi_v<float> * 720.0F * time);
    }
    auto inference = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    inference.n_threads = 1;
    inference.language = "en";
    inference.no_context = true;
    inference.print_special = false;
    inference.print_progress = false;
    inference.print_realtime = false;
    inference.print_timestamps = false;
    const int inferenceStatus = whisper_full(
        context,
        inference,
        samples.data(),
        static_cast<int>(samples.size()));
    whisper_free(context);
    return inferenceStatus == 0 ? 0 : 4;
}
