/*
 * qhv_codec_v5.c - Quantum Hybrid Video v5
 * -----------------------------------------
 * Unified direct transcode tool with checkpoint/resume.
 *
 * Modes:
 *   encode <input_video> <output.mp4> [crf] [-noaudio]
 *   decode <input_video> <output.mp4> [crf] [-noaudio]
 *
 * The current pipeline is segment-based:
 *   - each segment is encoded to a checkpoint directory
 *   - manifest is updated after every successful segment
 *   - interruption can resume from the next unfinished segment
 *   - final MP4 is concatenated from completed segments
 *
 * Audio is copied from the input when present unless -noaudio is set.
 * The legacy .qhv container path has been removed.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#endif

typedef struct {
    char input[1024];
    char output[1024];
    int crf;
    int copy_audio;
    int w;
    int h;
    double fps;
    double duration;
    double segment_sec;
    int total_segments;
    int next_segment;
    long long input_size;
    long long input_mtime;
} QhvCheckpoint;

static int parse_fraction(const char *s, double *out) {
    int num = 0, den = 0;
    if (sscanf(s, "%d/%d", &num, &den) == 2 && num > 0 && den > 0) {
        *out = (double)num / (double)den;
        return 1;
    }
    char *end = NULL;
    double v = strtod(s, &end);
    if (end != s && v > 0.0) {
        *out = v;
        return 1;
    }
    return 0;
}

static int file_exists(const char *path) {
    struct stat st;
    return path && path[0] && stat(path, &st) == 0;
}

static int stat_signature(const char *path, long long *size_out, long long *mtime_out) {
    struct stat st;
    if (!path || !path[0]) return 0;
    if (stat(path, &st) != 0) return 0;
    if (size_out) *size_out = (long long)st.st_size;
    if (mtime_out) *mtime_out = (long long)st.st_mtime;
    return 1;
}

static int build_temp_path(char *dst, size_t dst_sz, const char *out_path) {
    int n = snprintf(dst, dst_sz, "%s.part", out_path);
    return (n > 0 && (size_t)n < dst_sz);
}

static int publish_output(const char *temp_path, const char *out_path) {
#ifdef _WIN32
    if (!MoveFileExA(temp_path, out_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return 0;
    }
    return 1;
#else
    return rename(temp_path, out_path) == 0;
#endif
}

static void cleanup_temp_file(const char *path) {
    if (!path || !path[0]) return;
#ifdef _WIN32
    DeleteFileA(path);
#else
    remove(path);
#endif
}

static int ensure_dir(const char *path) {
#ifdef _WIN32
    if (_mkdir(path) == 0) return 1;
#else
    if (mkdir(path, 0755) == 0) return 1;
#endif
    return file_exists(path);
}

static int build_checkpoint_dir(char *dst, size_t dst_sz, const char *out_path) {
    int n = snprintf(dst, dst_sz, "%s.qhv_resume", out_path);
    return (n > 0 && (size_t)n < dst_sz);
}

static int build_path_in_dir(char *dst, size_t dst_sz, const char *dir, const char *name) {
    int n = snprintf(dst, dst_sz, "%s\\%s", dir, name);
    return (n > 0 && (size_t)n < dst_sz);
}

static int build_segment_path(char *dst, size_t dst_sz, const char *dir, int seg_idx, int is_tmp) {
    char name[64];
    if (is_tmp) {
        snprintf(name, sizeof(name), "seg_%05d.mp4.part", seg_idx);
    } else {
        snprintf(name, sizeof(name), "seg_%05d.mp4", seg_idx);
    }
    return build_path_in_dir(dst, dst_sz, dir, name);
}

static int build_manifest_path(char *dst, size_t dst_sz, const char *dir) {
    return build_path_in_dir(dst, dst_sz, dir, "manifest.txt");
}

static int build_manifest_tmp_path(char *dst, size_t dst_sz, const char *dir) {
    return build_path_in_dir(dst, dst_sz, dir, "manifest.tmp");
}

static int build_concat_path(char *dst, size_t dst_sz, const char *dir) {
    return build_path_in_dir(dst, dst_sz, dir, "concat.txt");
}

static int build_absolute_path(char *dst, size_t dst_sz, const char *path) {
#ifdef _WIN32
    char *res = _fullpath(dst, path, (int)dst_sz);
    return res != NULL;
#else
    return realpath(path, dst) != NULL;
#endif
}

static int probe_size(const char *path, int *w, int *h) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=s=x:p=0 \"%s\"",
             path);
    FILE *fp = popen(cmd, "r");
    if (!fp) return 0;
    int ok = fscanf(fp, "%dx%d", w, h) == 2;
    pclose(fp);
    return ok && *w > 0 && *h > 0;
}

static double probe_fps(const char *path) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "ffprobe -v error -select_streams v:0 -show_entries stream=avg_frame_rate -of csv=p=0 \"%s\"",
             path);
    FILE *fp = popen(cmd, "r");
    if (!fp) return 30.0;

    char rate[64] = {0};
    if (!fgets(rate, sizeof(rate), fp)) {
        pclose(fp);
        return 30.0;
    }
    pclose(fp);

    double fps = 0.0;
    if (parse_fraction(rate, &fps)) return fps;
    return 30.0;
}

static double probe_duration(const char *path) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "ffprobe -v error -show_entries format=duration -of csv=p=0 \"%s\"",
             path);
    FILE *fp = popen(cmd, "r");
    if (!fp) return 0.0;

    char buf[128] = {0};
    if (!fgets(buf, sizeof(buf), fp)) {
        pclose(fp);
        return 0.0;
    }
    pclose(fp);

    char *end = NULL;
    double dur = strtod(buf, &end);
    if (end == buf || dur <= 0.0) return 0.0;
    return dur;
}

static long long probe_file_size_bytes(const char *path) {
    struct stat st;
    if (!path || !path[0]) return 0;
    if (stat(path, &st) != 0) return 0;
    return (long long)st.st_size;
}

static long long probe_nb_frames(const char *path) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "ffprobe -v error -select_streams v:0 -show_entries stream=nb_frames -of csv=p=0 \"%s\"",
             path);
    FILE *fp = popen(cmd, "r");
    if (!fp) return 0;

    char buf[64] = {0};
    if (!fgets(buf, sizeof(buf), fp)) {
        pclose(fp);
        return 0;
    }
    pclose(fp);

    char *end = NULL;
    long long frames = strtoll(buf, &end, 10);
    if (end == buf || frames <= 0) return 0;
    return frames;
}

static double choose_segment_sec(double duration, double fps, long long frames, long long input_bytes) {
    /*
     * Keep the number of checkpoints bounded without making short clips
     * overly fragmented.
     *
     * We combine:
     *   - duration: keep total segment count near a practical default
     *   - frame count: avoid segments that are too tiny or too large in frame terms
     *   - input size: denser checkpoints for larger/heavier inputs
     */
    double target_segments_by_duration = duration / 8.0;
    double target_segments_by_frames = 0.0;
    double target_segments_by_size = 0.0;

    if (frames > 0) {
        double frames_per_segment = 180.0;
        if (fps > 0.0) {
            if (fps <= 24.0) frames_per_segment = 144.0;
            else if (fps >= 60.0) frames_per_segment = 360.0;
            else frames_per_segment = 180.0;
        }
        target_segments_by_frames = (double)frames / frames_per_segment;
    }

    if (input_bytes > 0) {
        /*
         * Rough heuristic: ~256 MiB per segment means longer/heavier
         * sources get more checkpoints, but we avoid over-fragmenting.
         */
        target_segments_by_size = (double)input_bytes / (256.0 * 1024.0 * 1024.0);
    }

    double target_segments = target_segments_by_duration;
    if (target_segments_by_frames > target_segments) target_segments = target_segments_by_frames;
    if (target_segments_by_size > target_segments) target_segments = target_segments_by_size;
    if (target_segments < 1.0) target_segments = 1.0;
    if (target_segments > 64.0) target_segments = 64.0;

    double seg = duration / target_segments;
    if (seg < 2.0) seg = 2.0;
    if (seg > 15.0) seg = 15.0;
    return seg;
}

static int run_command(const char *cmd) {
    FILE *fp = popen(cmd, "r");
    if (!fp) return 0;
    int rc = pclose(fp);
    return rc == 0;
}

static int read_manifest(const char *manifest_path, QhvCheckpoint *ck) {
    FILE *fp = fopen(manifest_path, "rb");
    if (!fp) return 0;

    char line[2048];
    int magic_ok = 0;
    while (fgets(line, sizeof(line), fp)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        if (strncmp(line, "magic=", 6) == 0) {
            if (strcmp(line + 6, "QHVCHK1") == 0) magic_ok = 1;
        } else if (strncmp(line, "input=", 6) == 0) {
            snprintf(ck->input, sizeof(ck->input), "%s", line + 6);
        } else if (strncmp(line, "output=", 7) == 0) {
            snprintf(ck->output, sizeof(ck->output), "%s", line + 7);
        } else if (strncmp(line, "crf=", 4) == 0) {
            ck->crf = atoi(line + 4);
        } else if (strncmp(line, "copy_audio=", 11) == 0) {
            ck->copy_audio = atoi(line + 11);
        } else if (strncmp(line, "w=", 2) == 0) {
            ck->w = atoi(line + 2);
        } else if (strncmp(line, "h=", 2) == 0) {
            ck->h = atoi(line + 2);
        } else if (strncmp(line, "fps=", 4) == 0) {
            ck->fps = strtod(line + 4, NULL);
        } else if (strncmp(line, "duration=", 9) == 0) {
            ck->duration = strtod(line + 9, NULL);
        } else if (strncmp(line, "segment_sec=", 12) == 0) {
            ck->segment_sec = strtod(line + 12, NULL);
        } else if (strncmp(line, "total_segments=", 15) == 0) {
            ck->total_segments = atoi(line + 15);
        } else if (strncmp(line, "next_segment=", 13) == 0) {
            ck->next_segment = atoi(line + 13);
        } else if (strncmp(line, "input_size=", 11) == 0) {
            ck->input_size = strtoll(line + 11, NULL, 10);
        } else if (strncmp(line, "input_mtime=", 12) == 0) {
            ck->input_mtime = strtoll(line + 12, NULL, 10);
        }
    }

    fclose(fp);
    return magic_ok && ck->input[0] != '\0' && ck->output[0] != '\0' && ck->total_segments > 0;
}

static int write_manifest(const char *manifest_path, const QhvCheckpoint *ck) {
    char tmp_path[2048];
    int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", manifest_path);
    if (n <= 0 || (size_t)n >= sizeof(tmp_path)) return 0;

    FILE *fp = fopen(tmp_path, "wb");
    if (!fp) return 0;

    fprintf(fp, "magic=QHVCHK1\n");
    fprintf(fp, "input=%s\n", ck->input);
    fprintf(fp, "output=%s\n", ck->output);
    fprintf(fp, "crf=%d\n", ck->crf);
    fprintf(fp, "copy_audio=%d\n", ck->copy_audio);
    fprintf(fp, "w=%d\n", ck->w);
    fprintf(fp, "h=%d\n", ck->h);
    fprintf(fp, "fps=%.6f\n", ck->fps);
    fprintf(fp, "duration=%.6f\n", ck->duration);
    fprintf(fp, "segment_sec=%.6f\n", ck->segment_sec);
    fprintf(fp, "total_segments=%d\n", ck->total_segments);
    fprintf(fp, "next_segment=%d\n", ck->next_segment);
    fprintf(fp, "input_size=%lld\n", ck->input_size);
    fprintf(fp, "input_mtime=%lld\n", ck->input_mtime);
    fclose(fp);

    if (!publish_output(tmp_path, manifest_path)) {
        cleanup_temp_file(tmp_path);
        return 0;
    }
    return 1;
}

static int remove_checkpoint_tree(const char *ck_dir, int total_segments) {
    char path[2048];
    char tmp[2048];

    if (build_manifest_path(path, sizeof(path), ck_dir)) cleanup_temp_file(path);
    if (build_manifest_tmp_path(tmp, sizeof(tmp), ck_dir)) cleanup_temp_file(tmp);
    if (build_concat_path(path, sizeof(path), ck_dir)) cleanup_temp_file(path);

    for (int i = 0; i < total_segments; i++) {
        if (build_segment_path(path, sizeof(path), ck_dir, i, 0)) cleanup_temp_file(path);
        if (build_segment_path(tmp, sizeof(tmp), ck_dir, i, 1)) cleanup_temp_file(tmp);
    }

#ifdef _WIN32
    _rmdir(ck_dir);
#else
    rmdir(ck_dir);
#endif
    return 1;
}

static int resume_or_init_checkpoint(QhvCheckpoint *ck, const char *in_path, const char *out_path,
                                     int crf, int copy_audio, int w, int h,
                                     double fps, double duration, double segment_sec,
                                     const char *ck_dir, const char *manifest_path) {
    long long input_size = 0;
    long long input_mtime = 0;
    if (!stat_signature(in_path, &input_size, &input_mtime)) {
        return 0;
    }

    if (file_exists(manifest_path)) {
        QhvCheckpoint loaded = {0};
        if (!read_manifest(manifest_path, &loaded)) {
            fprintf(stderr, "qhv_codec_v5: checkpoint exists but manifest is invalid\n");
            return 0;
        }
        if (strcmp(loaded.input, in_path) != 0 ||
            strcmp(loaded.output, out_path) != 0 ||
            loaded.crf != crf ||
            loaded.copy_audio != copy_audio ||
            loaded.w != w ||
            loaded.h != h ||
            fabs(loaded.fps - fps) > 0.001 ||
            fabs(loaded.duration - duration) > 0.01 ||
            fabs(loaded.segment_sec - segment_sec) > 0.001 ||
            loaded.input_size != input_size ||
            loaded.input_mtime != input_mtime ||
            loaded.total_segments <= 0) {
            fprintf(stderr, "qhv_codec_v5: checkpoint does not match current job\n");
            return 0;
        }
        *ck = loaded;
        return 1;
    }

    if (!ensure_dir(ck_dir)) {
        fprintf(stderr, "qhv_codec_v5: failed to create checkpoint directory\n");
        return 0;
    }

    memset(ck, 0, sizeof(*ck));
    snprintf(ck->input, sizeof(ck->input), "%s", in_path);
    snprintf(ck->output, sizeof(ck->output), "%s", out_path);
    ck->crf = crf;
    ck->copy_audio = copy_audio;
    ck->w = w;
    ck->h = h;
    ck->fps = fps;
    ck->duration = duration;
    ck->segment_sec = segment_sec;
    ck->total_segments = (int)ceil(duration / segment_sec);
    if (ck->total_segments < 1) ck->total_segments = 1;
    ck->next_segment = 0;
    ck->input_size = input_size;
    ck->input_mtime = input_mtime;

    return write_manifest(manifest_path, ck);
}

static int encode_segment(const QhvCheckpoint *ck, int seg_idx, const char *seg_tmp_path) {
    double start = (double)seg_idx * ck->segment_sec;
    double dur = ck->segment_sec;
    if (start + dur > ck->duration) {
        dur = ck->duration - start;
    }
    if (dur <= 0.0) {
        dur = ck->segment_sec;
    }

    char cmd[2048];
    if (ck->copy_audio) {
        snprintf(cmd, sizeof(cmd),
                 "ffmpeg -ss %.6f -i \"%s\" -t %.6f -map 0:v:0 -map 0:a? "
                 "-c:v libx264 -preset veryfast -crf %d -c:a copy -pix_fmt yuv420p "
                 "-f mp4 -movflags +faststart -shortest \"%s\" -y",
                 start, ck->input, dur, ck->crf, seg_tmp_path);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "ffmpeg -ss %.6f -i \"%s\" -t %.6f -map 0:v:0 "
                 "-c:v libx264 -preset veryfast -crf %d -pix_fmt yuv420p "
                 "-f mp4 -movflags +faststart \"%s\" -y",
                 start, ck->input, dur, ck->crf, seg_tmp_path);
    }

    return run_command(cmd);
}

static int build_concat_file(const QhvCheckpoint *ck, const char *ck_dir, const char *concat_path) {
    FILE *fp = fopen(concat_path, "wb");
    if (!fp) return 0;

    for (int i = 0; i < ck->total_segments; i++) {
        char seg_path[2048];
        char abs_path[2048];
        if (!build_segment_path(seg_path, sizeof(seg_path), ck_dir, i, 0)) {
            fclose(fp);
            return 0;
        }
        if (!build_absolute_path(abs_path, sizeof(abs_path), seg_path)) {
            snprintf(abs_path, sizeof(abs_path), "%s", seg_path);
        }
        fprintf(fp, "file '%s'\n", abs_path);
    }

    fclose(fp);
    return 1;
}

static int all_segments_present(const QhvCheckpoint *ck, const char *ck_dir) {
    char seg_path[2048];
    for (int i = 0; i < ck->total_segments; i++) {
        if (!build_segment_path(seg_path, sizeof(seg_path), ck_dir, i, 0)) return 0;
        if (!file_exists(seg_path)) return 0;
    }
    return 1;
}

static int finalize_segments(const QhvCheckpoint *ck, const char *ck_dir, const char *temp_out_path) {
    char concat_path[2048];
    if (!build_concat_path(concat_path, sizeof(concat_path), ck_dir)) return 0;
    if (!build_concat_file(ck, ck_dir, concat_path)) return 0;

    char cmd[3072];
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -f concat -safe 0 -i \"%s\" -c copy -f mp4 -movflags +faststart \"%s\" -y",
             concat_path, temp_out_path);
    return run_command(cmd);
}

static int transcode_video(const char *mode, const char *in_path, const char *out_path,
                           int crf, int copy_audio) {
    int w = 0, h = 0;
    if (!probe_size(in_path, &w, &h)) {
        fprintf(stderr, "qhv_codec_v5: unable to probe input size\n");
        return 1;
    }

    double fps = probe_fps(in_path);
    if (fps <= 0.0) fps = 30.0;

    double duration = probe_duration(in_path);
    if (duration <= 0.0) {
        fprintf(stderr, "qhv_codec_v5: unable to probe input duration\n");
        return 1;
    }

    long long input_bytes = probe_file_size_bytes(in_path);
    long long nb_frames = probe_nb_frames(in_path);
    if (nb_frames <= 0 && fps > 0.0) {
        nb_frames = (long long)(duration * fps + 0.5);
    }

    double segment_sec = choose_segment_sec(duration, fps, nb_frames, input_bytes);

    char ck_dir[1024];
    char manifest_path[2048];
    char temp_path[1024];
    if (!build_checkpoint_dir(ck_dir, sizeof(ck_dir), out_path) ||
        !build_manifest_path(manifest_path, sizeof(manifest_path), ck_dir) ||
        !build_temp_path(temp_path, sizeof(temp_path), out_path)) {
        fprintf(stderr, "qhv_codec_v5: path too long\n");
        return 1;
    }

    QhvCheckpoint ck = {0};
    if (!resume_or_init_checkpoint(&ck, in_path, out_path, crf, copy_audio,
                                   w, h, fps, duration, segment_sec, ck_dir, manifest_path)) {
        return 1;
    }

    if (ck.next_segment < 0) ck.next_segment = 0;
    if (ck.next_segment > ck.total_segments) ck.next_segment = ck.total_segments;

    fprintf(stdout,
            "QHV v5 %s: %dx%d @ %.3f fps, duration=%.3fs, frames=%lld, input=%lld bytes, segments=%d -> %s (crf=%d, audio=%s)\n",
            mode, ck.w, ck.h, ck.fps, ck.duration, nb_frames, input_bytes, ck.total_segments,
            out_path, ck.crf, ck.copy_audio ? "copy" : "off");
    fprintf(stdout, "qhv_codec_v5: segment size=%.3fs\n", ck.segment_sec);
    if (ck.next_segment > 0) {
        fprintf(stdout, "qhv_codec_v5: resuming from segment %d/%d\n",
                ck.next_segment + 1, ck.total_segments);
    }

    for (int seg = ck.next_segment; seg < ck.total_segments; seg++) {
        char seg_path[2048];
        char seg_tmp_path[2048];
        if (!build_segment_path(seg_path, sizeof(seg_path), ck_dir, seg, 0) ||
            !build_segment_path(seg_tmp_path, sizeof(seg_tmp_path), ck_dir, seg, 1)) {
            fprintf(stderr, "qhv_codec_v5: segment path too long\n");
            return 1;
        }

        if (file_exists(seg_path)) {
            fprintf(stdout, "qhv_codec_v5: segment %d/%d already done, skipping\n",
                    seg + 1, ck.total_segments);
            ck.next_segment = seg + 1;
            if (!write_manifest(manifest_path, &ck)) {
                fprintf(stderr, "qhv_codec_v5: failed updating checkpoint manifest\n");
                return 1;
            }
            continue;
        }

        cleanup_temp_file(seg_tmp_path);

        double start = (double)seg * ck.segment_sec;
        double dur = ck.segment_sec;
        if (start + dur > ck.duration) {
            dur = ck.duration - start;
        }
        if (dur <= 0.0) {
            dur = ck.segment_sec;
        }

        fprintf(stdout, "qhv_codec_v5: segment %d/%d (%.3fs -> %.3fs)\n",
                seg + 1, ck.total_segments, start, start + dur);

        if (!encode_segment(&ck, seg, seg_tmp_path)) {
            fprintf(stderr, "qhv_codec_v5: failed encoding segment %d\n", seg);
            cleanup_temp_file(seg_tmp_path);
            return 1;
        }
        if (!publish_output(seg_tmp_path, seg_path)) {
            fprintf(stderr, "qhv_codec_v5: failed publishing segment %d\n", seg);
            cleanup_temp_file(seg_tmp_path);
            return 1;
        }

        ck.next_segment = seg + 1;
        if (!write_manifest(manifest_path, &ck)) {
            fprintf(stderr, "qhv_codec_v5: failed updating checkpoint manifest\n");
            return 1;
        }
    }

    if (!all_segments_present(&ck, ck_dir)) {
        fprintf(stderr, "qhv_codec_v5: checkpoint incomplete, cannot finalize\n");
        return 1;
    }

    if (!finalize_segments(&ck, ck_dir, temp_path)) {
        fprintf(stderr, "qhv_codec_v5: failed to concatenate final output\n");
        cleanup_temp_file(temp_path);
        return 1;
    }

    if (!publish_output(temp_path, out_path)) {
        fprintf(stderr, "qhv_codec_v5: failed to publish output\n");
        cleanup_temp_file(temp_path);
        return 1;
    }

    remove_checkpoint_tree(ck_dir, ck.total_segments);

    fprintf(stdout, "QHV v5 %s complete: segments=%d output=%s\n",
            mode, ck.total_segments, out_path);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr,
                "usage:\n"
                "  %s encode <input_video> <output.mp4> [crf] [-noaudio]\n"
                "  %s decode <input_video> <output.mp4> [crf] [-noaudio]\n"
                "  checkpoint/resume is automatic via <output>.qhv_resume\n"
                "  segment size is auto-chosen from input duration\n",
                argv[0], argv[0]);
        return 1;
    }

    const char *mode = argv[1];
    if (strcmp(mode, "encode") != 0 && strcmp(mode, "decode") != 0) {
        fprintf(stderr, "qhv_codec_v5: unknown mode '%s' (use encode or decode)\n", mode);
        return 1;
    }

    const char *in_path = argv[2];
    const char *out_path = argv[3];
    int crf = 23;
    int copy_audio = 1;
    if (argc >= 5) {
        for (int i = 4; i < argc; i++) {
            if (strcmp(argv[i], "-noaudio") == 0) {
                copy_audio = 0;
            } else {
                crf = atoi(argv[i]);
                if (crf < 0) crf = 0;
                if (crf > 51) crf = 51;
            }
        }
    }

    return transcode_video(mode, in_path, out_path, crf, copy_audio);
}
