#include "rf_driver_cfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <math.h>
#include <getopt.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>

#ifdef HAS_BLADERF
#include <libbladeRF.h>
#include "rf_driver_bladerf.h"
#include "../common_misc.h"

extern pthread_mutex_t callback_lock;
extern volatile IQ_TYPE *rx_buf;
extern volatile int rx_buf_offset; // remember to initialize it!
extern volatile bool do_exit;

struct bladerf_stream *bladerf_rx_stream;
pthread_t bladerf_rx_task;
struct bladerf_data bladerf_rx_data;

extern void sigint_callback_handler(int signum);

void *bladerf_stream_callback(struct bladerf *dev, struct bladerf_stream *stream,
                      struct bladerf_metadata *metadata, void *samples,
                      size_t num_samples, void *user_data)
{
    struct bladerf_data *my_data = (struct bladerf_data *)user_data;

    //count++ ;

    //if( (count&0xffff) == 0 ) {
    //    fprintf( stderr, "Called 0x%8.8x times\n", count ) ;
    //}

    /* Save off the samples to disk if we are in RX */
    //if( my_data->module == BLADERF_MODULE_RX ) {
        size_t i;
        int16_t *sample = (int16_t *)samples ;
        if (num_samples>0) {
          pthread_mutex_lock(&callback_lock);
          for(i = 0; i < num_samples ; i++ ) {
              //*(sample) &= 0xfff ;
              //if( (*sample)&0x800 ) *(sample) |= 0xf000 ;
              //*(sample+1) &= 0xfff ;
              //if( *(sample+1)&0x800 ) *(sample+1) |= 0xf000 ;
              //fprintf( my_data->fout, "%d, %d\n", *sample, *(sample+1) );

              rx_buf[rx_buf_offset] = (((*sample)>>4)&0xFF);
              rx_buf[rx_buf_offset+1] = (((*(sample+1))>>4)&0xFF);
              rx_buf_offset = (rx_buf_offset+2)&( LEN_BUF-1 ); //cyclic buffer

              sample += 2 ;
          }
          pthread_mutex_unlock(&callback_lock);
        }
        //my_data->samples_left -= num_samples ;
        //if( my_data->samples_left <= 0 ) {
        //    do_exit = true ;
        //}
    //}

    if (do_exit) {
        return NULL;
    } else {
        void *rv = my_data->buffers[my_data->idx];
        my_data->idx = (my_data->idx + 1) % my_data->num_buffers;
        return rv ;
    }
}

int bladerf_tune(void *dev, uint64_t freq_hz) {
  int status;
  status = bladerf_set_frequency((struct bladerf *)dev, BLADERF_MODULE_RX, freq_hz);
  if (status != 0) {
      fprintf(stderr, "Failed to set frequency: %s\n",
              bladerf_strerror(status));
      bladerf_close((struct bladerf *)dev);
      return EXIT_FAILURE;
  }
  return(0);
}

void *bladerf_rx_task_run(void *tmp)
{
  int status;

  /* Start stream and stay there until we kill the stream */
  status = bladerf_stream(bladerf_rx_stream, BLADERF_MODULE_RX);
  if (status < 0) {
    fprintf(stderr, "RX stream failure: %s\r\n", bladerf_strerror(status));
  }
  return NULL;
}

inline int bladerf_config_run_board(uint64_t freq_hz, int gain, void **rf_dev) {
  int status;
  unsigned int actual;
  struct bladerf *dev = NULL;

  (*rf_dev) = NULL;

  bladerf_rx_data.idx = 0;
  bladerf_rx_data.num_buffers = 2;
  bladerf_rx_data.samples_per_buffer = (LEN_BUF/2);

  #ifdef _MSC_VER
    SetConsoleCtrlHandler( (PHANDLER_ROUTINE) sighandler, TRUE );
  #else
    if (signal(SIGINT, sigint_callback_handler)==SIG_ERR ||
        signal(SIGILL, sigint_callback_handler)==SIG_ERR ||
        signal(SIGFPE, sigint_callback_handler)==SIG_ERR ||
        signal(SIGSEGV,sigint_callback_handler)==SIG_ERR ||
        signal(SIGTERM,sigint_callback_handler)==SIG_ERR ||
        signal(SIGABRT,sigint_callback_handler)==SIG_ERR) {
          fprintf(stderr, "bladerf_config_run_board: Failed to set up signal handler\n");
          return EXIT_FAILURE;
      }
  #endif

  status = bladerf_open(&dev, NULL);
  if (status < 0) {
      fprintf(stderr, "bladerf_config_run_board: Failed to open device: %s\n", bladerf_strerror(status));
      return EXIT_FAILURE;
  } else  {
    fprintf(stdout, "bladerf_config_run_board: open device: %s\n", bladerf_strerror(status));
  }
  
  status = bladerf_is_fpga_configured(dev);
  if (status < 0) {
      fprintf(stderr, "bladerf_config_run_board: Failed to determine FPGA state: %s\n",
              bladerf_strerror(status));
      return EXIT_FAILURE;
  } else if (status == 0) {
      fprintf(stderr, "bladerf_config_run_board: Error: FPGA is not loaded.\n");
      bladerf_close(dev);
      return EXIT_FAILURE;
  } else  {
    fprintf(stdout, "bladerf_config_run_board: FPGA is loaded.\n");
  }
  
  status = bladerf_set_frequency(dev, BLADERF_MODULE_RX, freq_hz);
  if (status != 0) {
      fprintf(stderr, "bladerf_config_run_board: Failed to set frequency: %s\n",
              bladerf_strerror(status));
      bladerf_close(dev);
      return EXIT_FAILURE;
  } else {
      fprintf(stdout, "bladerf_config_run_board: set frequency: %luHz %s\n", freq_hz,
              bladerf_strerror(status));
  }

  status = bladerf_set_sample_rate(dev, BLADERF_MODULE_RX, SAMPLE_PER_SYMBOL*1000000ul, &actual);
  if (status != 0) {
      fprintf(stderr, "bladerf_config_run_board: Failed to set sample rate: %s\n",
              bladerf_strerror(status));
      bladerf_close(dev);
      return EXIT_FAILURE;
  } else {
    fprintf(stdout, "bladerf_config_run_board: set sample rate: %dHz %s\n", actual,
              bladerf_strerror(status));
  }
  
  status = bladerf_set_bandwidth(dev, BLADERF_MODULE_RX, SAMPLE_PER_SYMBOL*1000000ul/2, &actual);
  if (status != 0) {
      fprintf(stderr, "bladerf_config_run_board: Failed to set bandwidth: %s\n",
              bladerf_strerror(status));
      bladerf_close(dev);
      return EXIT_FAILURE;
  } else {
    fprintf(stdout, "bladerf_config_run_board: bladerf_set_bandwidth: %d %s\n", actual,
              bladerf_strerror(status));
  }
  
  status = bladerf_set_gain(dev, BLADERF_MODULE_RX, gain);
  if (status != 0) {
      fprintf(stderr, "bladerf_config_run_board: Failed to set gain: %s\n",
              bladerf_strerror(status));
      bladerf_close(dev);
      return EXIT_FAILURE;
  } else {
    fprintf(stdout, "bladerf_config_run_board: bladerf_set_gain: %d %s\n", gain,
              bladerf_strerror(status));
  }

#if 0 // old version do not have this API
  status = bladerf_get_gain(dev, BLADERF_MODULE_RX, &actual);
  if (status != 0) {
      fprintf(stderr, "Failed to get gain: %s\n",
              bladerf_strerror(status));
      bladerf_close(dev);
      return EXIT_FAILURE;
  } else {
    fprintf(stdout, "bladerf_get_gain: %d %s\n", actual,
              bladerf_strerror(status));
  }
#endif

  /* Initialize the stream */
  status = bladerf_init_stream(
              &bladerf_rx_stream,
              dev,
              bladerf_stream_callback,
              &bladerf_rx_data.buffers,
              bladerf_rx_data.num_buffers,
              BLADERF_FORMAT_SC16_Q11,
              bladerf_rx_data.samples_per_buffer,
              bladerf_rx_data.num_buffers,
              &bladerf_rx_data
            );

  if (status != 0) {
      fprintf(stderr, "bladerf_config_run_board: Failed to init stream: %s\n",
              bladerf_strerror(status));
      bladerf_close(dev);
      return EXIT_FAILURE;
  } else {
    fprintf(stdout, "bladerf_config_run_board: init stream: %s\n",
              bladerf_strerror(status));
  }

  bladerf_set_stream_timeout(dev, BLADERF_MODULE_RX, 100);

  status = bladerf_enable_module(dev, BLADERF_MODULE_RX, true);
  if (status < 0) {
      fprintf(stderr, "bladerf_config_run_board: Failed to enable module: %s\n",
              bladerf_strerror(status));
      bladerf_deinit_stream(bladerf_rx_stream);
      bladerf_close(dev);
      return EXIT_FAILURE;
  } else {
    fprintf(stdout, "bladerf_config_run_board: enable module true: %s\n",
              bladerf_strerror(status));
  }

  status = pthread_create(&bladerf_rx_task, NULL, bladerf_rx_task_run, NULL);
  if (status < 0) {
      return EXIT_FAILURE;
  }

  (*rf_dev) = dev;
  return(0);
}

void bladerf_stop_close_board(void *dev){
  int status;

  fprintf(stderr, "bladerf_stop_close_board...\n");
  
  pthread_join(bladerf_rx_task, NULL);
  //pthread_cancel(async_task.rx_task);
  printf("bladerf_stop_close_board: bladeRF rx thread quit.\n");

  if (dev==NULL)
    return;

  bladerf_deinit_stream(bladerf_rx_stream);
  printf("bladerf_deinit_stream.\n");

  status = bladerf_enable_module((struct bladerf *)dev, BLADERF_MODULE_RX, false);
  if (status < 0) {
      fprintf(stderr, "Failed to enable module: %s\n",
              bladerf_strerror(status));
  } else {
    fprintf(stdout, "enable module false: %s\n", bladerf_strerror(status));
  }

  bladerf_close((struct bladerf *)dev);
  printf("bladerf_close.\n");
}

#endif
