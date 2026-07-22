#include "udp_audio/audio/frame.hpp"

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"
#include "miniaudio.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <opus/opus.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <numeric>
#include <random>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using udp_audio::audio::MonoAudioFrame;

constexpr double kPi = 3.14159265358979323846;
constexpr double kToneHz = 440.0;
constexpr double kChirpStartHz = 220.0;
constexpr double kChirpEndHz = 880.0;
constexpr float kToneGain = 0.2F;
constexpr std::size_t kMaxOpusPacketBytes = 1500;
constexpr std::size_t kFrameSamples = udp_audio::audio::kFrameSamples;
constexpr std::size_t kSampleRateHz = udp_audio::audio::kSampleRateHz;
constexpr std::size_t kFrameDurationMs = udp_audio::audio::kFrameDurationMs;
constexpr int kMaxRedundancyFrames = 8;

enum class SourceMode {
  sine,
  chirp,
};

enum class RecoveryMode {
  plc,
  fec,
};

enum class FrameStatus {
  decoded,
  redundant,
  fec_attempt,
  plc,
  late_or_missing,
};

struct LabSettings {
  int frame_count = 100;
  int loss_percent = 20;
  int jitter_ms = 25;
  int seed = 1337;
  int bitrate_bps = 64000;
  int redundancy_frames = 5;
  int jitter_depth_frames = 6;
  SourceMode source_mode = SourceMode::chirp;
  RecoveryMode recovery_mode = RecoveryMode::fec;
};

struct EncodedFrame {
  std::array<unsigned char, kMaxOpusPacketBytes> payload{};
  std::size_t payload_size = 0;
};

struct FrameReport {
  FrameStatus status = FrameStatus::decoded;
  double network_latency_ms = 0.0;
  double playout_latency_ms = 0.0;
  float rms = 0.0F;
  float peak = 0.0F;
};

struct RunResult {
  std::vector<float> samples;
  std::vector<float> waveform_plot;
  std::vector<float> frame_energy;
  std::vector<FrameReport> frames;
  std::string error;
  int generated = 0;
  int dropped = 0;
  int decoded = 0;
  int redundant = 0;
  int fec_attempts = 0;
  int plc = 0;
  int late_or_missing = 0;
  int decode_errors = 0;
  double avg_latency_ms = 0.0;
  double max_latency_ms = 0.0;
  double avg_playout_ms = 0.0;
  double max_playout_ms = 0.0;
  double elapsed_ms = 0.0;
  double avg_packet_bytes = 0.0;
  double avg_redundancy_bytes = 0.0;
};

struct PlaybackState {
  std::mutex mutex;
  std::vector<float> samples;
  std::size_t cursor = 0;
  bool playing = false;
  std::atomic<std::uint64_t> callback_underruns{0};
  std::atomic<std::uint64_t> rendered_samples{0};
};

MonoAudioFrame make_sine_frame(std::uint32_t sequence, double& phase) {
  MonoAudioFrame frame{};
  frame.sequence = sequence;
  frame.timestamp_samples = sequence * static_cast<std::uint32_t>(kFrameSamples);

  const double phase_step = (2.0 * kPi * kToneHz) / static_cast<double>(kSampleRateHz);
  for (float& sample : frame.samples) {
    sample = static_cast<float>(std::sin(phase)) * kToneGain;
    phase += phase_step;
    if (phase >= 2.0 * kPi) {
      phase -= 2.0 * kPi;
    }
  }
  return frame;
}

MonoAudioFrame make_chirp_frame(std::uint32_t sequence,
                                std::size_t total_frames,
                                double& phase) {
  MonoAudioFrame frame{};
  frame.sequence = sequence;
  frame.timestamp_samples = sequence * static_cast<std::uint32_t>(kFrameSamples);
  const auto total_samples = std::max<std::size_t>(1U, total_frames * kFrameSamples);

  for (std::size_t i = 0; i < frame.samples.size(); ++i) {
    const auto absolute_sample = static_cast<std::size_t>(sequence) * kFrameSamples + i;
    const double progress =
      static_cast<double>(std::min(absolute_sample, total_samples - 1U)) /
      static_cast<double>(total_samples - 1U);
    const double frequency = kChirpStartHz + ((kChirpEndHz - kChirpStartHz) * progress);
    const double phase_step = (2.0 * kPi * frequency) / static_cast<double>(kSampleRateHz);
    frame.samples[i] = static_cast<float>(std::sin(phase)) * kToneGain;
    phase += phase_step;
    if (phase >= 2.0 * kPi) {
      phase -= 2.0 * kPi;
    }
  }
  return frame;
}

MonoAudioFrame make_source_frame(SourceMode source_mode,
                                 std::uint32_t sequence,
                                 std::size_t total_frames,
                                 double& phase) {
  return source_mode == SourceMode::chirp
           ? make_chirp_frame(sequence, total_frames, phase)
           : make_sine_frame(sequence, phase);
}

bool encode_frames(const LabSettings& settings,
                   std::vector<EncodedFrame>& encoded_frames,
                   int encode_frame_count,
                   RunResult& result) {
  int opus_error = OPUS_OK;
  OpusEncoder* encoder =
    opus_encoder_create(static_cast<opus_int32>(kSampleRateHz), 1, OPUS_APPLICATION_AUDIO,
                        &opus_error);
  if (opus_error != OPUS_OK || encoder == nullptr) {
    result.error = std::string("Opus encoder: ") + opus_strerror(opus_error);
    return false;
  }

  opus_encoder_ctl(encoder, OPUS_SET_BITRATE(settings.bitrate_bps));
  opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
  opus_encoder_ctl(encoder, OPUS_SET_PACKET_LOSS_PERC(settings.loss_percent));
  if (settings.recovery_mode == RecoveryMode::fec) {
    opus_encoder_ctl(encoder, OPUS_SET_INBAND_FEC(1));
  }

  double phase = 0.0;
  encoded_frames.resize(static_cast<std::size_t>(encode_frame_count));
  for (int i = 0; i < encode_frame_count; ++i) {
    const auto frame =
      make_source_frame(settings.source_mode, static_cast<std::uint32_t>(i),
                        static_cast<std::size_t>(encode_frame_count), phase);
    auto& encoded = encoded_frames[static_cast<std::size_t>(i)];
    const auto byte_count = opus_encode_float(
      encoder, frame.samples.data(), static_cast<int>(kFrameSamples), encoded.payload.data(),
      static_cast<opus_int32>(encoded.payload.size()));
    if (byte_count < 0) {
      result.error = std::string("Opus encode: ") + opus_strerror(byte_count);
      opus_encoder_destroy(encoder);
      return false;
    }
    encoded.payload_size = static_cast<std::size_t>(byte_count);
    result.avg_packet_bytes += static_cast<double>(encoded.payload_size);
  }

  opus_encoder_destroy(encoder);
  result.avg_packet_bytes /= std::max(1, encode_frame_count);
  return true;
}

void append_frame_metrics(const MonoAudioFrame& frame, FrameReport& report) {
  float peak = 0.0F;
  double energy = 0.0;
  for (const float sample : frame.samples) {
    peak = std::max(peak, std::abs(sample));
    energy += static_cast<double>(sample) * static_cast<double>(sample);
  }
  report.peak = peak;
  report.rms = static_cast<float>(std::sqrt(energy / static_cast<double>(frame.samples.size())));
}

bool decode_packet(OpusDecoder* decoder,
                   const EncodedFrame& encoded,
                   MonoAudioFrame& frame,
                   RunResult& result) {
  const auto decoded = opus_decode_float(
    decoder, encoded.payload.data(), static_cast<opus_int32>(encoded.payload_size),
    frame.samples.data(), static_cast<int>(kFrameSamples), 0);
  if (decoded != static_cast<int>(kFrameSamples)) {
    ++result.decode_errors;
    std::fill(frame.samples.begin(), frame.samples.end(), 0.0F);
    return false;
  }
  return true;
}

bool decode_fec_attempt(OpusDecoder* decoder,
                        const EncodedFrame& encoded,
                        MonoAudioFrame& frame,
                        RunResult& result) {
  const auto decoded = opus_decode_float(
    decoder, encoded.payload.data(), static_cast<opus_int32>(encoded.payload_size),
    frame.samples.data(), static_cast<int>(kFrameSamples), 1);
  if (decoded != static_cast<int>(kFrameSamples)) {
    ++result.decode_errors;
    std::fill(frame.samples.begin(), frame.samples.end(), 0.0F);
    return false;
  }
  return true;
}

void decode_plc(OpusDecoder* decoder, MonoAudioFrame& frame, RunResult& result) {
  const auto decoded = opus_decode_float(decoder, nullptr, 0, frame.samples.data(),
                                         static_cast<int>(kFrameSamples), 0);
  if (decoded != static_cast<int>(kFrameSamples)) {
    ++result.decode_errors;
    std::fill(frame.samples.begin(), frame.samples.end(), 0.0F);
  }
}

RunResult run_simulation(const LabSettings& settings) {
  const auto started = Clock::now();
  RunResult result{};
  result.generated = settings.frame_count;

  std::vector<EncodedFrame> encoded_frames;
  const int repair_tail_frames = std::clamp(settings.redundancy_frames, 0, kMaxRedundancyFrames);
  const int encoded_frame_count = settings.frame_count + repair_tail_frames;
  if (!encode_frames(settings, encoded_frames, encoded_frame_count, result)) {
    return result;
  }

  std::mt19937 rng(static_cast<std::uint32_t>(settings.seed));
  std::uniform_int_distribution<int> loss_distribution(1, 100);
  std::uniform_int_distribution<int> jitter_distribution(0, std::max(0, settings.jitter_ms));
  std::vector<bool> dropped(static_cast<std::size_t>(encoded_frame_count), false);
  std::vector<double> arrival_ms(static_cast<std::size_t>(encoded_frame_count), 0.0);

  for (int i = 0; i < encoded_frame_count; ++i) {
    dropped[static_cast<std::size_t>(i)] =
      i < settings.frame_count && settings.loss_percent > 0 &&
      loss_distribution(rng) <= settings.loss_percent;
    const double jitter_ms =
      !dropped[static_cast<std::size_t>(i)] && settings.jitter_ms > 0
        ? jitter_distribution(rng)
        : 0.0;
    arrival_ms[static_cast<std::size_t>(i)] =
      static_cast<double>(i * static_cast<int>(kFrameDurationMs)) + jitter_ms;
    if (i < settings.frame_count && dropped[static_cast<std::size_t>(i)]) {
      ++result.dropped;
    }
  }

  int opus_error = OPUS_OK;
  OpusDecoder* decoder =
    opus_decoder_create(static_cast<opus_int32>(kSampleRateHz), 1, &opus_error);
  if (opus_error != OPUS_OK || decoder == nullptr) {
    result.error = std::string("Opus decoder: ") + opus_strerror(opus_error);
    return result;
  }

  result.frames.resize(static_cast<std::size_t>(settings.frame_count));
  result.samples.reserve(static_cast<std::size_t>(settings.frame_count) * kFrameSamples);

  for (int sequence = 0; sequence < settings.frame_count; ++sequence) {
    const double playout_ms =
      static_cast<double>((sequence + settings.jitter_depth_frames) *
                          static_cast<int>(kFrameDurationMs));
    FrameReport report{};
    report.playout_latency_ms =
      static_cast<double>(settings.jitter_depth_frames * static_cast<int>(kFrameDurationMs));
    MonoAudioFrame output{};
    output.sequence = static_cast<std::uint32_t>(sequence);
    output.timestamp_samples = output.sequence * static_cast<std::uint32_t>(kFrameSamples);

    const auto seq_index = static_cast<std::size_t>(sequence);
    const bool primary_available =
      !dropped[seq_index] && arrival_ms[seq_index] <= playout_ms;
    if (primary_available) {
      decode_packet(decoder, encoded_frames[seq_index], output, result);
      report.status = FrameStatus::decoded;
      report.network_latency_ms =
        arrival_ms[seq_index] - static_cast<double>(sequence * static_cast<int>(kFrameDurationMs));
      ++result.decoded;
    } else {
      bool recovered = false;
      for (int lookahead = 1; lookahead <= settings.redundancy_frames; ++lookahead) {
        const int carrier = sequence + lookahead;
        if (carrier >= encoded_frame_count) {
          break;
        }
        const auto carrier_index = static_cast<std::size_t>(carrier);
        if (!dropped[carrier_index] && arrival_ms[carrier_index] <= playout_ms) {
          decode_packet(decoder, encoded_frames[seq_index], output, result);
          report.status = FrameStatus::redundant;
          ++result.decoded;
          ++result.redundant;
          recovered = true;
          break;
        }
      }

      if (!recovered && settings.recovery_mode == RecoveryMode::fec &&
          sequence + 1 < encoded_frame_count) {
        const auto carrier_index = static_cast<std::size_t>(sequence + 1);
        if (!dropped[carrier_index] && arrival_ms[carrier_index] <= playout_ms) {
          decode_fec_attempt(decoder, encoded_frames[carrier_index], output, result);
          report.status = FrameStatus::fec_attempt;
          ++result.fec_attempts;
          recovered = true;
        }
      }

      if (!recovered) {
        decode_plc(decoder, output, result);
        report.status = FrameStatus::plc;
        ++result.plc;
      }

      ++result.late_or_missing;
    }

    append_frame_metrics(output, report);
    result.frame_energy.push_back(report.rms);
    result.samples.insert(result.samples.end(), output.samples.begin(), output.samples.end());
    result.frames[seq_index] = report;

    result.avg_latency_ms += report.network_latency_ms;
    result.max_latency_ms = std::max(result.max_latency_ms, report.network_latency_ms);
    result.avg_playout_ms += report.playout_latency_ms;
    result.max_playout_ms = std::max(result.max_playout_ms, report.playout_latency_ms);
  }

  opus_decoder_destroy(decoder);

  result.avg_latency_ms /= std::max(1, result.decoded);
  result.avg_playout_ms /= std::max(1, settings.frame_count);
  result.avg_redundancy_bytes =
    result.avg_packet_bytes * static_cast<double>(settings.redundancy_frames);

  constexpr std::size_t kMaxPlotSamples = 2400;
  const auto stride =
    std::max<std::size_t>(1U, result.samples.size() / kMaxPlotSamples);
  for (std::size_t i = 0; i < result.samples.size(); i += stride) {
    result.waveform_plot.push_back(result.samples[i]);
  }

  result.elapsed_ms = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
  return result;
}

void playback_callback(ma_device* device, void* output, const void* input, ma_uint32 frame_count) {
  static_cast<void>(input);
  auto* state = static_cast<PlaybackState*>(device->pUserData);
  auto* out = static_cast<float*>(output);
  std::lock_guard<std::mutex> lock(state->mutex);

  bool counted_underrun = false;
  for (ma_uint32 i = 0; i < frame_count; ++i) {
    if (!state->playing) {
      out[i] = 0.0F;
      continue;
    }

    if (state->cursor >= state->samples.size()) {
      out[i] = 0.0F;
      state->playing = false;
      if (!counted_underrun) {
        state->callback_underruns.fetch_add(1, std::memory_order_relaxed);
        counted_underrun = true;
      }
      continue;
    }

    out[i] = state->samples[state->cursor++];
    state->rendered_samples.fetch_add(1, std::memory_order_relaxed);
  }
}

void set_playback_buffer(PlaybackState& playback, const std::vector<float>& samples) {
  std::lock_guard<std::mutex> lock(playback.mutex);
  playback.samples = samples;
  playback.cursor = 0;
  playback.playing = true;
  playback.callback_underruns.store(0, std::memory_order_relaxed);
  playback.rendered_samples.store(0, std::memory_order_relaxed);
}

void stop_playback(PlaybackState& playback) {
  std::lock_guard<std::mutex> lock(playback.mutex);
  playback.playing = false;
  playback.cursor = 0;
}

float playback_progress(PlaybackState& playback) {
  std::lock_guard<std::mutex> lock(playback.mutex);
  if (playback.samples.empty()) {
    return 0.0F;
  }
  return static_cast<float>(playback.cursor) / static_cast<float>(playback.samples.size());
}

void draw_playhead_on_last_item(float progress) {
  const ImVec2 item_min = ImGui::GetItemRectMin();
  const ImVec2 item_max = ImGui::GetItemRectMax();
  const float clamped_progress = std::clamp(progress, 0.0F, 1.0F);
  const float x = item_min.x + ((item_max.x - item_min.x) * clamped_progress);
  ImDrawList* draw_list = ImGui::GetWindowDrawList();
  draw_list->AddLine(ImVec2(x, item_min.y), ImVec2(x, item_max.y),
                     ImGui::ColorConvertFloat4ToU32(ImVec4(1.0F, 0.86F, 0.18F, 1.0F)),
                     2.0F);
}

const char* status_name(FrameStatus status) {
  switch (status) {
    case FrameStatus::decoded:
      return "decoded";
    case FrameStatus::redundant:
      return "redundant";
    case FrameStatus::fec_attempt:
      return "fec";
    case FrameStatus::plc:
      return "plc";
    case FrameStatus::late_or_missing:
      return "late";
  }
  return "unknown";
}

ImU32 status_color(FrameStatus status) {
  const auto color = [](float r, float g, float b) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, 1.0F));
  };

  switch (status) {
    case FrameStatus::decoded:
      return color(0.27F, 0.69F, 0.46F);
    case FrameStatus::redundant:
      return color(0.35F, 0.61F, 1.0F);
    case FrameStatus::fec_attempt:
      return color(0.87F, 0.70F, 0.26F);
    case FrameStatus::plc:
      return color(0.85F, 0.34F, 0.32F);
    case FrameStatus::late_or_missing:
      return color(0.65F, 0.65F, 0.65F);
  }
  return color(1.0F, 1.0F, 1.0F);
}

void draw_recovery_strip(const RunResult& result) {
  const ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
  const ImVec2 canvas_size(ImGui::GetContentRegionAvail().x, 42.0F);
  ImDrawList* draw_list = ImGui::GetWindowDrawList();
  draw_list->AddRectFilled(canvas_pos,
                           ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y),
                           ImGui::ColorConvertFloat4ToU32(ImVec4(0.11F, 0.12F, 0.14F, 1.0F)),
                           4.0F);

  if (!result.frames.empty()) {
    const float width_per_frame = canvas_size.x / static_cast<float>(result.frames.size());
    for (std::size_t i = 0; i < result.frames.size(); ++i) {
      const float x0 = canvas_pos.x + (static_cast<float>(i) * width_per_frame);
      const float x1 = canvas_pos.x + (static_cast<float>(i + 1U) * width_per_frame);
      draw_list->AddRectFilled(ImVec2(x0, canvas_pos.y), ImVec2(x1, canvas_pos.y + canvas_size.y),
                               status_color(result.frames[i].status));
    }
  }

  ImGui::InvisibleButton("recovery_strip", canvas_size);
  if (ImGui::IsItemHovered() && !result.frames.empty()) {
    const float local_x = ImGui::GetIO().MousePos.x - canvas_pos.x;
    const auto index = std::clamp<std::size_t>(
      static_cast<std::size_t>((local_x / canvas_size.x) * result.frames.size()), 0U,
      result.frames.size() - 1U);
    const auto& frame = result.frames[index];
    ImGui::BeginTooltip();
    ImGui::Text("frame %zu", index);
    ImGui::Text("status: %s", status_name(frame.status));
    ImGui::Text("rms: %.5f", frame.rms);
    ImGui::Text("peak: %.5f", frame.peak);
    ImGui::EndTooltip();
  }
}

void print_headless_summary(const RunResult& result) {
  SDL_Log("generated=%d dropped=%d decoded=%d redundant=%d fec_attempts=%d plc=%d",
          result.generated, result.dropped, result.decoded, result.redundant,
          result.fec_attempts, result.plc);
  SDL_Log("late_or_missing=%d decode_errors=%d avg_playout_latency_ms=%.3f",
          result.late_or_missing, result.decode_errors, result.avg_playout_ms);
  std::string plc_frames;
  for (std::size_t i = 0; i < result.frames.size(); ++i) {
    if (result.frames[i].status == FrameStatus::plc) {
      if (!plc_frames.empty()) {
        plc_frames += ",";
      }
      plc_frames += std::to_string(i);
    }
  }
  SDL_Log("plc_frames=%s", plc_frames.empty() ? "(none)" : plc_frames.c_str());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::string(argv[1]) == "--headless") {
    const LabSettings settings{};
    const auto result = run_simulation(settings);
    print_headless_summary(result);
    return result.error.empty() ? 0 : 1;
  }

  if (!SDL_Init(SDL_INIT_VIDEO)) {
    SDL_Log("SDL_Init failed: %s", SDL_GetError());
    return 1;
  }

  SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

  SDL_Window* window =
    SDL_CreateWindow("UDP Audio Pipeline Lab", 1320, 860,
                     SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (window == nullptr) {
    SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
    SDL_Quit();
    return 1;
  }

  SDL_GLContext gl_context = SDL_GL_CreateContext(window);
  if (gl_context == nullptr) {
    SDL_Log("SDL_GL_CreateContext failed: %s", SDL_GetError());
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }
  SDL_GL_MakeCurrent(window, gl_context);
  SDL_GL_SetSwapInterval(1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  ImGui::StyleColorsDark();

  ImGui_ImplSDL3_InitForOpenGL(window, gl_context);
  ImGui_ImplOpenGL3_Init("#version 150");

  PlaybackState playback;
  ma_device device{};
  bool audio_ok = false;
  auto config = ma_device_config_init(ma_device_type_playback);
  config.playback.format = ma_format_f32;
  config.playback.channels = 1;
  config.sampleRate = static_cast<ma_uint32>(kSampleRateHz);
  config.dataCallback = playback_callback;
  config.pUserData = &playback;
  if (ma_device_init(nullptr, &config, &device) == MA_SUCCESS &&
      ma_device_start(&device) == MA_SUCCESS) {
    audio_ok = true;
  }

  LabSettings settings{};
  RunResult result = run_simulation(settings);
  bool done = false;

  while (!done) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      ImGui_ImplSDL3_ProcessEvent(&event);
      if (event.type == SDL_EVENT_QUIT) {
        done = true;
      }
      if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
          event.window.windowID == SDL_GetWindowID(window)) {
        done = true;
      }
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
    ImGui::Begin("UDP Audio Pipeline Lab", nullptr,
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoCollapse);

    if (ImGui::BeginTabBar("tabs")) {
      if (ImGui::BeginTabItem("Lab")) {
        ImGui::Columns(2, nullptr, true);
        ImGui::SetColumnWidth(0, 360.0F);

        ImGui::TextUnformatted("Controls");
        ImGui::Separator();
        if (ImGui::Button("Robust Chirp Preset", ImVec2(-1.0F, 0))) {
          settings.frame_count = 100;
          settings.loss_percent = 20;
          settings.jitter_ms = 25;
          settings.seed = 1337;
          settings.bitrate_bps = 64000;
          settings.redundancy_frames = 5;
          settings.jitter_depth_frames = 6;
          settings.source_mode = SourceMode::chirp;
          settings.recovery_mode = RecoveryMode::fec;
          stop_playback(playback);
          result = run_simulation(settings);
        }
        ImGui::SliderInt("Frames", &settings.frame_count, 20, 500);
        ImGui::SliderInt("Loss %", &settings.loss_percent, 0, 60);
        ImGui::SliderInt("Jitter ms", &settings.jitter_ms, 0, 80);
        ImGui::SliderInt("Seed", &settings.seed, 1, 9999);
        ImGui::SliderInt("Bitrate", &settings.bitrate_bps, 16000, 128000);
        ImGui::SliderInt("Redundant Frames", &settings.redundancy_frames, 0,
                         kMaxRedundancyFrames);
        ImGui::SliderInt("Jitter Depth", &settings.jitter_depth_frames, 1, 12);

        int source_index = settings.source_mode == SourceMode::chirp ? 1 : 0;
        if (ImGui::Combo("Source", &source_index, "sine\0chirp\0")) {
          settings.source_mode = source_index == 1 ? SourceMode::chirp : SourceMode::sine;
        }

        int recovery_index = settings.recovery_mode == RecoveryMode::fec ? 1 : 0;
        if (ImGui::Combo("Recovery", &recovery_index, "plc\0fec\0")) {
          settings.recovery_mode =
            recovery_index == 1 ? RecoveryMode::fec : RecoveryMode::plc;
        }

        if (ImGui::Button("Run", ImVec2(100, 0))) {
          stop_playback(playback);
          result = run_simulation(settings);
        }
        ImGui::SameLine();
        if (ImGui::Button("Play", ImVec2(100, 0)) && audio_ok && !result.samples.empty()) {
          set_playback_buffer(playback, result.samples);
        }
        ImGui::SameLine();
        if (ImGui::Button("Stop", ImVec2(100, 0))) {
          stop_playback(playback);
        }

        ImGui::ProgressBar(playback_progress(playback), ImVec2(-1.0F, 0.0F));
        ImGui::Text("Audio device: %s", audio_ok ? "ready" : "unavailable");
        if (!result.error.empty()) {
          ImGui::TextColored(ImVec4(1.0F, 0.25F, 0.25F, 1.0F), "%s", result.error.c_str());
        }

        ImGui::NextColumn();

        ImGui::TextUnformatted("Summary");
        ImGui::Separator();
        ImGui::Text("generated=%d dropped=%d", result.generated, result.dropped);
        ImGui::Text("decoded=%d redundant=%d fec_attempts=%d plc=%d",
                    result.decoded, result.redundant, result.fec_attempts, result.plc);
        ImGui::Text("late_or_missing=%d decode_errors=%d", result.late_or_missing,
                    result.decode_errors);
        ImGui::Text("avg_network_latency_ms=%.3f max_network_latency_ms=%.3f",
                    result.avg_latency_ms, result.max_latency_ms);
        ImGui::Text("avg_playout_latency_ms=%.3f max_playout_latency_ms=%.3f",
                    result.avg_playout_ms, result.max_playout_ms);
        ImGui::Text("avg_packet_bytes=%.2f avg_redundancy_bytes=%.2f",
                    result.avg_packet_bytes, result.avg_redundancy_bytes);
        ImGui::Text("simulation_time_ms=%.3f", result.elapsed_ms);
        ImGui::Text("rendered_samples=%llu callback_underruns=%llu",
                    static_cast<unsigned long long>(
                      playback.rendered_samples.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(
                      playback.callback_underruns.load(std::memory_order_relaxed)));

        ImGui::Columns(1);
        ImGui::Spacing();

        if (!result.waveform_plot.empty()) {
          ImGui::TextUnformatted("Output Waveform");
          const float progress = playback_progress(playback);
          ImGui::PlotLines("##waveform", result.waveform_plot.data(),
                           static_cast<int>(result.waveform_plot.size()), 0, nullptr, -0.25F,
                           0.25F, ImVec2(-1.0F, 170.0F));
          draw_playhead_on_last_item(progress);
        }

        if (!result.frame_energy.empty()) {
          ImGui::TextUnformatted("Frame RMS");
          ImGui::PlotLines("##energy", result.frame_energy.data(),
                           static_cast<int>(result.frame_energy.size()), 0, nullptr, 0.0F,
                           0.22F, ImVec2(-1.0F, 120.0F));
        }

        ImGui::TextUnformatted("Recovery Timeline");
        draw_recovery_strip(result);
        ImGui::TextColored(ImColor(status_color(FrameStatus::decoded)), "decoded");
        ImGui::SameLine();
        ImGui::TextColored(ImColor(status_color(FrameStatus::redundant)), "redundant");
        ImGui::SameLine();
        ImGui::TextColored(ImColor(status_color(FrameStatus::fec_attempt)), "fec");
        ImGui::SameLine();
        ImGui::TextColored(ImColor(status_color(FrameStatus::plc)), "plc");

        ImGui::EndTabItem();
      }

      if (ImGui::BeginTabItem("ImGui Demo")) {
        ImGui::ShowDemoWindow();
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }

    ImGui::End();

    ImGui::Render();
    glViewport(0, 0, static_cast<int>(io.DisplaySize.x), static_cast<int>(io.DisplaySize.y));
    glClearColor(0.08F, 0.09F, 0.10F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(window);
  }

  if (audio_ok) {
    ma_device_stop(&device);
    ma_device_uninit(&device);
  }
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  SDL_GL_DestroyContext(gl_context);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
