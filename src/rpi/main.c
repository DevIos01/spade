#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include "hardware/spi.h"
#include "hardware/timer.h"
#include "hardware/watchdog.h"
#include "hardware/adc.h"
#include "pico/util/queue.h"
#include "pico/multicore.h"
#include "jerryscript.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Set to false to enable debug prints for development (this is janky)
#if true
#define dbg puts
#define dbgf printf
#else
#define dbg(...) ;
#define dbgf(...) ;
#endif

// Debugging shortcut
#define yell puts

#ifdef SPADE_AUDIO
#include "audio.c"
#endif

// More firmware stuiff
#include "ST7735_TFT.h"
#include "upload.h"

// Other imports
#include "shared/sprig_engine/base_engine.c"
#include "shared/sprig_engine/module_native.c"
#include "shared/js_runtime/jerry_mem.h"
#include "shared/js_runtime/jerryxx.c"
#include "shared/js_runtime/js.h"

// Externs for shared/ui/errorbuf.h
char errorbuf[512] = "";
Color errorbuf_color; // Initialized in main()
static void fatal_error()
{
  // On fatal error, start an infinite loop rendering the errorbuf.
  errorbuf_color = color16(255, 0, 0); // Make sure it's red
  while (1)
  {
    text_clear();
    render_errorbuf();
    st7735_fill_start();
    render(st7735_fill_send);
    st7735_fill_finish();
  }
}
#include "shared/ui/errorbuf.h"

#define ARR_LEN(arr) (sizeof(arr) / sizeof(arr[0]))

/**
 * We store a 64-boolean ringbuffer of polled button states for a primitive
 * sort of debouncing. The button counts as pressed if more than 5/6th of
 * the ringbuffer is true.
 *
 * (gpio_set_input_hysteresis_enabled was too slow.)
 */
#define HISTORY_LEN (64)
#define MAX_TEST_CASES 10
typedef struct
{
  uint8_t history[HISTORY_LEN / 8];
  uint8_t last_state;
  uint8_t ring_i;
} ButtonState;
uint button_pins[] = {5, 7, 6, 8, 12, 14, 13, 15};
static ButtonState button_states[ARR_LEN(button_pins)] = {0};

static bool button_history_read(ButtonState *bs, int i)
{
  // We want to store bools compactly so we have to do some bit twiddling.
  int q = 1 << (i % 8);
  return !!(bs->history[i / 8] & q);
}
static void button_history_write(ButtonState *bs, int i, bool value)
{
  if (value)
    bs->history[i / 8] |= 1 << (i % 8);
  else
    bs->history[i / 8] &= ~(1 << (i % 8));
}

static void button_init(void)
{
  for (int i = 0; i < ARR_LEN(button_pins); i++)
  {
    ButtonState *bs = button_states + i;
    gpio_set_dir(button_pins[i], GPIO_IN);
    gpio_pull_up(button_pins[i]);
  }
}

/**
 * Poll the buttons and push any keypresses to the main core.
 *
 * (Should be run in a loop on a non-primary core.)
 */
static void button_poll(void)
{
  for (int i = 0; i < ARR_LEN(button_pins); i++)
  {
    ButtonState *bs = button_states + i;

    bs->ring_i = (bs->ring_i + 1) % HISTORY_LEN; // Incrememnt ringbuffer index
    button_history_write(bs, bs->ring_i, gpio_get(button_pins[i]));

    // up is true if more than 5/6 are true
    int up = 0;
    for (int i = 0; i < HISTORY_LEN; i++)
    {
      up += button_history_read(bs, i);
    }
    up = up > ((HISTORY_LEN * 5) / 6); // Here we convert to a bool

    if (up != bs->last_state)
    {
      bs->last_state = up;
      if (!up)
      {
        // Send the keypress to the main core
        multicore_fifo_push_blocking(button_pins[i]);
      }
    }
  }
}

// Turn on the power lights and dim them with PWM.
static void power_lights()
{
  // left white light
  const int pin_num_0 = 28;
  gpio_set_function(pin_num_0, GPIO_FUNC_PWM);
  uint slice_num_0 = pwm_gpio_to_slice_num(pin_num_0);
  pwm_set_enabled(slice_num_0, true);
  pwm_set_gpio_level(pin_num_0, 65535 / 8);

  // right blue light
  // const pin_num_1 = 4;
  // gpio_set_function(pin_num_1, GPIO_FUNC_PWM);
  // uint slice_num_1 = pwm_gpio_to_slice_num(pin_num_1);
  // pwm_set_enabled(slice_num_1, true);
  // pwm_set_gpio_level(pin_num_1, 65535/4);
}

// Entry point for the second core that polls the buttons.
static void core1_entry(void)
{
  button_init();

  while (1)
  {
    button_poll();
  }
}

/**
 * Seed the random number generator with entropy from
 * random electricity as well as temperature readings.
 */
static void rng_init(void)
{
  adc_init();
  uint32_t seed = 0;

  // Read some random electricity
  for (int i = 0; i < 4; i++)
  {
    adc_select_input(4);
    sleep_ms(1);
    seed ^= adc_read();
  }

  // Read some temperature data
  adc_set_temp_sensor_enabled(true);
  adc_select_input(4);
  sleep_ms(1);
  seed ^= adc_read();
  adc_set_temp_sensor_enabled(false);

  srand(seed);
}

// Wait for a game to be uploaded.
static int load_new_scripts(void)
{
  return upl_stdin_read();
}

/**
 * Implementations for PianoOpts (see src/shared/audio/piano.h)
 *
 * p (the song object) is type erased because that's an implementation detail
 * for us. It's actually a jerry_value_t, not a void pointer, so we gotta cast.
 */
#ifdef SPADE_AUDIO
void piano_jerry_song_free(void *p)
{
  jerry_value_t jvt = (jerry_value_t)p;
  jerry_release_value(jvt);
}

int piano_jerry_song_chars(void *p, char *buf, int buf_len)
{
  jerry_value_t jvt = (jerry_value_t)p;
  int read = jerry_string_to_char_buffer(jvt, (jerry_char_t *)buf, (jerry_size_t)buf_len);
  return read;
}
#endif

typedef enum
{
  TEST_CASE_IDLE,
  TEST_CASE_RUNNING,
  TEST_CASE_DELAY
} TestCaseState;

TestCaseState testCaseState = TEST_CASE_IDLE;
int currentTestCaseIndex = 0;
uint32_t testCaseStartTime = 0;
const int testCaseDelayMs = 5000;

bool containsTestCases(const char *script)
{
  const char *delimiter = "/* -------------------------TEST------------------- */";
  return strstr(script, delimiter) != NULL;
}

void splitIntoTestCases(const char *script, char **testCases, int *testCaseCount)
{
  const char *delimiter = "/* -------------------------TEST------------------- */";
  const char *start = script;
  const char *end;
  *testCaseCount = 0;

  printf("Debug: Starting to split test cases\n");

  while ((end = strstr(start, delimiter)) != NULL && *testCaseCount < MAX_TEST_CASES)
  {
    int length = end - start;
    if (length > 0)
    {
      testCases[*testCaseCount] = (char *)malloc(length + 1);
      if (testCases[*testCaseCount] != NULL)
      {
        strncpy(testCases[*testCaseCount], start, length);
        testCases[*testCaseCount][length] = '\0';
        printf("Debug: Test Case %d extracted, length: %d\n", *testCaseCount, length);
        (*testCaseCount)++;
      }
    }
    start = end + strlen(delimiter);
  }

  if (*start != '\0' && *testCaseCount < MAX_TEST_CASES)
  {
    int length = strlen(start);
    testCases[*testCaseCount] = (char *)malloc(length + 1);
    if (testCases[*testCaseCount] != NULL)
    {
      strncpy(testCases[*testCaseCount], start, length);
      testCases[*testCaseCount][length] = '\0';
      printf("Debug: Final Test Case extracted, length: %d\n", length);
      (*testCaseCount)++;
    }
  }

  printf("Debug: Total Test Cases: %d\n", *testCaseCount);
}

void runTestCase(const char *testCase)
{
  printf("Running Test Case %d...\n", currentTestCaseIndex);
  js_run(testCase, strlen(testCase));
  testCaseState = TEST_CASE_RUNNING;
  printf("Test Case %d execution started.\n", currentTestCaseIndex);
}

void processTestCases(char **testCases, int testCaseCount)
{
  printf("Processing Test Cases... Current index: %d\n", currentTestCaseIndex);
  if (testCaseState == TEST_CASE_RUNNING && (to_ms_since_boot(get_absolute_time()) - testCaseStartTime >= testCaseDelayMs))
  {
    printf("Test Case %d completed. Moving to next.\n", currentTestCaseIndex);
    testCaseState = TEST_CASE_IDLE;
    currentTestCaseIndex++;
  }

  if (testCaseState == TEST_CASE_IDLE && currentTestCaseIndex < testCaseCount)
  {
    runTestCase(testCases[currentTestCaseIndex]);
    testCaseStartTime = to_ms_since_boot(get_absolute_time());
  }
  else if (currentTestCaseIndex >= testCaseCount)
  {
    printf("All Test Cases completed.\n");
    testCaseState = TEST_CASE_IDLE;
    currentTestCaseIndex = 0;
  }
}

int main()
{
  // Overclock the RP2040!
  set_sys_clock_khz(270000, true);

  errorbuf_color = color16(0, 255, 255); // cyan

  power_lights();   // Turn on the power lights
  stdio_init_all(); // Init serial port
  st7735_init();    // Init display
  rng_init();       // Init RNG

  // Init JerryScript
  jerry_init(JERRY_INIT_MEM_STATS);
  init(sprite_free_jerry_object); // TODO: document

  while (!save_read())
  {
    // No game stored in memory
    strcpy(errorbuf, "                    \n"
                     "                    \n"
                     "                    \n"
                     "                    \n"
                     "                    \n"
                     "                    \n"
                     "                    \n"
                     "    PLEASE UPLOAD   \n"
                     "       A GAME       \n"
                     "                    \n"
                     "                    \n"
                     "                    \n"
                     "                    \n"
                     "                    \n"
                     " sprig.hackclub.com \n");
    render_errorbuf();
    st7735_fill_start();
    render(st7735_fill_send);
    st7735_fill_finish();

    load_new_scripts();
  }

  // Start a core to listen for keypresses.
  multicore_launch_core1(core1_entry);

  /**
   * We get a bunch of fake keypresses at startup, so we need to
   * drain them from the FIFO queue.
   *
   * What really needs to be done here is to have button_init
   * record when it starts so that we can ignore keypresses after
   * that timestamp.
   */
  sleep_ms(50);
  while (multicore_fifo_rvalid())
    multicore_fifo_pop_blocking();

  /**
   * Wait for a keypress to start the game.
   *
   * This is important so games with e.g. infinite loops don't
   * brick the device as soon as they start up.
   */
  while (!multicore_fifo_rvalid())
  {
    strcpy(errorbuf, "                    \n"
                     "                    \n"
                     "                    \n"
                     "                    \n"
                     "                    \n"
                     "                    \n"
                     "                    \n"
                     "    PRESS ANY KEY   \n"
                     "       TO RUN       \n"
                     "                    \n"
                     "                    \n"
                     "                    \n"
                     "                    \n"
                     "                    \n"
                     " sprig.hackclub.com \n");
    render_errorbuf();
    st7735_fill_start();
    render(st7735_fill_send);
    st7735_fill_finish();

    load_new_scripts();
  }

  // Wow, we can actually run a game now!

  // Clear the errorbuf and make it red
  memset(errorbuf, 0, sizeof(errorbuf));
  errorbuf_color = color16(255, 0, 0);
  text_clear();

  // Drain any remaining keypresses
  while (multicore_fifo_rvalid())
    multicore_fifo_pop_blocking();

  const char *script = save_read();
  char *testCases[MAX_TEST_CASES];
  int testCaseCount = 0;

  if (containsTestCases(script))
  {
    splitIntoTestCases(script, testCases, &testCaseCount);
    printf("Found %d test cases.\n", testCaseCount);
  }
  else
  {
    printf("No test cases found. Running entire script.\n");
    js_run(script, strlen(script));
  }

#ifdef SPADE_AUDIO
  // Initialize audio
  piano_init((PianoOpts){
      .song_free = piano_jerry_song_free,
      .song_chars = piano_jerry_song_chars,
  });
  audio_init();
#endif

  // Current time for timer handling (see frame_cb in shared/sprig_engine/engine.js)
  absolute_time_t last = get_absolute_time();
  dbg("okay launching game loop");

  // Event loop!
  while (1)
  {

    // Handle any new button presses
    while (multicore_fifo_rvalid())
    {
      spade_call_press(multicore_fifo_pop_blocking());
    }

    if (testCaseCount > 0)
    {
      processTestCases(testCases, testCaseCount);
    }

    // Run async code
    js_promises();

    // setTimeout/setInterval impl
    absolute_time_t now = get_absolute_time();
    int elapsed = us_to_ms(absolute_time_diff_us(last, now));
    last = now;
    spade_call_frame(elapsed);

#ifdef SPADE_AUDIO
    // Get any audio to the speaker
    audio_try_push_samples();
#endif

    // Render
    render_errorbuf();
    st7735_fill_start();
    render(st7735_fill_send);
    st7735_fill_finish();

    if (load_new_scripts())
      break;
  }

  /**
   * User uploaded a new game mid-game. We're gonna try to reboot here,
   * but just in case it doesn't work, we'll print a message nicely
   * asking them to reboot. They'll never know there was a bug!
   *
   * (Unless they read this code. Which is encouraged. shhhhhhhhhhh~)
   */

  errorbuf_color = color16(50, 205, 50); // lime green
  strcpy(errorbuf, "                    \n"
                   "                    \n"
                   "                    \n"
                   "                    \n"
                   "                    \n"
                   "                    \n"
                   "                    \n"
                   "    PLEASE REBOOT   \n"
                   "     YOUR SPRIG     \n"
                   "                    \n"
                   "                    \n"
                   "                    \n"
                   "                    \n"
                   "                    \n"
                   " sprig.hackclub.com \n");
  render_errorbuf();
  st7735_fill_start();
  render(st7735_fill_send);
  st7735_fill_finish();

  /**
   * Watchdog is a mechanism designed to catch infinite loops. It will
   * automatically reboot the device if another function, watchdog_update()
   * is not called rapidly enough.
   *
   * Enabling watchdog with a timeout of 0 will cause the Pico to reboot
   * right away.
   */
  watchdog_enable(0, false);
  while (1)
  {
  }
}