// SPDX-License-Identifier: Apache-2.0 OR MIT
// Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
//
// hdrg-gui: opens a still or a video, plays it graded on an HDR window (SDL3's GPU swapchain in
// extended linear sRGB: EDR on macOS, scRGB on Windows; SDR where the display has no headroom),
// with every setting of the library (hdrg_setting_at) and the source's SDR to compare, and
// writes the still (hdrg_convert) or the video (gui.h's export).

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "gui.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"

namespace {

const char *const kPresets[] = {"写真 (photo)", "アニメ (anime)", "ゲーム (game)"};
const char *const kTemporal[] = {"WINDOWED", "RECURSIVE", "TWO_PASS", "STATIC"};
const char *const kGpu[] = {"OFF", "AUTO", "FILTERS"};

struct App {
    SDL_Window *window = nullptr;
    SDL_GPUDevice *device = nullptr;
    SDL_GPUTexture *texture = nullptr;
    SDL_GPUTransferBuffer *transfer = nullptr;
    uint32_t texture_width = 0, texture_height = 0;
    uint64_t uploaded = 0;
    bool hdr_swapchain = false;
    float white_level = 1.0f, headroom = 1.0f;

    hdrg_gui_player_t *player = nullptr;
    hdrg_gui_export_t *job = nullptr;
    hdrg_gui_settings_t settings{};
    int preset = 0;
    bool follow_window = true, playing = false;
    int view = HDRG_GUI_VIEW_OUTPUT;
    // where the picture went (swapchain pixels) and its width: the labels of the side-by-side view
    SDL_FRect shown{};
    bool side_by_side = false;
    float drag_seconds = -1.0f;
    std::string path, message;

    SDL_Mutex *chosen_lock = nullptr;
    std::string chosen_open, chosen_save;

    // the writers of stills (the built-in codecs' and the modules' beside the program): their ids,
    // the combo's labels and the save dialog's filters (their names and extensions kept here), their
    // options (key, type, default and range) and the values chosen (text, by writer and key)
    struct Option {
        std::string key, description, default_text;
        uint32_t type;
        int64_t default_int, min_int, max_int;
        double default_float, min_float, max_float;
    };
    std::vector<std::string> writers, writer_labels, writer_names, writer_patterns;
    std::vector<std::vector<Option>> writer_options;
    std::map<std::string, std::map<std::string, std::string>> option_values;
    std::vector<SDL_DialogFileFilter> still_filters;
};

// the writers a context of the library has, the codec modules beside the program loaded
void ListWriters(App &app)
{
    hdrg_context_t *ctx = nullptr;

    if (hdrg_context_create(nullptr, &ctx) != HDRG_OK) {
        return;
    }

    hdrg_gui_load_modules(ctx);

    for (uint32_t i = 0; i < hdrg_context_codec_count(ctx); i++) {
        hdrg_codec_info_t codec;

        codec.struct_size = sizeof(codec);

        if (hdrg_context_codec_info(ctx, i, &codec) != HDRG_OK || (codec.roles & HDRG_CODEC_WRITE) == 0) {
            continue;
        }

        std::string extensions(codec.extensions);
        std::string pattern = extensions;

        for (char &c : pattern) {
            c = (c == ',') ? ';' : c;
        }

        std::vector<App::Option> options;

        for (uint32_t k = 0; k < codec.option_count; k++) {
            hdrg_option_info_t info;

            info.struct_size = sizeof(info);

            if (hdrg_context_codec_option(ctx, i, k, &info) == HDRG_OK) {
                options.push_back({info.key, (info.description != nullptr) ? info.description : "",
                                   (info.default_text != nullptr) ? info.default_text : "", info.type,
                                   info.default_int, info.min_int, info.max_int, info.default_float,
                                   info.min_float, info.max_float});
            }
        }

        app.writer_options.push_back(options);
        app.writers.emplace_back(codec.id);
        app.writer_labels.push_back(std::string(codec.id) + " (." + extensions.substr(0, extensions.find(',')) + ")");
        app.writer_names.push_back(std::string(codec.id).substr(std::string(codec.id).rfind('.') + 1));
        app.writer_patterns.push_back(pattern);
    }

    hdrg_context_destroy(ctx);

    for (size_t i = 0; i < app.writers.size(); i++) {
        app.still_filters.push_back({app.writer_names[i].c_str(), app.writer_patterns[i].c_str()});
    }
}

// the still's writer: the output's extension's, or one chosen
void FormatWidget(App &app)
{
    std::vector<const char *> items{"出力の拡張子から"};
    int chosen = 0;

    for (size_t i = 0; i < app.writers.size(); i++) {
        items.push_back(app.writer_labels[i].c_str());

        if (app.writers[i] == app.settings.format) {
            chosen = static_cast<int>(i) + 1;
        }
    }

    if (ImGui::Combo("形式 (静止画)", &chosen, items.data(), static_cast<int>(items.size()))) {
        std::snprintf(app.settings.format, sizeof(app.settings.format), "%s",
                      (chosen > 0) ? app.writers[static_cast<size_t>(chosen - 1)].c_str() : "");
    }

    if (chosen == 0) {
        app.settings.format_option_count = 0;

        return;
    }

    // the chosen writer's options, their defaults until changed; the changed ones into the settings
    const std::string &id = app.writers[static_cast<size_t>(chosen - 1)];
    std::map<std::string, std::string> &values = app.option_values[id];

    ImGui::PushID(id.c_str());

    for (const App::Option &option : app.writer_options[static_cast<size_t>(chosen - 1)]) {
        auto found = values.find(option.key);
        std::string label = option.key;

        if (option.type == HDRG_OPTION_INT) {
            int value = static_cast<int>((found != values.end()) ? std::atoll(found->second.c_str()) : option.default_int);

            if (ImGui::SliderInt(label.c_str(), &value, static_cast<int>(option.min_int), static_cast<int>(option.max_int))) {
                values[option.key] = std::to_string(value);
            }
        } else if (option.type == HDRG_OPTION_FLOAT) {
            float value = static_cast<float>((found != values.end()) ? std::atof(found->second.c_str()) : option.default_float);

            if (ImGui::SliderFloat(label.c_str(), &value, static_cast<float>(option.min_float), static_cast<float>(option.max_float))) {
                char text[64];

                std::snprintf(text, sizeof(text), "%.6g", static_cast<double>(value));
                values[option.key] = text;
            }
        } else {
            char text[HDRG_GUI_FORMAT_OPTION];

            std::snprintf(text, sizeof(text), "%s", (found != values.end()) ? found->second.c_str() : option.default_text.c_str());

            if (ImGui::InputText(label.c_str(), text, sizeof(text))) {
                values[option.key] = text;
            }
        }

        if (ImGui::IsItemHovered() && !option.description.empty()) {
            ImGui::SetTooltip("%s", option.description.c_str());
        }
    }

    ImGui::PopID();
    app.settings.format_option_count = 0;

    for (const auto &[key, value] : values) {
        if (app.settings.format_option_count < HDRG_GUI_FORMAT_OPTIONS &&
            key.size() + 1u + value.size() < HDRG_GUI_FORMAT_OPTION) {
            std::snprintf(app.settings.format_options[app.settings.format_option_count++], HDRG_GUI_FORMAT_OPTION, "%s=%s",
                          key.c_str(), value.c_str());
        }
    }
}

void ToLinearStyle()
{
    ImGuiStyle &style = ImGui::GetStyle();

    for (ImVec4 &color : style.Colors) {
        color.x = std::pow(color.x, 2.2f);
        color.y = std::pow(color.y, 2.2f);
        color.z = std::pow(color.z, 2.2f);
    }
}

// the window's SDR white (linear sRGB) and headroom; the display's settings from them
void ReadDisplay(App &app)
{
    SDL_PropertiesID props = SDL_GetWindowProperties(app.window);
    bool hdr = SDL_GetBooleanProperty(props, SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN, false);

    app.white_level = app.hdr_swapchain
                          ? SDL_GetFloatProperty(props, SDL_PROP_WINDOW_SDR_WHITE_LEVEL_FLOAT, 1.0f)
                          : 1.0f;
    app.headroom = hdr ? SDL_GetFloatProperty(props, SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT, 1.0f) : 1.0f;

    if (app.follow_window) {
        app.settings.display.headroom_stops =
            (app.hdr_swapchain && app.headroom > 1.0f) ? std::log2(app.headroom) : -1.0f;
    }
}

void Apply(App &app)
{
    if (app.player != nullptr) {
        hdrg_gui_player_settings(app.player, &app.settings);
    }
}

void Open(App &app, const std::string &path)
{
    char error[HDRG_GUI_ERROR];

    hdrg_gui_player_close(app.player);
    app.player = nullptr;
    app.uploaded = 0;

    if (!hdrg_gui_player_open(path.c_str(), &app.settings, &app.player, error, sizeof(error))) {
        app.message = std::string("開けません: ") + error;

        return;
    }

    app.path = path;
    app.message.clear();
    app.playing = false;
    hdrg_gui_player_view(app.player, app.white_level, static_cast<uint8_t>(app.view));
    hdrg_gui_player_settings(app.player, &app.settings);
}

void SDLCALL OnOpen(void *user, const char *const *files, int)
{
    App *app = static_cast<App *>(user);

    if (files != nullptr && files[0] != nullptr) {
        SDL_LockMutex(app->chosen_lock);
        app->chosen_open = files[0];
        SDL_UnlockMutex(app->chosen_lock);
    }
}

void SDLCALL OnSave(void *user, const char *const *files, int)
{
    App *app = static_cast<App *>(user);

    if (files != nullptr && files[0] != nullptr) {
        SDL_LockMutex(app->chosen_lock);
        app->chosen_save = files[0];
        SDL_UnlockMutex(app->chosen_lock);
    }
}

// a setting's widget (hdrg_setting_at): true when it changed
bool SettingWidget(App &app, const hdrg_setting_t &setting)
{
    double value = 0.0;
    bool changed = false;

    if (hdrg_setting_get(setting.name, &app.settings.params, &app.settings.display,
                         &app.settings.encode, &value) != HDRG_OK) {
        return false;
    }

    ImGui::PushID(setting.name);

    if (setting.kind == HDRG_SETTING_BOOL) {
        bool on = value != 0.0;

        if (ImGui::Checkbox(setting.name, &on)) {
            value = on ? 1.0 : 0.0;
            changed = true;
        }
    } else if (setting.values > 0) {
        // an enumerated one: its values' names (the library's table)
        const char *names[16];
        double values[16];
        int count = 0, current = 0;

        while (count < 16 && hdrg_setting_value_at(setting.name, static_cast<uint32_t>(count),
                                                   &names[count], &values[count])) {
            current = (values[count] == value) ? count : current;
            count++;
        }

        if (ImGui::Combo(setting.name, &current, names, count)) {
            value = values[current];
            changed = true;
        }
    } else if (setting.kind == HDRG_SETTING_FLOAT) {
        float f = static_cast<float>(value);
        float speed = (std::fabs(f) >= 100.0f) ? 1.0f : 0.01f;

        if (ImGui::DragFloat(setting.name, &f, speed, 0.0f, 0.0f, "%.3g")) {
            value = f;
            changed = true;
        }
    } else {
        int i = static_cast<int>(value);

        if (ImGui::InputInt(setting.name, &i)) {
            value = (i < 0) ? 0 : i;
            changed = true;
        }
    }

    ImGui::PopID();

    if (changed) {
        hdrg_gui_settings_t next = app.settings;

        if (hdrg_setting_set(setting.name, value, &next.params, &next.display, &next.encode) ==
            HDRG_OK) {
            app.settings = next;

            if (setting.part == HDRG_SETTING_DISPLAY && std::strcmp(setting.name, "display-headroom") == 0) {
                app.follow_window = false;
            }

            return true;
        }
    }

    return false;
}

void SettingsWindow(App &app)
{
    hdrg_setting_t setting;
    bool changed = false;
    int temporal = app.settings.temporal, gpu = app.settings.gpu;

    ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(380, 640), ImGuiCond_FirstUseEver);
    ImGui::Begin("設定");

    if (ImGui::Combo("プリセット", &app.preset, kPresets, 3)) {
        hdrg_display_caps_t display = app.settings.display;
        hdrg_encode_params_t encode = app.settings.encode;
        uint8_t keep_temporal = app.settings.temporal, keep_gpu = app.settings.gpu;

        hdrg_gui_settings_init(&app.settings, static_cast<uint8_t>(app.preset));
        app.settings.display = display;
        app.settings.encode = encode;
        app.settings.temporal = keep_temporal;
        app.settings.gpu = keep_gpu;
        changed = true;
    }

    if (ImGui::Combo("時間モード", &temporal, kTemporal, 4) && temporal != HDRG_TEMPORAL_TWO_PASS) {
        app.settings.temporal = static_cast<uint8_t>(temporal);
        changed = true;
    }

    if (ImGui::Combo("GPU", &gpu, kGpu, 3)) {
        app.settings.gpu = static_cast<uint8_t>(gpu);
        changed = true;
    }

    if (ImGui::Checkbox("ウィンドウのヘッドルームに合わせる", &app.follow_window)) {
        ReadDisplay(app);
        changed = true;
    }

    for (uint8_t part : {HDRG_SETTING_INTENT, HDRG_SETTING_DISPLAY, HDRG_SETTING_ENCODE}) {
        const char *title = (part == HDRG_SETTING_INTENT)    ? "コンテンツの意図"
                            : (part == HDRG_SETTING_DISPLAY) ? "表示"
                                                             : "書き出し";

        if (!ImGui::CollapsingHeader(title, (part == HDRG_SETTING_INTENT) ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
            continue;
        }

        if (part == HDRG_SETTING_ENCODE) {
            FormatWidget(app);
        }

        // (the preset is the combo above: setting it initializes the intent)
        for (uint32_t i = 0; hdrg_setting_at(i, &setting); i++) {
            if (setting.part == part && std::strcmp(setting.name, "preset") != 0 &&
                SettingWidget(app, setting)) {
                changed = true;
            }
        }
    }

    if (changed) {
        Apply(app);
    }

    ImGui::End();
}

void PlayerWindow(App &app)
{
    hdrg_gui_status_t status{};
    std::string title;

    ImGui::SetNextWindowPos(ImVec2(12, 664), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(380, 220), ImGuiCond_FirstUseEver);
    ImGui::Begin("再生と書き出し");

    if (ImGui::Button("開く…")) {
        SDL_ShowOpenFileDialog(OnOpen, &app, app.window, nullptr, 0, nullptr, false);
    }

    ImGui::SameLine();

    if (ImGui::Button("書き出す…") && app.player != nullptr && app.job == nullptr) {
#if defined(_WIN32)
        static const SDL_DialogFileFilter videos[] = {{"MP4", "mp4"}};
#elif defined(__APPLE__)
        static const SDL_DialogFileFilter videos[] = {{"QuickTime / MP4", "mov;mp4"}};
#else
        static const SDL_DialogFileFilter videos[] = {{"MP4 / Matroska / QuickTime", "mp4;mkv;mov"}};
#endif

        hdrg_gui_player_status(app.player, &status);
        SDL_ShowSaveFileDialog(OnSave, &app, app.window,
                               (status.info.rate_num == 0) ? app.still_filters.data() : videos,
                               (status.info.rate_num == 0) ? static_cast<int>(app.still_filters.size()) : 1,
                               nullptr);
    }

    if (app.player != nullptr) {
        hdrg_gui_player_status(app.player, &status);

        if (status.info.rate_num != 0) {
            if (ImGui::Button(app.playing ? "一時停止" : "再生")) {
                app.playing = !app.playing;
                hdrg_gui_player_play(app.player, app.playing);
            }

            if (!status.playing && app.playing) {
                app.playing = false;
            }

            float seconds = (app.drag_seconds >= 0.0f) ? app.drag_seconds
                                                       : static_cast<float>(status.position_us) / 1e6f;
            float total = static_cast<float>(status.info.duration_us) / 1e6f;

            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1);

            // the seek where the slider is let go (a session made again for each step would stall)
            if (ImGui::SliderFloat("##position", &seconds, 0.0f, (total > 0.0f) ? total : 1.0f, "%.2f s")) {
                app.drag_seconds = seconds;
            }

            if (ImGui::IsItemDeactivatedAfterEdit() && app.drag_seconds >= 0.0f) {
                hdrg_gui_player_seek(app.player, static_cast<int64_t>(app.drag_seconds * 1e6f));
                app.drag_seconds = -1.0f;
            }
        }

        bool view_changed = ImGui::RadioButton("HDR (処理済み)", &app.view, HDRG_GUI_VIEW_OUTPUT);

        ImGui::SameLine();
        view_changed = ImGui::RadioButton("元の SDR", &app.view, HDRG_GUI_VIEW_SOURCE) || view_changed;
        ImGui::SameLine();
        view_changed = ImGui::RadioButton("並べる", &app.view, HDRG_GUI_VIEW_BOTH) || view_changed;

        if (view_changed) {
            hdrg_gui_player_view(app.player, app.white_level, static_cast<uint8_t>(app.view));
        }

        ImGui::Text("%u × %u%s", status.info.width, status.info.height,
                    (status.info.rate_num != 0) ? "" : " (静止画)");

        if (status.info.rate_num != 0) {
            ImGui::Text("%.3f fps、グレーディング %.1f ms", status.info.rate_num / static_cast<double>(status.info.rate_den),
                        status.apply_ms);
        }

        ImGui::Text("描画のピーク %.0f nits", status.peak_nits);

        if (status.error[0] != '\0') {
            ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "%s", status.error);
        }
    }

    ImGui::Text("ウィンドウ: %s、SDR の白 %.2f、ヘッドルーム %.2f 倍", app.hdr_swapchain ? "HDR (拡張リニア)" : "SDR",
                app.white_level, app.headroom);

    if (app.job != nullptr) {
        hdrg_gui_progress_t progress{};

        hdrg_gui_export_progress(app.job, &progress);

        if (progress.finished) {
            app.message = progress.failed ? std::string("書き出しに失敗: ") + progress.error : "書き出しました";
            hdrg_gui_export_finish(app.job);
            app.job = nullptr;
        } else {
            ImGui::ProgressBar((progress.total_us > 0) ? static_cast<float>(progress.done_us) / progress.total_us : 0.0f,
                               ImVec2(-1, 0), "書き出し中");
        }
    }

    if (!app.message.empty()) {
        ImGui::TextWrapped("%s", app.message.c_str());
    }

    ImGui::End();
}

// a label on a dark box at pos (ImGui's coordinates)
void Label(ImDrawList *draw, ImVec2 pos, const char *text)
{
    ImVec2 size = ImGui::CalcTextSize(text);

    draw->AddRectFilled(ImVec2(pos.x - 6.0f, pos.y - 3.0f), ImVec2(pos.x + size.x + 6.0f, pos.y + size.y + 3.0f),
                        IM_COL32(0, 0, 0, 170), 4.0f);
    draw->AddText(pos, IM_COL32(255, 255, 255, 255), text);
}

// the side-by-side view: its halves' labels and the line between them, behind the windows
void DrawLabels(const App &app)
{
    if (app.player == nullptr || !app.side_by_side) {
        return;
    }

    ImGuiIO &io = ImGui::GetIO();
    ImDrawList *draw = ImGui::GetBackgroundDrawList();
    float sx = (io.DisplayFramebufferScale.x > 0.0f) ? io.DisplayFramebufferScale.x : 1.0f;
    float sy = (io.DisplayFramebufferScale.y > 0.0f) ? io.DisplayFramebufferScale.y : 1.0f;
    float x = app.shown.x / sx, y = app.shown.y / sy, w = app.shown.w / sx, h = app.shown.h / sy;
    float middle = x + w * 0.5f;

    draw->AddLine(ImVec2(middle, y), ImVec2(middle, y + h), IM_COL32(255, 255, 255, 140), 1.0f);
    Label(draw, ImVec2(x + 12.0f, y + 10.0f), "SDR (元)");
    Label(draw, ImVec2(middle + 12.0f, y + 10.0f), "HDR (処理済み)");
}

// the picture fitted into the swapchain texture (letterboxed)
void Blit(App &app, SDL_GPUCommandBuffer *commands, SDL_GPUTexture *target, uint32_t width, uint32_t height)
{
    hdrg_gui_picture_t picture{};

    if (app.player == nullptr || !hdrg_gui_player_lock(app.player, &picture)) {
        return;
    }

    if (app.texture == nullptr || app.texture_width != picture.width || app.texture_height != picture.height) {
        SDL_GPUTextureCreateInfo info{};
        SDL_GPUTransferBufferCreateInfo transfer{};

        SDL_ReleaseGPUTexture(app.device, app.texture);
        SDL_ReleaseGPUTransferBuffer(app.device, app.transfer);
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
        info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = picture.width;
        info.height = picture.height;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        app.texture = SDL_CreateGPUTexture(app.device, &info);
        transfer.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        transfer.size = picture.width * picture.height * 8u;
        app.transfer = SDL_CreateGPUTransferBuffer(app.device, &transfer);
        app.texture_width = picture.width;
        app.texture_height = picture.height;
        app.uploaded = 0;
    }

    if (app.texture != nullptr && app.transfer != nullptr && picture.serial != app.uploaded) {
        void *mapped = SDL_MapGPUTransferBuffer(app.device, app.transfer, true);

        if (mapped != nullptr) {
            SDL_GPUCopyPass *copy;
            SDL_GPUTextureTransferInfo source{};
            SDL_GPUTextureRegion region{};

            std::memcpy(mapped, picture.pixels, static_cast<size_t>(picture.width) * picture.height * 8u);
            SDL_UnmapGPUTransferBuffer(app.device, app.transfer);
            copy = SDL_BeginGPUCopyPass(commands);
            source.transfer_buffer = app.transfer;
            region.texture = app.texture;
            region.w = picture.width;
            region.h = picture.height;
            region.d = 1;
            SDL_UploadToGPUTexture(copy, &source, &region, true);
            SDL_EndGPUCopyPass(copy);
            app.uploaded = picture.serial;
        }
    }

    hdrg_gui_player_unlock(app.player);

    if (app.texture != nullptr && app.uploaded != 0) {
        SDL_GPUBlitInfo blit{};
        float scale = std::fmin(static_cast<float>(width) / picture.width, static_cast<float>(height) / picture.height);
        uint32_t w = static_cast<uint32_t>(picture.width * scale), h = static_cast<uint32_t>(picture.height * scale);

        blit.source.texture = app.texture;
        blit.source.w = picture.width;
        blit.source.h = picture.height;
        blit.destination.texture = target;
        blit.destination.x = (width - w) / 2u;
        blit.destination.y = (height - h) / 2u;
        blit.destination.w = w;
        blit.destination.h = h;
        app.shown = SDL_FRect{static_cast<float>(blit.destination.x), static_cast<float>(blit.destination.y),
                              static_cast<float>(w), static_cast<float>(h)};
        app.side_by_side = picture.view == HDRG_GUI_VIEW_BOTH;
        blit.load_op = SDL_GPU_LOADOP_CLEAR;
        blit.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 1.0f};
        blit.filter = SDL_GPU_FILTER_LINEAR;
        SDL_BlitGPUTexture(commands, &blit);
    }
}

// without a window: --probe <file> grades its first frames into pictures, --export <in> <out>
// writes it; both print what they did (the checks of a build without a display)
int Headless(int argc, char **argv)
{
    hdrg_gui_settings_t settings;
    char error[HDRG_GUI_ERROR];

    hdrg_gui_settings_init(&settings, HDRG_PRESET_PHOTO);

    if (std::strcmp(argv[1], "--probe") == 0 && argc >= 3) {
        hdrg_gui_player_t *player = nullptr;
        hdrg_gui_status_t status{};
        hdrg_gui_picture_t picture{};
        uint64_t seen = 0;
        uint8_t view = HDRG_GUI_VIEW_OUTPUT;
        int pictures = 0;
        bool side_by_side_ok = true;

        for (int a = 3; a + 1 < argc; a++) {
            if (std::strcmp(argv[a], "--view") == 0) {
                view = (std::strcmp(argv[a + 1], "sdr") == 0)    ? HDRG_GUI_VIEW_SOURCE
                       : (std::strcmp(argv[a + 1], "both") == 0) ? HDRG_GUI_VIEW_BOTH
                                                                  : HDRG_GUI_VIEW_OUTPUT;
            }
        }

        if (!hdrg_gui_player_open(argv[2], &settings, &player, error, sizeof(error))) {
            std::fprintf(stderr, "open: %s\n", error);

            return 1;
        }

        hdrg_gui_player_view(player, 1.0f, view);
        hdrg_gui_player_settings(player, &settings);
        hdrg_gui_player_play(player, true);

        for (int i = 0; i < 1000 && pictures < 30; i++) {
            SDL_Delay(10);

            if (hdrg_gui_player_lock(player, &picture)) {
                if (picture.serial != seen) {
                    // the green's maximum in the left and the right half (side by side: SDR, HDR)
                    float max[2] = {0.0f, 0.0f};

                    for (size_t k = 0; k < static_cast<size_t>(picture.width) * picture.height; k++) {
                        uint16_t h = picture.pixels[k * 4u + 1u];
                        uint32_t e = (h >> 10) & 31u, m = h & 1023u;
                        float v = (e == 0) ? m / 16777216.0f : std::ldexp(1.0f + m / 1024.0f, static_cast<int>(e) - 15);
                        int half = (k % picture.width) >= picture.width / 2u;

                        max[half] = std::fmax(max[half], v);
                    }

                    hdrg_gui_player_status(player, &status);

                    if (view == HDRG_GUI_VIEW_BOTH &&
                        (picture.view != HDRG_GUI_VIEW_BOTH || picture.width != 2u * status.info.width ||
                         max[1] <= max[0])) {
                        side_by_side_ok = false;
                    }

                    seen = picture.serial;
                    pictures++;
                    std::printf("picture %d: %u x %u, pts %lld us, green max %.3f (left) / %.3f (right) x SDR white\n",
                                pictures, picture.width, picture.height, static_cast<long long>(picture.pts_us), max[0],
                                max[1]);
                }

                hdrg_gui_player_unlock(player);
            }

            hdrg_gui_player_status(player, &status);

            if (status.error[0] != '\0' || (status.ended && pictures > 0)) {
                break;
            }
        }

        hdrg_gui_player_status(player, &status);
        std::printf("%u x %u, %s, peak %.0f nits, grading %.1f ms%s%s\n", status.info.width, status.info.height,
                    (status.info.rate_num != 0) ? "video" : "still", status.peak_nits, status.apply_ms,
                    (status.error[0] != '\0') ? ", error: " : "", status.error);
        hdrg_gui_player_close(player);

        if (!side_by_side_ok) {
            std::fprintf(stderr, "the side-by-side pictures are not the SDR left of the brighter HDR\n");
        }

        return (pictures > 0 && status.error[0] == '\0' && side_by_side_ok) ? 0 : 1;
    }

    if (std::strcmp(argv[1], "--export") == 0 && argc >= 4) {
        hdrg_gui_export_t *job = nullptr;
        hdrg_gui_progress_t progress{};

        // a still's writer by its id (else the output's extension's) and its options
        for (int i = 4; i + 1 < argc; i += 2) {
            if (std::strcmp(argv[i], "--format") == 0) {
                std::snprintf(settings.format, sizeof(settings.format), "%s", argv[i + 1]);
            } else if (std::strcmp(argv[i], "--format-opt") == 0 && settings.format_option_count < HDRG_GUI_FORMAT_OPTIONS) {
                std::snprintf(settings.format_options[settings.format_option_count++], HDRG_GUI_FORMAT_OPTION, "%s", argv[i + 1]);
            }
        }

        if (!hdrg_gui_export_start(argv[2], argv[3], &settings, &job, error, sizeof(error))) {
            std::fprintf(stderr, "export: %s\n", error);

            return 1;
        }

        do {
            SDL_Delay(20);
            hdrg_gui_export_progress(job, &progress);
        } while (!progress.finished);

        hdrg_gui_export_finish(job);
        std::printf("export: %s%s\n", progress.failed ? "failed: " : "done", progress.error);

        return progress.failed ? 1 : 0;
    }

    std::fprintf(stderr, "usage: hdrg-gui [file] | --probe <file> [--view hdr|sdr|both] | --export <in> <out> [--format <id>] [--format-opt <key>=<value>]...\n");

    return 2;
}

}  // namespace

int main(int argc, char **argv)
{
    App app;
    bool quit = false;

    if (argc > 1 && argv[1][0] == '-' && argv[1][1] == '-') {
        int code;

        SDL_Init(0);
        code = Headless(argc, argv);
        SDL_Quit();

        return code;
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());

        return 1;
    }

    app.window = SDL_CreateWindow("hdrg-gui", 1440, 900, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    app.device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL |
                                         SDL_GPU_SHADERFORMAT_METALLIB,
                                     false, nullptr);

    if (app.window == nullptr || app.device == nullptr || !SDL_ClaimWindowForGPUDevice(app.device, app.window)) {
        std::fprintf(stderr, "SDL: %s\n", SDL_GetError());

        return 1;
    }

    if (SDL_WindowSupportsGPUSwapchainComposition(app.device, app.window,
                                                  SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR)) {
        app.hdr_swapchain = SDL_SetGPUSwapchainParameters(app.device, app.window,
                                                          SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR,
                                                          SDL_GPU_PRESENTMODE_VSYNC);
    }

    app.chosen_lock = SDL_CreateMutex();
    hdrg_gui_settings_init(&app.settings, HDRG_PRESET_PHOTO);
    ListWriters(app);
    ReadDisplay(app);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    if (app.hdr_swapchain) {
        ToLinearStyle();
    }

    {
        ImGuiIO &io = ImGui::GetIO();
        const char *fonts[] = {"/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc", "C:\\Windows\\Fonts\\meiryo.ttc",
                               "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"};

        for (const char *font : fonts) {
            if (SDL_GetPathInfo(font, nullptr)) {
                io.Fonts->AddFontFromFileTTF(font, 16.0f, nullptr, io.Fonts->GetGlyphRangesJapanese());

                break;
            }
        }
    }

    ImGui_ImplSDL3_InitForSDLGPU(app.window);
    {
        ImGui_ImplSDLGPU3_InitInfo init{};

        init.Device = app.device;
        init.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(app.device, app.window);
        init.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
        ImGui_ImplSDLGPU3_Init(&init);
    }

    if (argc > 1) {
        Open(app, argv[1]);
    }

    while (!quit) {
        SDL_Event event;

        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);

            if (event.type == SDL_EVENT_QUIT ||
                (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(app.window))) {
                quit = true;
            } else if (event.type == SDL_EVENT_WINDOW_HDR_STATE_CHANGED) {
                ReadDisplay(app);
                Apply(app);

                if (app.player != nullptr) {
                    hdrg_gui_player_view(app.player, app.white_level, static_cast<uint8_t>(app.view));
                }
            } else if (event.type == SDL_EVENT_DROP_FILE && event.drop.data != nullptr) {
                Open(app, event.drop.data);
            }
        }

        {
            std::string open, save;

            SDL_LockMutex(app.chosen_lock);
            open.swap(app.chosen_open);
            save.swap(app.chosen_save);
            SDL_UnlockMutex(app.chosen_lock);

            if (!open.empty()) {
                Open(app, open);
            }

            if (!save.empty() && app.job == nullptr) {
                char error[HDRG_GUI_ERROR];

                if (!hdrg_gui_export_start(app.path.c_str(), save.c_str(), &app.settings, &app.job, error, sizeof(error))) {
                    app.message = std::string("書き出せません: ") + error;
                }
            }
        }

        ImGui_ImplSDLGPU3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        SettingsWindow(app);
        PlayerWindow(app);
        DrawLabels(app);
        ImGui::Render();

        SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(app.device);
        SDL_GPUTexture *swapchain = nullptr;
        uint32_t width = 0, height = 0;

        if (commands == nullptr) {
            continue;
        }

        if (!SDL_WaitAndAcquireGPUSwapchainTexture(commands, app.window, &swapchain, &width, &height) ||
            swapchain == nullptr) {
            SDL_SubmitGPUCommandBuffer(commands);

            continue;
        }

        ImDrawData *draw = ImGui::GetDrawData();
        SDL_GPUColorTargetInfo target{};

        Blit(app, commands, swapchain, width, height);
        ImGui_ImplSDLGPU3_PrepareDrawData(draw, commands);
        target.texture = swapchain;
        target.load_op = (app.uploaded != 0) ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR;
        target.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 1.0f};
        target.store_op = SDL_GPU_STOREOP_STORE;

        SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(commands, &target, 1, nullptr);

        ImGui_ImplSDLGPU3_RenderDrawData(draw, commands, pass);
        SDL_EndGPURenderPass(pass);
        SDL_SubmitGPUCommandBuffer(commands);
    }

    if (app.job != nullptr) {
        hdrg_gui_export_finish(app.job);
    }

    hdrg_gui_player_close(app.player);
    SDL_WaitForGPUIdle(app.device);
    ImGui_ImplSDL3_Shutdown();
    ImGui_ImplSDLGPU3_Shutdown();
    ImGui::DestroyContext();
    SDL_ReleaseGPUTexture(app.device, app.texture);
    SDL_ReleaseGPUTransferBuffer(app.device, app.transfer);
    SDL_ReleaseWindowFromGPUDevice(app.device, app.window);
    SDL_DestroyGPUDevice(app.device);
    SDL_DestroyWindow(app.window);
    SDL_DestroyMutex(app.chosen_lock);
    SDL_Quit();

    return 0;
}
