/* Copyright (c) 2017-2018 Mozilla */
/*
   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

   - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "kiss_fft.h"
#include "common.h"
#include <math.h>
#include "freq.h"
#include "pitch.h"
#include "arch.h"
#include "celt_lpc.h"
#include "codec2_pitch.h"
#include <assert.h>
#include <getopt.h>


#define PITCH_MIN_PERIOD 32
#define PITCH_MAX_PERIOD 256
#define PITCH_FRAME_SIZE 320
#define PITCH_BUF_SIZE (PITCH_MAX_PERIOD+PITCH_FRAME_SIZE)

#define CEPS_MEM 8
#define NB_DELTA_CEPS 6

#define NB_FEATURES (2*NB_BANDS+3+LPC_ORDER)


typedef struct {
  float analysis_mem[OVERLAP_SIZE];
  float cepstral_mem[CEPS_MEM][NB_BANDS];
  float pitch_buf[PITCH_BUF_SIZE];
  float last_gain;
  int last_period;
  float lpc[LPC_ORDER];
  float sig_mem[LPC_ORDER];
  int exc_mem;
} DenoiseState;

static int rnnoise_get_size() {
  return sizeof(DenoiseState);
}

static int rnnoise_init(DenoiseState *st) {
  memset(st, 0, sizeof(*st));
  return 0;
}

static DenoiseState *rnnoise_create() {
  DenoiseState *st;
  st = malloc(rnnoise_get_size());
  rnnoise_init(st);
  return st;
}

static void rnnoise_destroy(DenoiseState *st) {
  free(st);
}

static short float2short(float x)
{
  int i;
  i = (int)floor(.5+x);
  return IMAX(-32767, IMIN(32767, i));
}

int lowpass = FREQ_SIZE;
int band_lp = NB_BANDS;

static void frame_analysis(DenoiseState *st, kiss_fft_cpx *X, float *Ex, const float *in) {
  int i;
  float x[WINDOW_SIZE];
  RNN_COPY(x, st->analysis_mem, OVERLAP_SIZE);
  RNN_COPY(&x[OVERLAP_SIZE], in, FRAME_SIZE);
  RNN_COPY(st->analysis_mem, &in[FRAME_SIZE-OVERLAP_SIZE], OVERLAP_SIZE);
  apply_window(x);
  forward_transform(X, x);
  for (i=lowpass;i<FREQ_SIZE;i++)
    X[i].r = X[i].i = 0;
  compute_band_energy(Ex, X);
}

static void compute_frame_features(DenoiseState *st, kiss_fft_cpx *X, kiss_fft_cpx *P,
				   float *Ex, float *Ep, float *Exp, float *features, const float *in,
				   int logmag) {
  int i;
  float E = 0;
  float Ly[NB_BANDS];
  float p[WINDOW_SIZE];
  float pitch_buf[PITCH_BUF_SIZE];
  int pitch_index;
  float gain;
  float tmp[NB_BANDS];
  float follow, logMax;
  float g;

  for(i=0; i<NB_FEATURES; i++) features[i] = 0.0;
  
  frame_analysis(st, X, Ex, in);
  RNN_MOVE(st->pitch_buf, &st->pitch_buf[FRAME_SIZE], PITCH_BUF_SIZE-FRAME_SIZE);
  RNN_COPY(&st->pitch_buf[PITCH_BUF_SIZE-FRAME_SIZE], in, FRAME_SIZE);
  RNN_COPY(pitch_buf, &st->pitch_buf[0], PITCH_BUF_SIZE);
  pitch_downsample(pitch_buf, PITCH_BUF_SIZE);
  pitch_search(pitch_buf+PITCH_MAX_PERIOD, pitch_buf, PITCH_FRAME_SIZE<<1,
               (PITCH_MAX_PERIOD-3*PITCH_MIN_PERIOD)<<1, &pitch_index);
  pitch_index = 2*PITCH_MAX_PERIOD-pitch_index;
  gain = remove_doubling(pitch_buf, 2*PITCH_MAX_PERIOD, 2*PITCH_MIN_PERIOD,
          2*PITCH_FRAME_SIZE, &pitch_index, st->last_period, st->last_gain);
  st->last_period = pitch_index;
  st->last_gain = gain;
  for (i=0;i<WINDOW_SIZE;i++)
    p[i] = st->pitch_buf[PITCH_BUF_SIZE-WINDOW_SIZE-pitch_index/2+i];
  apply_window(p);
  forward_transform(P, p);
  compute_band_energy(Ep, P);
  compute_band_corr(Exp, X, P);
  for (i=0;i<NB_BANDS;i++) Exp[i] = Exp[i]/sqrt(.001+Ex[i]*Ep[i]);
  dct(tmp, Exp);

  logMax = -2;
  follow = -2;
  for (i=0;i<NB_BANDS;i++) {
    Ly[i] = log10(1e-2+Ex[i]);
    Ly[i] = MAX16(logMax-8, MAX16(follow-2.5, Ly[i]));
    logMax = MAX16(logMax, Ly[i]);
    follow = MAX16(follow-2.5, Ly[i]);
    E += Ex[i];
  }

  if (logmag) {
    memcpy(features, Ly, sizeof(float)*NB_BANDS);
    float Ex[NB_BANDS];
    RNN_COPY(Ex, Ly, NB_BANDS);
    for (i=0;i<NB_BANDS;i++) Ex[i] = pow(10.f, Ly[i]);
    g = lpc_from_bands(st->lpc, Ex);
  }
  else {
    dct(features, Ly);
    features[0] -= 4;
    g = lpc_from_cepstrum(st->lpc, features);
  }

  features[2*NB_BANDS] = .01*(pitch_index-200);
  features[2*NB_BANDS+1] = gain;
  features[2*NB_BANDS+2] = log10(g);
  for (i=0;i<LPC_ORDER;i++) features[2*NB_BANDS+3+i] = st->lpc[i];
}

static void biquad(float *y, float mem[2], const float *x, const float *b, const float *a, int N) {
  int i;
  for (i=0;i<N;i++) {
    float xi, yi;
    xi = x[i];
    yi = x[i] + mem[0];
    mem[0] = mem[1] + (b[0]*(double)xi - a[0]*(double)yi);
    mem[1] = (b[1]*(double)xi - a[1]*(double)yi);
    y[i] = yi;
  }
}

static void preemphasis(float *y, float *mem, const float *x, float coef, int N) {
  int i;
  for (i=0;i<N;i++) {
    float yi;
    yi = x[i] + *mem;
    *mem = -coef*x[i];
    y[i] = yi;
  }
}

static float uni_rand() {
  return rand()/(double)RAND_MAX-.5;
}

static void rand_resp(float *a, float *b) {
  a[0] = .75*uni_rand();
  a[1] = .75*uni_rand();
  b[0] = .75*uni_rand();
  b[1] = .75*uni_rand();
}

void write_audio(DenoiseState *st, const short *pcm, float noise_std, FILE *file) {
  int i;
  unsigned char data[4*FRAME_SIZE];
  for (i=0;i<FRAME_SIZE;i++) {
    int noise;
    float p=0;
    float e;
    int j;
    for (j=0;j<LPC_ORDER;j++) p -= st->lpc[j]*st->sig_mem[j];
    e = lin2ulaw(pcm[i] - p);
    /* Signal. */
    data[4*i] = lin2ulaw(st->sig_mem[0]);
    /* Prediction. */
    data[4*i+1] = lin2ulaw(p);
    /* Excitation in. */
    data[4*i+2] = st->exc_mem;
    /* Excitation out. */
    data[4*i+3] = e;
    /* Simulate error on excitation. */
    noise = (int)floor(.5 + noise_std*.707*(log_approx((float)rand()/RAND_MAX)-log_approx((float)rand()/RAND_MAX)));
    e += noise;
    e = IMIN(255, IMAX(0, e));
    
    RNN_MOVE(&st->sig_mem[1], &st->sig_mem[0], LPC_ORDER-1);
    st->sig_mem[0] = p + ulaw2lin(e);
    st->exc_mem = e;
  }
  fwrite(data, 4*FRAME_SIZE, 1, file);
}

int main(int argc, char **argv) {
  int i;
  int count=0;
  static const float a_hp[2] = {-1.99599, 0.99600};
  static const float b_hp[2] = {-2, 1};
  float a_sig[2] = {0};
  float b_sig[2] = {0};
  float mem_hp_x[2]={0};
  float mem_resp_x[2]={0};
  float mem_preemph=0;
  float x[FRAME_SIZE];
  int gain_change_count=0;
  FILE *f1;
  FILE *ffeat;
  FILE *fpcm=NULL;
  short pcm[FRAME_SIZE]={0};
  short tmp[FRAME_SIZE] = {0};
  float savedX[FRAME_SIZE] = {0};
  float speech_gain=1;
  int last_silent = 1;
  float old_speech_gain = 1;
  int one_pass_completed = 0;
  DenoiseState *st;
  float noise_std=0;
  int training = -1;
  int c2pitch_en = 0;
  int c2voicing_en = 0;
  int nvec = 5000000;
  int fuzz = 1;
  int logmag = 0;
  int  nb_passes = 0;
  int  nb_frames_per_pass = 0;  

  st = rnnoise_create();

  int o = 0;
  int opt_idx = 0;
  while( o != -1 ) {
      static struct option long_opts[] = {
          {"c2pitch",   no_argument,      0, 'c'},
          {"c2voicing", no_argument,       0, 'v'},
          {"help",      no_argument,      0, 'h'},
	  {"mag",       no_argument,      0, 'i'},
          {"nvec",      required_argument,0, 'n'},
          {"train",     no_argument,      0, 'r'},
          {"fuzz",      required_argument,0, 'z'},
          {"test",      no_argument,      0, 't'},
          {0, 0, 0, 0}
      };
        
      o = getopt_long(argc,argv,"chn:rtz:i",long_opts,&opt_idx);
        
      switch(o){
      case 'r':
          training = 1;
          break;
      case 'n':
	  nvec = atof(optarg);
	  assert(nvec > 0);
	  fprintf(stderr, "nvec: %d\n", nvec);
	  break;
      case 'i':
	  logmag = 1;
	  fprintf(stderr, "logmag: %d\n", logmag);
	  break;
      case 't':
          training = 0;
          break;
      case 'c':
          c2pitch_en = 1;
          break;
      case 'v':
          c2voicing_en = 1;
          break;
      case 'z':
          fuzz = atoi(optarg);
          break;
      case 'h':
      case '?':
          goto helpmsg;
          break;
      }
  }
  int dx = optind;
    
  if (training == -1) goto helpmsg;

  if ( (training && (argc - dx) < 3) || (!training && (argc - dx) < 2)) {
      fprintf(stderr, "Too few arguments\n");
      goto helpmsg;
  }
    
  if ( argc - dx > 3 ) {
      fprintf(stderr, "Too many arguments\n");
  helpmsg:
      fprintf(stderr, "usage: %s --train [options] <speech> <features out> <pcm out>\n", argv[0]);
      fprintf(stderr, "  or   %s --test [options] <speech> <features out>\n", argv[0]);
      fprintf(stderr, "\nOptions:\n");
      fprintf(stderr, "  -c --c2pitch   Codec 2 pitch estimator\n");
      fprintf(stderr, "  -v --c2voicing Codec 2 voicing estimator\n");
      fprintf(stderr, "  -i --mag       output magnitudes Ly rather than dct(Ly)\n");
      fprintf(stderr, "  -n --nvec      Number of training vectors to generate\n");
      fprintf(stderr, "  -z --fuzz      fuzz freq response and gain during training (default on)\n");
      exit(1);
  }
    
  if (strcmp(argv[dx], "-") == 0)
      f1 = stdin;
  else {
      f1 = fopen(argv[dx], "rb");
      if (f1 == NULL) {
          fprintf(stderr,"Error opening input .s16 16kHz speech input file: %s\n", argv[dx]);
          exit(1);
      }
  }
  if (strcmp(argv[dx+1], "-") == 0)
      ffeat = stdout;
  else {
      ffeat = fopen(argv[dx+1], "wb");
      if (ffeat == NULL) {
          fprintf(stderr,"Error opening output feature file: %s\n", argv[dx+1]);
          exit(1);
      }
  }
  if (training) {
      fpcm = fopen(argv[dx+2], "wb");
      if (fpcm == NULL) {
          fprintf(stderr,"Error opening output PCM file: %s\n", argv[dx+2]);
          exit(1);
      }
  }

  /* optionally fire up Codec 2 pitch estimator */
  CODEC2_PITCH *c2pitch = NULL;
  int c2_Sn_size, c2_frame_size;
  float *c2_Sn = NULL;
  if (c2pitch_en) {
      c2pitch = codec2_pitch_create(&c2_Sn_size, &c2_frame_size);
      assert(FRAME_SIZE == c2_frame_size);
      c2_Sn = (float*)malloc(sizeof(float)*c2_Sn_size); assert(c2_Sn != NULL);
      for(i=0; i<c2_Sn_size; i++) c2_Sn[i] = 0.0;
  }
  
  while (1) {
    kiss_fft_cpx X[FREQ_SIZE], P[WINDOW_SIZE];
    float Ex[NB_BANDS], Ep[NB_BANDS];
    float Exp[NB_BANDS];
    float features[NB_FEATURES];
    float E=0;
    int silent;
    for (i=0;i<FRAME_SIZE;i++) x[i] = tmp[i];
    int nread = fread(tmp, sizeof(short), FRAME_SIZE, f1);
    if (nread != FRAME_SIZE) {
      if (!training) break;
      rewind(f1);
      nread = fread(tmp, sizeof(short), FRAME_SIZE, f1);
      one_pass_completed = 1;
      nb_passes++;
      if (!nb_frames_per_pass)
	  nb_frames_per_pass = count;
    }
    for (i=0;i<FRAME_SIZE;i++) E += tmp[i]*(float)tmp[i];
    if (training) {
      silent = E < 5000 || (last_silent && E < 20000);
      if (!last_silent && silent) {
        for (i=0;i<FRAME_SIZE;i++) savedX[i] = x[i];
      }
      if (last_silent && !silent) {
          for (i=0;i<FRAME_SIZE;i++) {
            float f = (float)i/FRAME_SIZE;
            tmp[i] = (int)floor(.5 + f*tmp[i] + (1-f)*savedX[i]);
          }
      }
      if (last_silent) {
        last_silent = silent;
        continue;
      }
      last_silent = silent;
    }
    if (count>=nvec && one_pass_completed) break;
    if (fuzz && training && ++gain_change_count > 2821) {
      float tmp;
      speech_gain = pow(10., (-20+(rand()%40))/20.);
      if (rand()%20==0) speech_gain *= .01;
      if (rand()%100==0) speech_gain = 0;
      gain_change_count = 0;
      rand_resp(a_sig, b_sig);
      tmp = (float)rand()/RAND_MAX;
      noise_std = 4*tmp*tmp;
      //fprintf(stderr, "speech_gain: %f noise_std: %f a_sig: %f %fb_sig: %f %f\n",
      //        speech_gain, noise_std, a_sig[0], a_sig[1], b_sig[0], b_sig[1]);
    }
    biquad(x, mem_hp_x, x, b_hp, a_hp, FRAME_SIZE);
    biquad(x, mem_resp_x, x, b_sig, a_sig, FRAME_SIZE);
    preemphasis(x, &mem_preemph, x, PREEMPHASIS, FRAME_SIZE);
    for (i=0;i<FRAME_SIZE;i++) {
      float g;
      float f = (float)i/FRAME_SIZE;
      g = f*speech_gain + (1-f)*old_speech_gain;
      x[i] *= g;
    }
    for (i=0;i<FRAME_SIZE;i++) x[i] += rand()/(float)RAND_MAX - .5;
    compute_frame_features(st, X, P, Ex, Ep, Exp, features, x, logmag);

    if (c2pitch_en) {
        for(i=0; i<c2_Sn_size-c2_frame_size; i++)
            c2_Sn[i] = c2_Sn[i+c2_frame_size];
        for(i=0; i<c2_frame_size; i++)
            c2_Sn[i+c2_Sn_size-c2_frame_size] = x[i];
        float f0, voicing; int pitch_index;
        pitch_index = codec2_pitch_est(c2pitch, c2_Sn, &f0, &voicing);
 	if (pitch_index >= 2*PITCH_MAX_PERIOD) pitch_index = 2*PITCH_MAX_PERIOD-1;
	if (pitch_index < 2*PITCH_MIN_PERIOD) pitch_index = 2*PITCH_MIN_PERIOD;
	assert(pitch_index < 2*PITCH_MAX_PERIOD);
	assert(pitch_index >= 2*PITCH_MIN_PERIOD);
        features[2*NB_BANDS] = 0.01*(pitch_index-200);
        if (c2voicing_en) features[2*NB_BANDS+1] = voicing;
    }
    fwrite(features, sizeof(float), NB_FEATURES, ffeat);
    /* PCM is delayed by 1/2 frame to make the features centered on the frames. */
    for (i=0;i<FRAME_SIZE-TRAINING_OFFSET;i++) pcm[i+TRAINING_OFFSET] = float2short(x[i]);
    if (fpcm) write_audio(st, pcm, noise_std, fpcm);
    //if (fpcm) fwrite(pcm, sizeof(short), FRAME_SIZE, fpcm);
    for (i=0;i<TRAINING_OFFSET;i++) pcm[i] = float2short(x[i+FRAME_SIZE-TRAINING_OFFSET]);
    old_speech_gain = speech_gain;
    count++;
  }
  fclose(f1);
  fclose(ffeat);
  if (fpcm) fclose(fpcm);
  if (c2pitch_en) { free(c2_Sn); codec2_pitch_destroy(c2pitch); }
  if (training) {
      int minutes = (float)count*0.01/60.0;     
      int minutes_per_pass = (float)nb_frames_per_pass*0.01/60.0;     
      fprintf(stderr, "frames: %d (%d minutes) frames_per_pass: %d (%d minutes) nb_passes: %d\n",
	      count, minutes, nb_frames_per_pass, minutes_per_pass, nb_passes);
  }
  rnnoise_destroy(st);
  return 0;
}

