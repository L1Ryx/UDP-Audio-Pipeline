#include "udp_audio/sim/opus_playout_sim.hpp"

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"
#include "miniaudio.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_opengl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kSampleRateHz = udp_audio::audio::kSampleRateHz;
using LabSettings = udp_audio::sim::OpusSimulationSettings;
using RunResult = udp_audio::sim::OpusSimulationResult;
using FrameStatus = udp_audio::sim::OpusFrameStatus;
using InputMode = udp_audio::sim::OpusInputMode;
using LossModel = udp_audio::sim::OpusLossModel;
using RecoveryMode = udp_audio::sim::OpusRecoveryMode;
using SourceMode = udp_audio::audio::SourceMode;

constexpr int kMaxRedundancyFrames = udp_audio::sim::kMaxOpusRedundancyFrames;
constexpr int kMaxFileInputFrames = 30'000;
constexpr std::size_t kFrameSamples = udp_audio::audio::kFrameSamples;
constexpr std::size_t kPlaybackTailFadeSamples = udp_audio::audio::kFrameSamples;
constexpr std::size_t kPlaybackTailSilenceSamples = udp_audio::audio::kFrameSamples;

struct LoadedAudio {
  std::vector<float> samples;
  std::string error;
  std::string status;
  bool truncated = false;
};

struct FileDialogState {
  std::mutex mutex;
  std::optional<std::string> selected_path;
  std::optional<std::string> message;
};

struct PlaybackState {
  std::mutex mutex;
  std::vector<float> samples;
  std::size_t cursor = 0;
  bool playing = false;
  std::atomic<std::uint64_t> callback_underruns{0};
  std::atomic<std::uint64_t> rendered_samples{0};
};

struct PlaybackPosition {
  std::size_t cursor = 0;
  std::size_t length = 0;
  bool playing = false;
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

std::vector<float> make_playback_samples(const std::vector<float>& samples) {
  auto playback_samples = samples;
  const auto fade_samples = std::min(kPlaybackTailFadeSamples, playback_samples.size());
  if (fade_samples > 1U) {
    const auto fade_start = playback_samples.size() - fade_samples;
    for (std::size_t i = 0; i < fade_samples; ++i) {
      const float fade =
        1.0F - (static_cast<float>(i) / static_cast<float>(fade_samples - 1U));
      playback_samples[fade_start + i] *= fade;
    }
  }
  playback_samples.insert(playback_samples.end(), kPlaybackTailSilenceSamples, 0.0F);
  return playback_samples;
}

void resume_or_start_playback(PlaybackState& playback, const std::vector<float>& samples) {
  std::lock_guard<std::mutex> lock(playback.mutex);
  if (playback.samples.empty()) {
    playback.samples = make_playback_samples(samples);
    playback.cursor = 0;
  }
  if (samples.empty() || playback.samples.empty()) {
    playback.playing = false;
    return;
  }
  if (playback.cursor >= samples.size()) {
    playback.cursor = 0;
  }
  playback.playing = true;
  playback.callback_underruns.store(0, std::memory_order_relaxed);
  playback.rendered_samples.store(0, std::memory_order_relaxed);
}

void seek_playback_buffer(PlaybackState& playback,
                          const std::vector<float>& samples,
                          float progress,
                          bool start_playback) {
  std::lock_guard<std::mutex> lock(playback.mutex);
  if (playback.samples.empty() || playback.samples.size() < samples.size()) {
    playback.samples = make_playback_samples(samples);
  }
  if (playback.samples.empty()) {
    playback.cursor = 0;
    playback.playing = false;
    return;
  }

  const auto audio_length = samples.empty() ? playback.samples.size() : samples.size();
  const auto max_cursor = audio_length > 0U ? audio_length - 1U : 0U;
  playback.cursor = static_cast<std::size_t>(
    std::clamp(progress, 0.0F, 1.0F) * static_cast<float>(max_cursor));
  playback.playing = start_playback;
  playback.callback_underruns.store(0, std::memory_order_relaxed);
  playback.rendered_samples.store(0, std::memory_order_relaxed);
}

void stop_playback(PlaybackState& playback) {
  std::lock_guard<std::mutex> lock(playback.mutex);
  playback.playing = false;
}

void clear_playback(PlaybackState& playback) {
  std::lock_guard<std::mutex> lock(playback.mutex);
  playback.playing = false;
  playback.cursor = 0;
  playback.samples.clear();
}

PlaybackPosition playback_position(PlaybackState& playback) {
  std::lock_guard<std::mutex> lock(playback.mutex);
  return {.cursor = playback.cursor, .length = playback.samples.size(), .playing = playback.playing};
}

float playback_progress(PlaybackState& playback) {
  const auto position = playback_position(playback);
  if (position.length == 0U) {
    return 0.0F;
  }
  return static_cast<float>(position.cursor) / static_cast<float>(position.length);
}

const char* source_name(SourceMode source_mode) {
  switch (source_mode) {
    case SourceMode::sine:
      return "sine";
    case SourceMode::chirp:
      return "chirp";
  }
  return "unknown";
}

LoadedAudio load_audio_file_mono_48k(const char* path) {
  LoadedAudio loaded{};
  if (path == nullptr || path[0] == '\0') {
    loaded.error = "Enter a WAV or MP3 path first";
    return loaded;
  }

  ma_decoder_config config =
    ma_decoder_config_init(ma_format_f32, 1, static_cast<ma_uint32>(kSampleRateHz));
  ma_decoder decoder{};
  const ma_result init_result = ma_decoder_init_file(path, &config, &decoder);
  if (init_result != MA_SUCCESS) {
    loaded.error = std::string("Decode open failed: ") + ma_result_description(init_result);
    return loaded;
  }

  constexpr std::size_t kReadBufferFrames = 4096;
  constexpr std::size_t kMaxInputSamples =
    static_cast<std::size_t>(kMaxFileInputFrames) * kFrameSamples;
  std::array<float, kReadBufferFrames> read_buffer{};

  while (loaded.samples.size() < kMaxInputSamples) {
    const auto remaining_capacity = kMaxInputSamples - loaded.samples.size();
    const auto frames_to_read = std::min<std::size_t>(read_buffer.size(), remaining_capacity);
    ma_uint64 frames_read = 0;
    const ma_result read_result =
      ma_decoder_read_pcm_frames(&decoder, read_buffer.data(),
                                 static_cast<ma_uint64>(frames_to_read), &frames_read);
    if (frames_read > 0) {
      loaded.samples.insert(loaded.samples.end(), read_buffer.begin(),
                            read_buffer.begin() + static_cast<std::ptrdiff_t>(frames_read));
    }
    if (read_result == MA_AT_END || frames_read == 0) {
      break;
    }
    if (read_result != MA_SUCCESS) {
      loaded.error = std::string("Decode read failed: ") + ma_result_description(read_result);
      break;
    }
  }

  if (loaded.error.empty()) {
    ma_uint64 extra_frame = 0;
    float scratch = 0.0F;
    const ma_result extra_result =
      ma_decoder_read_pcm_frames(&decoder, &scratch, 1, &extra_frame);
    loaded.truncated = extra_result == MA_SUCCESS && extra_frame > 0;
  }

  ma_decoder_uninit(&decoder);

  if (!loaded.error.empty()) {
    loaded.samples.clear();
    return loaded;
  }
  if (loaded.samples.empty()) {
    loaded.error = "Decoded audio was empty";
    return loaded;
  }

  const int frame_count =
    static_cast<int>((loaded.samples.size() + kFrameSamples - 1U) / kFrameSamples);
  const double seconds =
    static_cast<double>(loaded.samples.size()) / static_cast<double>(kSampleRateHz);
  char status_buffer[192]{};
  std::snprintf(status_buffer, sizeof(status_buffer), "Loaded %.2fs as %d x 10ms frames%s",
                seconds, frame_count, loaded.truncated ? " (trimmed to first 5 min)" : "");
  loaded.status = status_buffer;
  return loaded;
}

RunResult run_simulation(const LabSettings& settings) {
  return udp_audio::sim::run_opus_playout_simulation(settings);
}

int random_seed() {
  static std::random_device random_device;
  static std::mt19937 rng(random_device());
  std::uniform_int_distribution<int> seed_distribution(1, 9999);
  return seed_distribution(rng);
}

RunResult run_with_seed_policy(LabSettings& settings,
                               PlaybackState& playback,
                               bool reroll_seed_on_run) {
  if (reroll_seed_on_run) {
    settings.seed = random_seed();
  }
  clear_playback(playback);
  return run_simulation(settings);
}

void load_selected_audio_path(const char* path,
                              LabSettings& settings,
                              PlaybackState& playback,
                              bool reroll_seed_on_run,
                              RunResult& result,
                              std::string& audio_load_status) {
  auto loaded = load_audio_file_mono_48k(path);
  if (loaded.error.empty()) {
    settings.input_mode = InputMode::file;
    settings.input_samples = std::move(loaded.samples);
    settings.input_label = path;
    settings.frame_count = static_cast<int>(
      (settings.input_samples.size() + kFrameSamples - 1U) / kFrameSamples);
    audio_load_status = loaded.status;
    result = run_with_seed_policy(settings, playback, reroll_seed_on_run);
  } else {
    audio_load_status = loaded.error;
  }
}

void apply_preset(LabSettings& settings,
                  int loss_percent,
                  int jitter_ms,
                  LossModel loss_model,
                  int burst_min_frames,
                  int burst_max_frames,
                  int redundancy_frames,
                  int jitter_depth_frames) {
  if (settings.input_mode == InputMode::generated) {
    settings.frame_count = 100;
  }
  settings.loss_percent = loss_percent;
  settings.jitter_ms = jitter_ms;
  settings.bitrate_bps = 64000;
  settings.loss_model = loss_model;
  settings.burst_min_frames = burst_min_frames;
  settings.burst_max_frames = burst_max_frames;
  settings.redundancy_frames = redundancy_frames;
  settings.jitter_depth_frames = jitter_depth_frames;
  settings.recovery_mode = RecoveryMode::fec;
}

void show_tooltip_if_hovered(const char* text) {
  if (!ImGui::IsItemHovered()) {
    return;
  }
  ImGui::BeginTooltip();
  ImGui::TextWrapped("%s", text);
  ImGui::EndTooltip();
}

void file_dialog_callback(void* userdata, const char* const* filelist, int filter) {
  static_cast<void>(filter);
  auto* state = static_cast<FileDialogState*>(userdata);
  std::lock_guard<std::mutex> lock(state->mutex);
  if (filelist == nullptr) {
    state->message = std::string("File picker failed: ") + SDL_GetError();
    return;
  }
  if (filelist[0] == nullptr) {
    state->message = "File selection canceled";
    return;
  }
  state->selected_path = filelist[0];
}

std::string format_playback_time(float seconds) {
  if (seconds < 0.0F) {
    return "--:--";
  }
  const int total_seconds = static_cast<int>(seconds);
  const int minutes = total_seconds / 60;
  const int remaining_seconds = total_seconds % 60;
  char buffer[16]{};
  std::snprintf(buffer, sizeof(buffer), "%d:%02d", minutes, remaining_seconds);
  return buffer;
}

bool draw_playback_bar(float cursor_seconds, float length_seconds, float& seek_seconds) {
  const ImGuiStyle& style = ImGui::GetStyle();
  const ImVec2 bar_size(std::max(160.0F, ImGui::GetContentRegionAvail().x), 18.0F);
  const ImVec2 bar_min = ImGui::GetCursorScreenPos();
  const ImVec2 bar_max(bar_min.x + bar_size.x, bar_min.y + bar_size.y);
  const float progress =
    length_seconds > 0.0F ? std::clamp(cursor_seconds / length_seconds, 0.0F, 1.0F) : 0.0F;

  ImGui::InvisibleButton("##playback_scrub_bar", bar_size);
  const bool hovered = ImGui::IsItemHovered();
  const bool active = ImGui::IsItemActive();

  ImDrawList* draw_list = ImGui::GetWindowDrawList();
  const ImU32 background_color = ImGui::GetColorU32(ImGuiCol_FrameBg);
  const ImU32 fill_color = ImGui::GetColorU32(ImGuiCol_ButtonActive);
  const ImU32 border_color = ImGui::GetColorU32(hovered || active ? ImGuiCol_Text : ImGuiCol_Border);
  const ImU32 playhead_color = ImGui::GetColorU32(ImGuiCol_Text);
  const float rounding = std::min(style.FrameRounding, 4.0F);

  draw_list->AddRectFilled(bar_min, bar_max, background_color, rounding);
  if (progress > 0.0F) {
    const ImVec2 fill_max(bar_min.x + bar_size.x * progress, bar_max.y);
    draw_list->AddRectFilled(bar_min, fill_max, fill_color, rounding);
  }

  const float playhead_x = bar_min.x + bar_size.x * progress;
  draw_list->AddLine(ImVec2(playhead_x, bar_min.y - 2.0F),
                     ImVec2(playhead_x, bar_max.y + 2.0F), playhead_color, 2.0F);
  draw_list->AddRect(bar_min, bar_max, border_color, rounding);

  if (length_seconds <= 0.0F) {
    return false;
  }

  if ((hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) ||
      (active && ImGui::IsMouseDown(ImGuiMouseButton_Left))) {
    const float mouse_x = ImGui::GetIO().MousePos.x;
    const float seek_percent = std::clamp((mouse_x - bar_min.x) / bar_size.x, 0.0F, 1.0F);
    seek_seconds = seek_percent * length_seconds;
    return true;
  }
  return false;
}

bool draw_scrubbable_playhead_on_last_item(float progress, float& seek_progress) {
  const ImVec2 item_min = ImGui::GetItemRectMin();
  const ImVec2 item_max = ImGui::GetItemRectMax();
  const bool hovered = ImGui::IsMouseHoveringRect(item_min, item_max);
  const float clamped_progress = std::clamp(progress, 0.0F, 1.0F);
  const float x = item_min.x + ((item_max.x - item_min.x) * clamped_progress);
  ImDrawList* draw_list = ImGui::GetWindowDrawList();
  if (hovered) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  }
  draw_list->AddLine(ImVec2(x, item_min.y), ImVec2(x, item_max.y),
                     ImGui::ColorConvertFloat4ToU32(ImVec4(1.0F, 0.86F, 0.18F, 1.0F)),
                     2.0F);
  if (hovered && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    const float mouse_x = ImGui::GetIO().MousePos.x;
    seek_progress = std::clamp((mouse_x - item_min.x) / (item_max.x - item_min.x), 0.0F, 1.0F);
    return true;
  }
  return false;
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

void draw_summary_row(const char* label, const char* value) {
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::TextDisabled("%s", label);
  ImGui::TableSetColumnIndex(1);
  ImGui::TextWrapped("%s", value);
}

void draw_summary_rowf(const char* label, const char* format, ...) {
  char buffer[256]{};
  va_list args;
  va_start(args, format);
  std::vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  draw_summary_row(label, buffer);
}

bool begin_summary_table(const char* id) {
  if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
    return false;
  }
  ImGui::TableSetupColumn("Metric", ImGuiTableColumnFlags_WidthFixed, 155.0F);
  ImGui::TableSetupColumn("Value");
  return true;
}

void apply_lossy_audio_theme() {
  ImGui::StyleColorsDark();
  ImGuiStyle& style = ImGui::GetStyle();
  style.WindowRounding = 4.0F;
  style.ChildRounding = 4.0F;
  style.FrameRounding = 3.0F;
  style.GrabRounding = 3.0F;
  style.ScrollbarRounding = 4.0F;
  style.TabRounding = 3.0F;

  ImVec4* colors = style.Colors;
  colors[ImGuiCol_Text] = ImVec4(0.94F, 0.91F, 0.88F, 1.0F);
  colors[ImGuiCol_TextDisabled] = ImVec4(0.68F, 0.60F, 0.58F, 1.0F);
  colors[ImGuiCol_WindowBg] = ImVec4(0.075F, 0.065F, 0.065F, 1.0F);
  colors[ImGuiCol_ChildBg] = ImVec4(0.095F, 0.078F, 0.078F, 1.0F);
  colors[ImGuiCol_PopupBg] = ImVec4(0.12F, 0.08F, 0.08F, 1.0F);
  colors[ImGuiCol_Border] = ImVec4(0.36F, 0.14F, 0.13F, 1.0F);
  colors[ImGuiCol_FrameBg] = ImVec4(0.22F, 0.08F, 0.08F, 1.0F);
  colors[ImGuiCol_FrameBgHovered] = ImVec4(0.34F, 0.11F, 0.10F, 1.0F);
  colors[ImGuiCol_FrameBgActive] = ImVec4(0.48F, 0.14F, 0.12F, 1.0F);
  colors[ImGuiCol_TitleBg] = ImVec4(0.18F, 0.06F, 0.06F, 1.0F);
  colors[ImGuiCol_TitleBgActive] = ImVec4(0.35F, 0.08F, 0.08F, 1.0F);
  colors[ImGuiCol_CheckMark] = ImVec4(0.95F, 0.24F, 0.18F, 1.0F);
  colors[ImGuiCol_SliderGrab] = ImVec4(0.78F, 0.16F, 0.13F, 1.0F);
  colors[ImGuiCol_SliderGrabActive] = ImVec4(1.0F, 0.25F, 0.18F, 1.0F);
  colors[ImGuiCol_Button] = ImVec4(0.38F, 0.09F, 0.08F, 1.0F);
  colors[ImGuiCol_ButtonHovered] = ImVec4(0.56F, 0.13F, 0.10F, 1.0F);
  colors[ImGuiCol_ButtonActive] = ImVec4(0.74F, 0.16F, 0.12F, 1.0F);
  colors[ImGuiCol_Header] = ImVec4(0.35F, 0.09F, 0.08F, 1.0F);
  colors[ImGuiCol_HeaderHovered] = ImVec4(0.52F, 0.12F, 0.10F, 1.0F);
  colors[ImGuiCol_HeaderActive] = ImVec4(0.70F, 0.16F, 0.12F, 1.0F);
  colors[ImGuiCol_Separator] = ImVec4(0.38F, 0.13F, 0.12F, 1.0F);
  colors[ImGuiCol_SeparatorHovered] = ImVec4(0.65F, 0.18F, 0.14F, 1.0F);
  colors[ImGuiCol_SeparatorActive] = ImVec4(0.85F, 0.22F, 0.16F, 1.0F);
  colors[ImGuiCol_ResizeGrip] = ImVec4(0.50F, 0.12F, 0.10F, 0.55F);
  colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.75F, 0.18F, 0.13F, 0.70F);
  colors[ImGuiCol_ResizeGripActive] = ImVec4(0.95F, 0.24F, 0.18F, 0.90F);
  colors[ImGuiCol_PlotLines] = ImVec4(0.95F, 0.70F, 0.58F, 1.0F);
  colors[ImGuiCol_PlotHistogram] = ImVec4(0.86F, 0.22F, 0.16F, 1.0F);
  colors[ImGuiCol_TableHeaderBg] = ImVec4(0.22F, 0.08F, 0.08F, 1.0F);
  colors[ImGuiCol_TableBorderStrong] = ImVec4(0.38F, 0.13F, 0.12F, 1.0F);
  colors[ImGuiCol_TableBorderLight] = ImVec4(0.24F, 0.10F, 0.09F, 1.0F);
  colors[ImGuiCol_TableRowBg] = ImVec4(0.10F, 0.075F, 0.075F, 1.0F);
  colors[ImGuiCol_TableRowBgAlt] = ImVec4(0.14F, 0.075F, 0.075F, 1.0F);
  colors[ImGuiCol_ScrollbarBg] = ImVec4(0.08F, 0.06F, 0.06F, 1.0F);
  colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.33F, 0.10F, 0.09F, 1.0F);
  colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.50F, 0.13F, 0.11F, 1.0F);
  colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.72F, 0.18F, 0.13F, 1.0F);
}

float half_width_button() {
  const float spacing = ImGui::GetStyle().ItemSpacing.x;
  return (ImGui::GetContentRegionAvail().x - spacing) * 0.5F;
}

void print_headless_summary(const LabSettings& settings, const RunResult& result) {
  SDL_Log("input=%s source=%s label=%s input_frames=%d padded_input_samples=%d",
          udp_audio::sim::input_mode_name(settings.input_mode),
          settings.input_mode == InputMode::generated ? source_name(settings.source_mode) : "file",
          result.input_label.empty() ? "(none)" : result.input_label.c_str(),
          result.input_frames, result.padded_input_samples);
  SDL_Log("loss_model=%s loss_bursts=%d max_loss_burst_frames=%d",
          udp_audio::sim::loss_model_name(settings.loss_model), result.loss_bursts,
          result.max_loss_burst_frames);
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
    print_headless_summary(settings, result);
    return result.error.empty() ? 0 : 1;
  }
  if (argc > 2 && std::string(argv[1]) == "--headless-file") {
    LabSettings settings{};
    auto loaded = load_audio_file_mono_48k(argv[2]);
    if (!loaded.error.empty()) {
      SDL_Log("%s", loaded.error.c_str());
      return 1;
    }
    settings.input_mode = InputMode::file;
    settings.input_samples = std::move(loaded.samples);
    settings.input_label = argv[2];
    settings.frame_count =
      static_cast<int>((settings.input_samples.size() + kFrameSamples - 1U) / kFrameSamples);
    const auto result = run_simulation(settings);
    print_headless_summary(settings, result);
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
    SDL_CreateWindow("Lossy Audio Lab", 1320, 860,
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
  apply_lossy_audio_theme();

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
  settings.seed = random_seed();
  RunResult result = run_simulation(settings);
  std::array<char, 1024> audio_path{};
  std::string audio_load_status = "Using generated source";
  FileDialogState file_dialog_state{};
  bool file_dialog_open = false;
  bool reroll_seed_on_run = false;
  bool done = false;
  static constexpr SDL_DialogFileFilter kAudioFileFilters[] = {
    {"Audio Files", "wav;mp3"},
    {"WAV", "wav"},
    {"MP3", "mp3"},
  };

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
    {
      std::optional<std::string> selected_path;
      std::optional<std::string> dialog_message;
      {
        std::lock_guard<std::mutex> lock(file_dialog_state.mutex);
        selected_path = std::move(file_dialog_state.selected_path);
        dialog_message = std::move(file_dialog_state.message);
        file_dialog_state.selected_path.reset();
        file_dialog_state.message.reset();
      }
      if (selected_path.has_value()) {
        std::snprintf(audio_path.data(), audio_path.size(), "%s", selected_path->c_str());
        file_dialog_open = false;
        load_selected_audio_path(audio_path.data(), settings, playback, reroll_seed_on_run,
                                 result, audio_load_status);
      }
      if (dialog_message.has_value()) {
        file_dialog_open = false;
        audio_load_status = *dialog_message;
      }
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
    ImGui::Begin("Lossy Audio Lab", nullptr,
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoCollapse);

    const float top_panel_height =
      std::clamp(ImGui::GetContentRegionAvail().y * 0.58F, 430.0F, 640.0F);
    if (ImGui::BeginTable("main_layout", 2,
                          ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV |
                            ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn("Controls", ImGuiTableColumnFlags_WidthFixed, 360.0F);
      ImGui::TableSetupColumn("Results", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableNextRow();

      ImGui::TableSetColumnIndex(0);
      ImGui::BeginChild("controls_panel", ImVec2(0.0F, top_panel_height), false,
                        ImGuiWindowFlags_None);

      ImGui::TextUnformatted("Presets");
      ImGui::Separator();
      const float preset_width = half_width_button();
        if (ImGui::Button("Clean LAN", ImVec2(preset_width, 0))) {
          apply_preset(settings, 0, 1, LossModel::independent, 2, 4, 0, 3);
          result = run_with_seed_policy(settings, playback, reroll_seed_on_run);
        }
        ImGui::SameLine();
        if (ImGui::Button("Room Wi-Fi", ImVec2(preset_width, 0))) {
          apply_preset(settings, 3, 8, LossModel::independent, 2, 4, 1, 4);
          result = run_with_seed_policy(settings, playback, reroll_seed_on_run);
        }
        if (ImGui::Button("Busy Wi-Fi", ImVec2(preset_width, 0))) {
          apply_preset(settings, 10, 20, LossModel::burst, 2, 3, 2, 5);
          result = run_with_seed_policy(settings, playback, reroll_seed_on_run);
        }
        ImGui::SameLine();
        if (ImGui::Button("Weak Wi-Fi", ImVec2(preset_width, 0))) {
          apply_preset(settings, 14, 30, LossModel::burst, 2, 4, 3, 7);
          result = run_with_seed_policy(settings, playback, reroll_seed_on_run);
        }
        if (ImGui::Button("Hotspot", ImVec2(preset_width, 0))) {
          apply_preset(settings, 20, 45, LossModel::burst, 3, 6, 3, 8);
          result = run_with_seed_policy(settings, playback, reroll_seed_on_run);
        }
        ImGui::SameLine();
        if (ImGui::Button("Stress", ImVec2(preset_width, 0))) {
          apply_preset(settings, 35, 65, LossModel::burst, 4, 8, 3, 10);
          result = run_with_seed_policy(settings, playback, reroll_seed_on_run);
        }

      ImGui::Spacing();
      ImGui::TextUnformatted("Transport");
      ImGui::Separator();
        if (ImGui::Button("Run", ImVec2(100, 0))) {
          result = run_with_seed_policy(settings, playback, reroll_seed_on_run);
        }
        ImGui::SameLine();
        if (ImGui::Button("Play", ImVec2(100, 0)) && audio_ok && !result.samples.empty()) {
          resume_or_start_playback(playback, result.samples);
        }
        ImGui::SameLine();
        if (ImGui::Button("Stop", ImVec2(100, 0))) {
          stop_playback(playback);
        }

        const auto position = playback_position(playback);
        const auto playback_length_samples = result.samples.size();
        const auto playback_cursor_samples = std::min(position.cursor, playback_length_samples);
        const float cursor_seconds =
          static_cast<float>(playback_cursor_samples) / static_cast<float>(kSampleRateHz);
        const float length_seconds =
          static_cast<float>(playback_length_samples) / static_cast<float>(kSampleRateHz);
        const std::string time_text =
          format_playback_time(cursor_seconds) + " / " + format_playback_time(length_seconds);
        ImGui::TextUnformatted(time_text.c_str());
        float seek_seconds = cursor_seconds;
        if (draw_playback_bar(cursor_seconds, length_seconds, seek_seconds) &&
            !result.samples.empty()) {
          seek_playback_buffer(playback, result.samples, seek_seconds / length_seconds, true);
        }
        ImGui::Text("Audio device: %s", audio_ok ? "ready" : "unavailable");
        if (!result.error.empty()) {
          ImGui::TextColored(ImVec4(1.0F, 0.25F, 0.25F, 1.0F), "%s", result.error.c_str());
        }

      ImGui::Spacing();
      ImGui::TextUnformatted("Audio Source");
      ImGui::Separator();
        ImGui::InputText("WAV/MP3 Path", audio_path.data(), audio_path.size());
        if (ImGui::Button("Browse", ImVec2(100, 0)) && !file_dialog_open) {
          file_dialog_open = true;
          SDL_ShowOpenFileDialog(file_dialog_callback, &file_dialog_state, window,
                                 kAudioFileFilters,
                                 static_cast<int>(std::size(kAudioFileFilters)), nullptr,
                                 false);
        }
        ImGui::SameLine();
        if (ImGui::Button("Load Audio", ImVec2(100, 0))) {
          load_selected_audio_path(audio_path.data(), settings, playback, reroll_seed_on_run,
                                   result, audio_load_status);
        }
        ImGui::TextWrapped("%s", audio_load_status.c_str());

      ImGui::Spacing();
      ImGui::TextUnformatted("Diagnostic Sources");
      show_tooltip_if_hovered(
        "Sine and chirp are synthetic probes for checking recovery behavior. Load WAV/MP3 for normal listening previews.");
      ImGui::Separator();
        int source_index = settings.source_mode == SourceMode::chirp ? 1 : 0;
        if (ImGui::Combo("Test Signal", &source_index, "sine\0chirp\0")) {
          settings.source_mode = source_index == 1 ? SourceMode::chirp : SourceMode::sine;
          settings.input_mode = InputMode::generated;
          audio_load_status = "Using diagnostic source";
        }
        if (ImGui::Button("Use Diagnostic Source", ImVec2(-1.0F, 0))) {
          settings.input_mode = InputMode::generated;
          settings.input_label.clear();
          audio_load_status = "Using diagnostic source";
          result = run_with_seed_policy(settings, playback, reroll_seed_on_run);
        }

      ImGui::Spacing();
      ImGui::TextUnformatted("Network");
      ImGui::Separator();
        if (settings.input_mode == InputMode::file) {
          ImGui::BeginDisabled();
        }
        ImGui::SliderInt("Frames", &settings.frame_count, 20, 500);
        if (settings.input_mode == InputMode::file) {
          ImGui::EndDisabled();
        }
        ImGui::SliderInt("Loss %", &settings.loss_percent, 0, 60);
        ImGui::SliderInt("Jitter ms", &settings.jitter_ms, 0, 80);
        ImGui::SliderInt("Seed", &settings.seed, 1, 9999);
        if (ImGui::Button("New Seed", ImVec2(110, 0))) {
          settings.seed = random_seed();
          result = run_with_seed_policy(settings, playback, false);
        }
        ImGui::SameLine();
        ImGui::Checkbox("Reroll Seed On Run", &reroll_seed_on_run);
        int loss_model_index = settings.loss_model == LossModel::burst ? 1 : 0;
        if (ImGui::Combo("Loss Model", &loss_model_index, "independent\0burst\0")) {
          settings.loss_model =
            loss_model_index == 1 ? LossModel::burst : LossModel::independent;
        }
        ImGui::SliderInt("Burst Min", &settings.burst_min_frames, 1, 12);
        ImGui::SliderInt("Burst Max", &settings.burst_max_frames, 1, 16);
        if (settings.burst_max_frames < settings.burst_min_frames) {
          settings.burst_max_frames = settings.burst_min_frames;
        }
        ImGui::SliderInt("Bitrate", &settings.bitrate_bps, 16000, 128000);
        ImGui::SliderInt("Redundant Frames", &settings.redundancy_frames, 0,
                         kMaxRedundancyFrames);
        ImGui::SliderInt("Jitter Depth", &settings.jitter_depth_frames, 1, 12);

      ImGui::Spacing();
      ImGui::TextUnformatted("Recovery");
      ImGui::Separator();
        int recovery_index = settings.recovery_mode == RecoveryMode::fec ? 1 : 0;
        if (ImGui::Combo("Recovery", &recovery_index, "plc\0fec\0")) {
          settings.recovery_mode =
            recovery_index == 1 ? RecoveryMode::fec : RecoveryMode::plc;
        }

      ImGui::EndChild();

      ImGui::TableSetColumnIndex(1);
      ImGui::BeginChild("summary_panel", ImVec2(0.0F, top_panel_height), false,
                        ImGuiWindowFlags_None);

      ImGui::TextUnformatted("Summary");
      ImGui::Separator();
      constexpr float kTimelinePanelHeight = 92.0F;
      const float summary_metrics_height =
        std::max(170.0F, ImGui::GetContentRegionAvail().y - kTimelinePanelHeight);
      ImGui::BeginChild("summary_metrics_panel", ImVec2(0.0F, summary_metrics_height), false,
                        ImGuiWindowFlags_AlwaysVerticalScrollbar);

      ImGui::TextUnformatted("Input");
      if (begin_summary_table("input_summary")) {
        draw_summary_row("Mode", udp_audio::sim::input_mode_name(settings.input_mode));
        draw_summary_row("Source",
                         settings.input_mode == InputMode::generated
                           ? source_name(settings.source_mode)
                           : "file");
        if (settings.input_mode == InputMode::file) {
          draw_summary_row("File", settings.input_label.c_str());
          draw_summary_rowf("Input frames", "%d", result.input_frames);
          draw_summary_rowf("Padded samples", "%d", result.padded_input_samples);
        } else {
          draw_summary_rowf("Frames", "%d", result.generated);
        }
        ImGui::EndTable();
      }

      ImGui::Spacing();
      ImGui::TextUnformatted("Network Impairment");
      if (begin_summary_table("network_summary")) {
        draw_summary_row("Loss model", udp_audio::sim::loss_model_name(settings.loss_model));
        draw_summary_rowf("Configured loss", "%d%%", settings.loss_percent);
        draw_summary_rowf("Configured jitter", "%d ms", settings.jitter_ms);
        draw_summary_rowf("Dropped frames", "%d / %d", result.dropped, result.generated);
        draw_summary_rowf("Loss bursts", "%d", result.loss_bursts);
        draw_summary_rowf("Max burst", "%d frames", result.max_loss_burst_frames);
        ImGui::EndTable();
      }

      ImGui::Spacing();
      ImGui::TextUnformatted("Recovery");
      if (begin_summary_table("recovery_summary")) {
        draw_summary_row("Mode",
                         settings.recovery_mode == RecoveryMode::fec ? "fec" : "plc");
        draw_summary_rowf("Decoded", "%d", result.decoded);
        draw_summary_rowf("Redundant repair", "%d", result.redundant);
        draw_summary_rowf("FEC attempts", "%d", result.fec_attempts);
        draw_summary_rowf("PLC fallback", "%d", result.plc);
        draw_summary_rowf("Late or missing", "%d", result.late_or_missing);
        draw_summary_rowf("Decode errors", "%d", result.decode_errors);
        ImGui::EndTable();
      }

      ImGui::Spacing();
      ImGui::TextUnformatted("Latency");
      if (begin_summary_table("latency_summary")) {
        draw_summary_rowf("Network avg", "%.3f ms", result.avg_latency_ms);
        draw_summary_rowf("Network max", "%.3f ms", result.max_latency_ms);
        draw_summary_rowf("Playout avg", "%.3f ms", result.avg_playout_ms);
        draw_summary_rowf("Playout max", "%.3f ms", result.max_playout_ms);
        ImGui::EndTable();
      }

      ImGui::Spacing();
      ImGui::TextUnformatted("Codec / Runtime");
      if (begin_summary_table("runtime_summary")) {
        draw_summary_rowf("Bitrate", "%d bps", settings.bitrate_bps);
        draw_summary_rowf("Packet avg", "%.2f bytes", result.avg_packet_bytes);
        draw_summary_rowf("Redundancy avg", "%.2f bytes", result.avg_redundancy_bytes);
        draw_summary_rowf("Simulation", "%.3f ms", result.elapsed_ms);
        draw_summary_rowf("Rendered samples", "%llu",
                          static_cast<unsigned long long>(
                            playback.rendered_samples.load(std::memory_order_relaxed)));
        draw_summary_rowf("Callback underruns", "%llu",
                          static_cast<unsigned long long>(
                            playback.callback_underruns.load(std::memory_order_relaxed)));
        ImGui::EndTable();
      }

      ImGui::EndChild();
      ImGui::Spacing();
      ImGui::Separator();
      ImGui::TextUnformatted("Recovery Timeline");
      draw_recovery_strip(result);
      ImGui::TextColored(ImColor(status_color(FrameStatus::decoded)), "decoded");
      ImGui::SameLine();
      ImGui::TextColored(ImColor(status_color(FrameStatus::redundant)), "redundant");
      ImGui::SameLine();
      ImGui::TextColored(ImColor(status_color(FrameStatus::fec_attempt)), "fec");
      ImGui::SameLine();
      ImGui::TextColored(ImColor(status_color(FrameStatus::plc)), "plc");

      ImGui::EndChild();
      ImGui::EndTable();
    }

    ImGui::Spacing();

        if (!result.waveform_plot.empty()) {
          ImGui::TextUnformatted("Output Waveform");
          const float progress = playback_progress(playback);
          ImGui::PlotLines("##waveform", result.waveform_plot.data(),
                           static_cast<int>(result.waveform_plot.size()), 0, nullptr, -0.25F,
                           0.25F, ImVec2(-1.0F, 170.0F));
          float waveform_seek_progress = progress;
          if (draw_scrubbable_playhead_on_last_item(progress, waveform_seek_progress)) {
            seek_playback_buffer(playback, result.samples, waveform_seek_progress, true);
          }
        }

        if (!result.frame_energy.empty()) {
          ImGui::TextUnformatted("Frame RMS");
          ImGui::PlotLines("##energy", result.frame_energy.data(),
                           static_cast<int>(result.frame_energy.size()), 0, nullptr, 0.0F,
                           0.22F, ImVec2(-1.0F, 120.0F));
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
