#include <locale.h>

#ifdef HAVE_ALLOCA_H
#include <alloca.h>
#else
#include <stdlib.h>
#endif

#include <fcntl.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.1415926535897932385
#endif

#include <stdio.h>
#include <stdlib.h>

#ifndef _WIN32
#include <ctype.h>
#include <dirent.h>
#include <getopt.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

#ifdef _WIN32
#include "input/winscap.h"
#include <windows.h>
#define PATH_MAX 260
#define PACKAGE "cava"
#define VERSION "1.0.0"
#define _CRT_SECURE_NO_WARNINGS 1
#endif // _WIN32

#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#include "cavacore.h"
#include "config.h"
#include "util.h"

// Wymuszenie tylko nagłówka SDL_GLSL
#include "output/sdl_glsl.h"

#include "input/common.h"

#ifndef _WIN32
// Zostawiamy tylko nowoczesny Pipewire dla Linuxa
#include "input/pipewire.h"
#endif

#ifdef __GNUC__
#undef GCC_UNUSED
#define GCC_UNUSED __attribute__((unused))
#else
#define GCC_UNUSED /* nothing */
#endif

struct cava_buffers {
  int *bars;
  int *previous_frame;
  int *right_bars;
  int *right_previous_frame;
  float *bars_left;
  float *bars_right;
  double *cava_out;
  float *bars_raw;
  float *previous_bars_raw;
};

struct terminal_dimensions {
  int width;
  int height;
  int *dim_bar;
  int *dim_val;
};

#ifdef _WIN32
char *optarg = NULL;
int optind = 1;
int optopt = 0;

static int getopt(int argc, char *const argv[], const char *optstring) {
  if ((optind >= argc) || (argv[optind][0] != '-') || (argv[optind][0] == 0)) {
      return -1;
    }

  if (argv[optind][0] == '-' && argv[optind][1] == '\0') {
      return -1;
    }

  if (strcmp(argv[optind], "--") == 0) {
      optind++;
      return -1;
    }

  if (argv[optind][0] == '-' && argv[optind][1] == '-' && argv[optind][2] != '\0') {
      const char *longopt = argv[optind] + 2;
      if (strcmp(longopt, "help") == 0) {
          optopt = 'h';
          optind++;
          return 'h';
        }
      if (strcmp(longopt, "version") == 0) {
          optopt = 'v';
          optind++;
          return 'v';
        }
      if (strcmp(longopt, "config") == 0) {
          optopt = 'p';
          optind++;
          if (optind >= argc) {
              return ':';
            }
          optarg = argv[optind];
          optind++;
          return 'p';
        }
      optopt = 0;
      optind++;
      return '?';
    }

  int opt = argv[optind][1];
  const char *opt_position = strchr(optstring, opt);

  if (opt_position == NULL) {
      optopt = opt;
      optind++;
      return '?';
    }
  if (opt_position[1] == ':') {
      optopt = opt;
      optind++;
      if (optind >= argc) {
          return ':';
        }
      optarg = argv[optind];
      optind++;
      return opt;
    }

  optopt = opt;
  optind++;
  return opt;
}
#endif

// czy powinnismy przeladowac config
volatile sig_atomic_t should_reload = 0;
// czy powinnismy przeladowac kolory
volatile sig_atomic_t reload_colors = 0;
// czy powinnismy wyjsc
volatile sig_atomic_t should_quit = 0;
volatile sig_atomic_t signal_received = 0;

static bool get_file_state(const char *path, time_t *mtime, long long *size) {
#ifdef _WIN32
  struct _stat st;
  if (_stat(path, &st) != 0)
    return false;
#else
  struct stat st;
  if (stat(path, &st) != 0)
    return false;
#endif
  *mtime = st.st_mtime;
  *size = (long long)st.st_size;
  return true;
}

struct config_params p = {0};

// general: cleanup
void cleanup(void) {
  cleanup_sdl_glsl();
}

// general: handle signals
#ifdef _WIN32
int sig_handler(DWORD sig_no) {
#else
void sig_handler(int sig_no) {
#endif
#ifndef _WIN32

  if (sig_no == SIGUSR1) {
      should_reload = 1;
      return;
    }

  if (sig_no == SIGUSR2) {
      reload_colors = 1;
      return;
    }
#endif

#ifdef _WIN32
  if (sig_no == CTRL_C_EVENT || sig_no == CTRL_CLOSE_EVENT) {
      sig_no = SIGINT;
    } else {
      return TRUE;
    }
#endif
  if (sig_no == SIGINT || sig_no == SIGTERM) {
      should_reload = 1;
      should_quit = 1;
    }

  signal_received = sig_no;

#ifdef _WIN32
  return TRUE;
#endif
}

float *monstercat_filter(float *bars, int number_of_bars, int waves, double monstercat,
                   int height) {
  int z;
  int m_y, de;
  float height_normalizer = 1.0;
  if (height > 1000) {
      height_normalizer = height / 912.76;
    }
  if (waves > 0) {
      for (z = 0; z < number_of_bars; z++) {
          bars[z] = bars[z] / 1.25;
          for (m_y = z - 1; m_y >= 0; m_y--) {
              de = z - m_y;
              bars[m_y] = max(bars[z] - height_normalizer * pow(de, 2), bars[m_y]);
            }
          for (m_y = z + 1; m_y < number_of_bars; m_y++) {
              de = m_y - z;
              bars[m_y] = max(bars[z] - height_normalizer * pow(de, 2), bars[m_y]);
            }
        }
    } else if (monstercat > 0) {
      for (z = 0; z < number_of_bars; z++) {
          for (m_y = z - 1; m_y >= 0; m_y--) {
              de = z - m_y;
              bars[m_y] = max(bars[z] / pow(monstercat * 1.5, de), bars[m_y]);
            }
          for (m_y = z + 1; m_y < number_of_bars; m_y++) {
              de = m_y - z;
              bars[m_y] = max(bars[z] / pow(monstercat * 1.5, de), bars[m_y]);
            }
        }
    }
  return bars;
}

static void parse_arguments(int argc, char **argv, char *configPath) {
  char *usage = "\n\
                Usage: " PACKAGE " [options]\n\
                        Visualize audio input (SDL GLSL).\n\
\n\
                        Options:\n\
\t-p, --config <path>    Path to config file\n\
\t-v, --version          Print version and exit\n\
\t-h, --help             Show this help and exit\n\n";

             int c;
#ifndef _WIN32
  static struct option long_options[] = {
                                          {"config", required_argument, NULL, 'p'},
                                          {"version", no_argument, NULL, 'v'},
                                          {"help", no_argument, NULL, 'h'},
                                          {0, 0, 0, 0},
                                          };
  opterr = 0;
  while ((c = getopt_long(argc, argv, ":p:vh", long_options, NULL)) != -1) {
#else
  while ((c = getopt(argc, argv, ":p:vh")) != -1) {
#endif
      switch (c) {
        case 'p':
          snprintf(configPath, sizeof(configPath), "%s", optarg);
          break;
        case 'h':
          printf("%s", usage);
          exit(0);
        case 'v':
          printf(PACKAGE " " VERSION "\n");
          exit(0);
        case ':':
          fprintf(stderr, PACKAGE ": error: option requires an argument -- '%c'\n", optopt);
          exit(1);
        case '?':
          if (optopt != 0) {
              fprintf(stderr, PACKAGE ": error: invalid option -- '%c'\n", optopt);
            } else {
              fprintf(stderr, PACKAGE ": error: invalid option\n");
            }
          exit(1);
        default:
          abort();
        }
    }
}

static void setup_signal_handlers() {
#ifdef _WIN32
  if (!SetConsoleCtrlHandler(sig_handler, TRUE)) {
      fprintf(stderr, "ERROR: Could not set control handler");
      exit(EXIT_FAILURE);
    }
#else
  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_handler = &sig_handler;
  sigaction(SIGINT, &action, NULL);
  sigaction(SIGTERM, &action, NULL);
  sigaction(SIGUSR1, &action, NULL);
  sigaction(SIGUSR2, &action, NULL);
#endif
}

static void start_audio_thread(struct config_params *cfg, struct audio_data *audio, pthread_t *p_thread) {
  int timeout_counter = 0;
  struct timespec timeout_timer = {.tv_sec = 0, .tv_nsec = 1000000};
  int thr_id GCC_UNUSED;

  switch (cfg->input) {
#ifndef _WIN32
    case INPUT_PIPEWIRE:
      audio->format = cfg->samplebits;
      audio->rate = cfg->samplerate;
      audio->channels = cfg->channels;
      audio->active = cfg->active;
      audio->remix = cfg->remix;
      audio->virtual_node = cfg->virtual_node;
      thr_id = pthread_create(p_thread, NULL, input_pipewire, (void *)audio);
      break;
#else
    case INPUT_WINSCAP:
      thr_id = pthread_create(p_thread, NULL, input_winscap, (void *)audio);
      break;
#endif
    default:
      cleanup();
      fprintf(stderr, "Unsupported or misconfigured audio input backend.\n");
      exit(EXIT_FAILURE);
    }

  timeout_counter = 0;
  while (true) {
#ifdef _WIN32
      Sleep(1);
#else
      nanosleep(&timeout_timer, NULL);
#endif
      pthread_mutex_lock(&audio->lock);
      if ((audio->threadparams == 0) && (audio->format != -1) && (audio->rate != 0))
        break;

      pthread_mutex_unlock(&audio->lock);
      timeout_counter++;
      if (timeout_counter > 5000) {
          cleanup();
          fprintf(stderr, "could not get rate and/or format, problems with audio thread? quitting...\n");
          exit(EXIT_FAILURE);
        }
    }
  pthread_mutex_unlock(&audio->lock);
}

static void init_cava_buffers(struct cava_buffers *buf, int number_of_bars, int output_channels, int audio_channels, bool split_stereo){
  buf->bars_left = (float *)malloc(number_of_bars / output_channels * sizeof(float));
  buf->bars_right = (float *)malloc(number_of_bars / output_channels * sizeof(float));
  memset(buf->bars_left, 0, sizeof(float) * number_of_bars / output_channels);
  memset(buf->bars_right, 0, sizeof(float) * number_of_bars / output_channels);

  buf->bars = (int *)malloc(number_of_bars * sizeof(int));
  buf->bars_raw = (float *)malloc(number_of_bars * sizeof(float));
  buf->previous_bars_raw = (float *)malloc(number_of_bars * sizeof(float));
  buf->previous_frame = (int *)malloc(number_of_bars * sizeof(int));
  buf->cava_out = (double *)malloc(number_of_bars * audio_channels / output_channels * sizeof(double));

  memset(buf->bars, 0, sizeof(int) * number_of_bars);
  memset(buf->bars_raw, 0, sizeof(float) * number_of_bars);
  memset(buf->previous_bars_raw, 0, sizeof(float) * number_of_bars);
  memset(buf->previous_frame, 0, sizeof(int) * number_of_bars);
  memset(buf->cava_out, 0, sizeof(double) * number_of_bars * audio_channels / output_channels);

  if (split_stereo) {
      buf->right_bars = (int *)malloc(number_of_bars * sizeof(int));
      buf->right_previous_frame = (int *)malloc(number_of_bars * sizeof(int));
      memset(buf->right_bars, 0, sizeof(int) * number_of_bars);
      memset(buf->right_previous_frame, 0, sizeof(int) * number_of_bars);
    } else {
      buf->right_bars = NULL;
      buf->right_previous_frame = NULL;
    }
}

static void free_cava_buffers(struct cava_buffers *buf, int audio_channels, bool split_stereo) {
  if (audio_channels == 2) {
      free(buf->bars_left);
      free(buf->bars_right);
    }
  free(buf->cava_out);
  free(buf->bars);
  free(buf->bars_raw);
  free(buf->previous_bars_raw);
  free(buf->previous_frame);
  if (split_stereo) {
      free(buf->right_bars);
      free(buf->right_previous_frame);
    }
}

static void handle_keyboard_input(char *ch, struct config_params *cfg, char *configPath,
                       struct terminal_dimensions *termDim,
                       bool *resizeTerminal, bool *reloadConf) {
  switch (*ch) {
    case 65: // key up
      cfg->sens = cfg->sens * 1.05;
      break;
    case 66: // key down
      cfg->sens = cfg->sens * 0.95;
      break;
    case 68: // key right
      cfg->bar_width++;
      *resizeTerminal = true;
      break;
    case 67: // key left
      if (cfg->bar_width > 1)
        cfg->bar_width--;
      *resizeTerminal = true;
      break;
    case 'r': // reload config
      should_reload = 1;
      break;
    case 'c': // reload colors
      reload_colors = 1;
      break;
    case 'f': // change foreground color
      if (cfg->col < 7) cfg->col++; else cfg->col = 0;
      *resizeTerminal = true;
      break;
    case 'b': // change background color
      if (cfg->bgcol < 7) cfg->bgcol++; else cfg->bgcol = 0;
      *resizeTerminal = true;
      break;
    case 'o': // change orientation
      cfg->orientation = (cfg->orientation == ORIENT_BOTTOM) ? ORIENT_TOP : ORIENT_BOTTOM;
      if (cfg->orientation == ORIENT_LEFT || cfg->orientation == ORIENT_RIGHT ||
          cfg->orientation == ORIENT_SPLIT_V) {
          termDim->dim_bar = &termDim->height;
          termDim->dim_val = &termDim->width;
        } else {
          termDim->dim_bar = &termDim->width;
          termDim->dim_val = &termDim->height;
        }
      *resizeTerminal = true;
      break;
    case 'q':
      should_reload = 1;
      should_quit = 1;
    }

  *ch = 0;

  if (should_reload) {
      *reloadConf = true;
      *resizeTerminal = true;
      should_reload = 0;
    }

  if (reload_colors) {
      struct error_s error;
      char *themeFile;
      error.length = 0;
      bool result = get_themeFile(configPath, cfg, NULL, &error, &themeFile);
      if (!result) {
          cleanup();
          exit(EXIT_FAILURE);
        }
      if (!load_colors(themeFile, (void *)cfg, &error)) {
          cleanup();
          free(themeFile);
          fprintf(stderr, "Error loading config. %s", error.message);
          exit(EXIT_FAILURE);
        }
      *resizeTerminal = true;
      reload_colors = 0;
      free(themeFile);
    }
}

static int render_output(struct config_params *cfg, struct cava_buffers *buf,
               int number_of_bars, int frame_time_msec, int re_paint) {
  return draw_sdl_glsl(number_of_bars, buf->bars_raw, buf->previous_bars_raw, frame_time_msec,
                        re_paint, cfg->continuous_rendering);
}

static void exit_if_audio_thread_unexpectedly_terminated(struct audio_data *audio) {
  pthread_mutex_lock(&audio->lock);
  if (audio->terminate == 1) {
      cleanup();
      fprintf(stderr, "Audio thread exited unexpectedly. %s\n", audio->error_message);
      exit(EXIT_FAILURE);
    }
  pthread_mutex_unlock(&audio->lock);
}

static void process_audio_chunk(struct audio_data *audio, struct config_params *cfg,
                     struct cava_buffers *buf, struct cava_plan *plan,
                     int high_framerate, int samples_per_frame,
                     int audio_channels, int number_of_bars) {
  pthread_mutex_lock(&audio->lock);

  int samples_to_use = 0;

  if (!high_framerate) {
      samples_to_use = audio->samples_counter;
    } else {
      samples_to_use = samples_per_frame * audio_channels;

      if (audio->samples_counter < samples_to_use) {
          samples_to_use = audio->samples_counter;
        }

      if (audio->samples_counter > audio->input_buffer_size + samples_to_use) {
          samples_to_use = audio->samples_counter - audio->input_buffer_size;
        }
    }

  if (cfg->waveform) {
      for (int n = 0; n < samples_to_use; n++) {
          for (int i = number_of_bars - 1; i > 0; i--) {
              buf->cava_out[i] = buf->cava_out[i - 1];
            }
          if (audio_channels == 2) {
              buf->cava_out[0] = cfg->sens * (audio->cava_in[n] / 2 + audio->cava_in[n + 1] / 2);
              n++;
            } else {
              buf->cava_out[0] = cfg->sens * audio->cava_in[n];
            }
        }
    } else {
      cava_execute(audio->cava_in, samples_to_use, buf->cava_out, plan);
    }

  audio->samples_counter -= samples_to_use;

  if (audio->samples_counter != 0) {
      for (int n = 0; n < audio->samples_counter; n++) {
          audio->cava_in[n] = audio->cava_in[n + samples_to_use];
        }
    }

  pthread_mutex_unlock(&audio->lock);
}

static void format_and_filter_bars(struct cava_buffers *buf, struct config_params *cfg,
                        struct terminal_dimensions *termDim,
                        int raw_number_of_bars, int number_of_bars,
                        int audio_channels, int output_channels,
                        double userEQ_keys_to_bars_ratio) {
  for (int n = 0; n < raw_number_of_bars; n++) {
      if (!cfg->waveform) {
          buf->cava_out[n] *= cfg->sens;
        } else {
          if (buf->cava_out[n] > 1.0)
            cfg->sens *= 0.999;
          else
            cfg->sens *= 1.00001;

          if (cfg->orientation != ORIENT_SPLIT_H)
            buf->cava_out[n] = (buf->cava_out[n] + 1.0) / 2.0;
        }

      if (cfg->sdl_glsl_gain != 1.0) {
          buf->cava_out[n] *= cfg->sdl_glsl_gain;
        }

      if (buf->cava_out[n] > 1.0)
        buf->cava_out[n] = 1.0;
      else if (buf->cava_out[n] < 0.0)
        buf->cava_out[n] = 0.0;

      if (cfg->orientation == ORIENT_SPLIT_H || cfg->orientation == ORIENT_SPLIT_V) {
          buf->cava_out[n] /= 2;
        }

      if (cfg->waveform) {
          buf->bars_raw[n] = buf->cava_out[n];
        }
    }

  if (!cfg->waveform) {
      if (audio_channels == 2) {
          for (int n = 0; n < number_of_bars / output_channels; n++) {
              if (cfg->userEQ_enabled)
                buf->cava_out[n] *= cfg->userEQ[(int)floor(((double)n) * userEQ_keys_to_bars_ratio)];
              buf->bars_left[n] = buf->cava_out[n];
            }
          for (int n = 0; n < number_of_bars / output_channels; n++) {
              if (cfg->userEQ_enabled)
                buf->cava_out[n + number_of_bars / output_channels] *= cfg->userEQ[(int)floor(((double)n) * userEQ_keys_to_bars_ratio)];
              buf->bars_right[n] = buf->cava_out[n + number_of_bars / output_channels];
            }
        } else {
          for (int n = 0; n < number_of_bars; n++) {
              if (cfg->userEQ_enabled)
                buf->cava_out[n] *= cfg->userEQ[(int)floor(((double)n) * userEQ_keys_to_bars_ratio)];
              buf->bars_raw[n] = buf->cava_out[n];
            }
        }

      if (cfg->monstercat) {
          if (audio_channels == 2) {
              buf->bars_left = monstercat_filter(buf->bars_left, number_of_bars / output_channels,
                                                  cfg->waves, cfg->monstercat, *termDim->dim_val);
              buf->bars_right = monstercat_filter(buf->bars_right, number_of_bars / output_channels,
                                                   cfg->waves, cfg->monstercat, *termDim->dim_val);
            } else {
              buf->bars_raw = monstercat_filter(buf->bars_raw, number_of_bars, cfg->waves,
                                                 cfg->monstercat, *termDim->dim_val);
            }
        }

      if (audio_channels == 2) {
          if (cfg->stereo) {
              if ((cfg->orientation == ORIENT_SPLIT_H || cfg->orientation == ORIENT_SPLIT_V) && cfg->split_stereo) {
                  for (int n = 0; n < number_of_bars; n++) {
                      if (n < number_of_bars / 2) {
                          if (cfg->reverse) buf->bars_raw[n] = buf->bars_left[number_of_bars / 2 - n - 1];
                          else buf->bars_raw[n] = buf->bars_left[n];
                        } else {
                          if (cfg->reverse) buf->bars_raw[n] = buf->bars_right[number_of_bars - n - 1];
                          else buf->bars_raw[n] = buf->bars_right[n - number_of_bars / 2];
                        }
                    }
                } else {
                  for (int n = 0; n < number_of_bars; n++) {
                      if (n < number_of_bars / 2) {
                          if (cfg->reverse) buf->bars_raw[n] = buf->bars_left[n];
                          else buf->bars_raw[n] = buf->bars_left[number_of_bars / 2 - n - 1];
                        } else {
                          if (cfg->reverse) buf->bars_raw[n] = buf->bars_right[number_of_bars - n - 1];
                          else buf->bars_raw[n] = buf->bars_right[n - number_of_bars / 2];
                        }
                    }
                }
            } else {
              for (int n = 0; n < number_of_bars; n++) {
                  if (cfg->reverse) {
                      if (cfg->mono_opt == AVERAGE) buf->bars_raw[number_of_bars - n - 1] = (buf->bars_left[n] + buf->bars_right[n]) / 2;
                      else if (cfg->mono_opt == LEFT) buf->bars_raw[number_of_bars - n - 1] = buf->bars_left[n];
                      else if (cfg->mono_opt == RIGHT) buf->bars_raw[number_of_bars - n - 1] = buf->bars_right[n];
                    } else {
                      if (cfg->mono_opt == AVERAGE) buf->bars_raw[n] = (buf->bars_left[n] + buf->bars_right[n]) / 2;
                      else if (cfg->mono_opt == LEFT) buf->bars_raw[n] = buf->bars_left[n];
                      else if (cfg->mono_opt == RIGHT) buf->bars_raw[n] = buf->bars_right[n];
                    }
                }
            }
        }
    }
}

int main(int argc, char **argv) {
  struct config_params cfg;
  memset(&cfg, 0, sizeof(cfg));
  char configPath[PATH_MAX];
  configPath[0] = '\0';

  setup_signal_handlers();
  parse_arguments(argc, argv, configPath);

  while (1) {
      struct error_s error;
      error.length = 0;
      if (!load_config(configPath, &cfg, &error)) {
          fprintf(stderr, "Error loading config. %s", error.message);
          exit(EXIT_FAILURE);
        }

             // Wymuszenie trybu SDL GLSL
      cfg.output = OUTPUT_SDL_GLSL;
#ifndef _WIN32
      cfg.input = INPUT_PIPEWIRE;
#else
      cfg.input = INPUT_WINSCAP;
#endif

      time_t config_mtime = 0;
      long long config_size = 0;
      bool has_config_state = false;
      if (cfg.live_config) {
          has_config_state = get_file_state(configPath, &config_mtime, &config_size);
        }

      struct audio_data audio;
      memset(&audio, 0, sizeof(audio));

      audio.source = malloc(1 + strlen(cfg.audio_source));
      strcpy(audio.source, cfg.audio_source);

      audio.format = -1;
      audio.rate = 0;
      audio.samples_counter = 0;
      audio.channels = 2;
      audio.IEEE_FLOAT = 0;
      audio.autoconnect = 0;
      audio.input_buffer_size = BUFFER_SIZE * audio.channels;
      audio.cava_buffer_size = 16384;
      audio.cava_in = (double *)malloc(audio.cava_buffer_size * sizeof(double));
      memset(audio.cava_in, 0, sizeof(double) * audio.cava_buffer_size);

      audio.threadparams = 0;
      audio.terminate = 0;

      pthread_t p_thread;
      pthread_mutex_init(&audio.lock, NULL);

      start_audio_thread(&cfg, &audio, &p_thread);

      int audio_channels = audio.channels;

      if (cfg.upper_cut_off > audio.rate / 2) {
          cleanup();
          fprintf(stderr, "higher cutoff frequency can't be higher than sample rate / 2\n");
          exit(EXIT_FAILURE);
        }

      if (cfg.stereo && audio_channels == 1)
        cfg.stereo = 0;

      int output_channels = 1;
      if (cfg.stereo)
        output_channels = 2;

      struct cava_buffers buf;
      struct terminal_dimensions termDim;
      termDim.width = 0;
      termDim.height = 0;

      if (cfg.orientation == ORIENT_LEFT || cfg.orientation == ORIENT_RIGHT || cfg.orientation == ORIENT_SPLIT_V) {
          termDim.dim_bar = &termDim.height;
          termDim.dim_val = &termDim.width;
        } else {
          termDim.dim_bar = &termDim.width;
          termDim.dim_val = &termDim.height;
        }

      init_sdl_glsl_window(cfg.sdl_width, cfg.sdl_height, cfg.sdl_x, cfg.sdl_y, cfg.sdl_full_screen,
                            cfg.vertex_shader, cfg.fragment_shader);

      termDim.height = cfg.sdl_height;
      termDim.width = cfg.sdl_width;
      *termDim.dim_val = 1;

      bool reloadConf = false;
      while (!reloadConf) {
          init_sdl_glsl_surface(&termDim.width, &termDim.height, cfg.color, cfg.bcolor, cfg.bar_width,
                                 cfg.bar_spacing, cfg.gradient, cfg.gradient_count,
                                 cfg.gradient_colors);

          int number_of_bars = -1;
          if (cfg.fixedbars) {
              number_of_bars = cfg.fixedbars;
              if (number_of_bars < output_channels) {
                  cleanup();
                  fprintf(stderr, "fixed number of bars must be at least 1 with mono output and 2 with stereo output\n");
                  exit(EXIT_FAILURE);
                }
              if (output_channels == 2 && number_of_bars % 2 != 0) {
                  cleanup();
                  fprintf(stderr, "must have even number of bars with stereo output\n");
                  exit(EXIT_FAILURE);
                }
            } else {
              number_of_bars = (*termDim.dim_bar + cfg.bar_spacing) / (cfg.bar_width + cfg.bar_spacing);
              if (number_of_bars > 512) number_of_bars = 512;
              if (number_of_bars <= 1) number_of_bars = cfg.stereo ? 2 : 1;
              if (output_channels == 2 && number_of_bars % 2 != 0) number_of_bars--;
            }

          if ((cfg.orientation == ORIENT_SPLIT_H || cfg.orientation == ORIENT_SPLIT_V) && cfg.split_stereo) {
              number_of_bars *= 2;
            }

          cfg.number_of_bars = number_of_bars;
          cfg.terminal_width = termDim.width;

          int raw_number_of_bars = (number_of_bars / output_channels) * audio_channels;
          if (cfg.waveform) {
              raw_number_of_bars = number_of_bars;
            }

          double userEQ_keys_to_bars_ratio = 1.0;
          if (cfg.userEQ_enabled && (number_of_bars / output_channels > 0)) {
              userEQ_keys_to_bars_ratio = (double)(((double)cfg.userEQ_keys) /
                                                   ((double)(number_of_bars / output_channels)));
            }

          struct cava_plan *plan =
              cava_init(number_of_bars / output_channels, audio.rate, audio.channels, cfg.autosens,
                         cfg.noise_reduction, cfg.lower_cut_off, cfg.upper_cut_off);

          if (plan->status == -1) {
              cleanup();
              fprintf(stderr, "Error initializing cava. %s", plan->error_message);
              exit(EXIT_FAILURE);
            }

          if (plan->input_buffer_size != audio.cava_buffer_size) {
              pthread_mutex_lock(&audio.lock);
              audio.cava_buffer_size = plan->input_buffer_size;
              free(audio.cava_in);
              audio.cava_in = (double *)malloc(audio.cava_buffer_size * sizeof(double));
              memset(audio.cava_in, 0, sizeof(double) * audio.cava_buffer_size);
              pthread_mutex_unlock(&audio.lock);
            }

          init_cava_buffers(&buf, number_of_bars, output_channels, audio_channels, cfg.split_stereo);

          bool resizeTerminal = false;

          long long frame_time_ns = (long long)(1000000000.0 / (double)cfg.framerate);
          if (frame_time_ns < 1) frame_time_ns = 1;
          int frame_time_msec = (int)(frame_time_ns / 1000000LL);
          if (frame_time_msec < 1) frame_time_msec = 1;

          char ch = '\0';
          float actual_framerate = 1000.0 / (float)frame_time_msec;
          int samples_per_frame = audio.rate / actual_framerate;

          int high_framerate = 0;
          if (samples_per_frame < audio.input_buffer_size / audio_channels) {
              high_framerate = 1;
            }

          pthread_mutex_lock(&audio.lock);
          audio.samples_counter = 0;
          pthread_mutex_unlock(&audio.lock);

          while (!resizeTerminal) {
              handle_keyboard_input(&ch, &cfg, configPath, &termDim, &resizeTerminal, &reloadConf);

              if (resizeTerminal) break;

              exit_if_audio_thread_unexpectedly_terminated(&audio);

              process_audio_chunk(&audio, &cfg, &buf, plan, high_framerate, samples_per_frame, audio_channels, number_of_bars);
              format_and_filter_bars(&buf, &cfg, &termDim, raw_number_of_bars, number_of_bars, audio_channels, output_channels, userEQ_keys_to_bars_ratio);

              int re_paint = 0;
              for (int n = 0; n < number_of_bars; n++) {
                  buf.bars[n] = buf.bars_raw[n] * 1000; // uzywane dla SDL GLSL zeby sprawdzic zmiany
                  if (buf.bars[n] != buf.previous_frame[n])
                    re_paint = 1;
                }

              int rc = render_output(&cfg, &buf, number_of_bars, frame_time_msec, re_paint);

              if (rc == -1) resizeTerminal = true;
              if (rc == -2) {
                  resizeTerminal = true;
                  reloadConf = true;
                  should_quit = true;
                }

              memcpy(buf.previous_frame, buf.bars, number_of_bars * sizeof(int));
              memcpy(buf.previous_bars_raw, buf.bars_raw, number_of_bars * sizeof(float));

              if (cfg.live_config) {
                  time_t new_mtime = 0;
                  long long new_size = 0;
                  if (get_file_state(configPath, &new_mtime, &new_size) &&
                      (!has_config_state || new_mtime != config_mtime || new_size != config_size)) {
                      should_reload = 1;
                      config_mtime = new_mtime;
                      config_size = new_size;
                      has_config_state = true;
                    }
                }
            }

          cava_destroy(plan);
          free(plan);
          free_cava_buffers(&buf, audio_channels, cfg.split_stereo);
        }

      pthread_mutex_lock(&audio.lock);
      audio.terminate = 1;
      pthread_mutex_unlock(&audio.lock);
      pthread_join(p_thread, NULL);

      pthread_mutex_destroy(&audio.lock);

      free(audio.source);
      free(audio.cava_in);
      cleanup();
      free_config(&cfg);

      if (should_quit && signal_received == 0) {
          return EXIT_SUCCESS;
        }

      if (signal_received != 0) {
          signal(signal_received, SIG_DFL);
          raise(signal_received);
        }
    }
}