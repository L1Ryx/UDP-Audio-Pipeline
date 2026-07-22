#include "udp_audio/sim/opus_playout_sim.hpp"

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"
#include "miniaudio.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kSampleRateHz = udp_audio::audio::kSampleRateHz;
using LabSettings = udp_audio::sim::OpusSimulationSettings;
using RunResult = udp_audio::sim::OpusSimulationResult;
using FrameStatus = udp_audio::sim::OpusFrameStatus;
using RecoveryMode = udp_audio::sim::OpusRecoveryMode;
using SourceMode = udp_audio::audio::SourceMode;

constexpr int kMaxRedundancyFrames = udp_audio::sim::kMaxOpusRedundancyFrames;

struct PlaybackState {
  std::mutex mutex;
  std::vector<float> samples;
  std::size_t cursor = 0;
  bool playing = false;
  std::atomic<std::uint64_t> callback_underruns{0};
  std::atomic<std::uint64_t> rendered_samples{0};
};

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

RunResult run_simulation(const LabSettings& settings) {
  return udp_audio::sim::run_opus_playout_simulation(settings);
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
    ImGui::Text("status: %s", udp_audio::sim::status_name(frame.status));
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
        if (ImGui::Button("Real Bundle Chirp Preset", ImVec2(-1.0F, 0))) {
          settings.frame_count = 100;
          settings.loss_percent = 20;
          settings.jitter_ms = 25;
          settings.seed = 1337;
          settings.bitrate_bps = 64000;
          settings.redundancy_frames = 3;
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
