#pragma once

#include <vector>
#include "benchmark.h"
#include "rfb/LogWriter.h"
#include "rfb/ffmpeg.h"

class FfmpegFrameFeeder final {
    rfb::LogWriter vlog{"FFmpeg"};
    FFmpeg *ffmpeg{};

    FFmpeg::ContextGuard codec_ctx_guard{};
    FFmpeg::FormatCtxGuard format_ctx_guard{};
    int video_stream_idx{-1};

public:
    explicit FfmpegFrameFeeder(FFmpeg *ffmpeg);

    ~FfmpegFrameFeeder();

    void open(std::string_view path);

    [[nodiscard]] int64_t get_total_frame_count() const { return format_ctx_guard->streams[video_stream_idx]->nb_frames; }

    struct frame_dimensions_t
    {
        int width{};
        int height{};
    };

    [[nodiscard]] frame_dimensions_t get_frame_dimensions() const { return {codec_ctx_guard->width, codec_ctx_guard->height}; }

    struct play_stats_t
    {
        uint64_t frames{};
        uint64_t total{};
        std::vector<uint64_t> timings;
    };

    play_stats_t play(benchmarking::MockTestConnection *connection) const;
};
