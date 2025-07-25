/*
 * GStreamer Vosk plugin
 * Copyright (C) 2022 Philippe Rouquier <bonfire-app@wanadoo.fr>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <libintl.h>
#include <locale.h>

#include <glib.h>
#include <gio/gio.h>
#include <gst/gst.h>
#ifdef HAVE_RNNOISE
#include <rnnoise.h>
#include <math.h>
#endif
#include "gstvosk.h"
#include "vosk-api.h"
#include "../gst-vosk-config.h"

GST_DEBUG_CATEGORY_STATIC (gst_vosk_debug);
#define GST_CAT_DEFAULT gst_vosk_debug

#define DEFAULT_SPEECH_MODEL "/usr/share/vosk/model"
#ifdef HAVE_RNNOISE
#define DEFAULT_ENABLE_DENOISE TRUE
#define RNNOISE_FRAME_SIZE 480  // RNNoise frame size (10ms at 48kHz)
#define NOISE_ESTIMATION_FRAMES 50
#endif
#define DEFAULT_ALTERNATIVE_NUM 0

#define _(STRING) gettext(STRING)

#define GST_VOSK_LOCK(vosk) (g_mutex_lock(&vosk->RecMut))
#define GST_VOSK_UNLOCK(vosk) (g_mutex_unlock(&vosk->RecMut))

enum
{
  RESULT,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = {0};

enum
{
  PROP_0,
  PROP_USE_SIGNALS,
  PROP_SPEECH_MODEL,
#ifdef HAVE_RNNOISE
  PROP_ENABLE_DENOISE,
#endif
  PROP_ALTERNATIVES,
  PROP_CURRENT_FINAL_RESULTS,
  PROP_CURRENT_RESULTS,
  PROP_PARTIAL_RESULTS_INTERVAL,
};

/*
 * gst-launch-1.0 -m pulsesrc  buffer-time=9223372036854775807 ! \
 *                   audio/x-raw,format=S16LE,rate=16000, channels=1 ! \
 *                   vosk speech-model=path/to/model ! fakesink
*/

#define VOSK_EMPTY_PARTIAL_RESULT  "{\n  \"partial\" : \"\"\n}"
#define VOSK_EMPTY_TEXT_RESULT     "{\n  \"text\" : \"\"\n}"
#define VOSK_EMPTY_TEXT_RESULT_ALT "{\"text\": \"\"}"

/* BUG : protect from local formatting errors when fr_ prefix
   Maybe there are other locales ?
   Use uselocale () when we can as it is supposed to be safer since it sets
   the locale only for the thread. */
#if HAVE_USELOCALE

#define PROTECT_FROM_LOCALE_BUG_START                         \
  locale_t current_locale;                                    \
  locale_t new_locale;                                        \
  locale_t old_locale = NULL;                                 \
                                                              \
  current_locale = uselocale (NULL);                          \
  old_locale = duplocale (current_locale);                    \
  new_locale = newlocale (LC_NUMERIC_MASK, "C", old_locale);  \
  if (new_locale)                                             \
    uselocale (new_locale);

#else

#define PROTECT_FROM_LOCALE_BUG_START                         \
  gchar *saved_locale = NULL;                                 \
  const gchar *current_locale;                                \
  current_locale = setlocale(LC_NUMERIC, NULL);               \
  if (current_locale != NULL &&                               \
      g_str_has_prefix (current_locale, "fr_") == TRUE) {     \
    saved_locale = g_strdup (current_locale);                 \
    setlocale (LC_NUMERIC, "C");                              \
    GST_LOG_OBJECT (vosk, "Changed locale %s", saved_locale); \
  }

#endif

#if HAVE_USELOCALE

#define PROTECT_FROM_LOCALE_BUG_END                           \
  if (old_locale) {                                           \
    uselocale (current_locale);                               \
    freelocale (new_locale);                                  \
  }

#else

#define PROTECT_FROM_LOCALE_BUG_END                           \
  if (saved_locale != NULL) {                                 \
    setlocale (LC_NUMERIC, saved_locale);                     \
    GST_LOG_OBJECT (vosk, "Reset locale %s", saved_locale);   \
    g_free (saved_locale);                                    \
  }

#endif

/* the capabilities of the inputs and outputs. */
static GstStaticPadTemplate sink_factory = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/x-raw,"
                     "format=S16LE,"
                     "rate=[1, MAX],"
                     "channels=1")
    );

static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/x-raw,"
                     "format=S16LE,"
                     "rate=[1, MAX],"
                     "channels=1")
    );

#define gst_vosk_parent_class parent_class
G_DEFINE_TYPE (GstVosk, gst_vosk, GST_TYPE_ELEMENT);

static void
gst_vosk_set_property (GObject * object, guint prop_id,
                       const GValue * value, GParamSpec * pspec);
static void
gst_vosk_get_property (GObject * object, guint prop_id,
                       GValue * value, GParamSpec * pspec);

static GstStateChangeReturn
gst_vosk_change_state (GstElement *element,
                       GstStateChange transition);

static gboolean
gst_vosk_sink_event (GstPad * pad, GstObject * parent, GstEvent * event);

static GstFlowReturn
gst_vosk_chain (GstPad * pad, GstObject * parent, GstBuffer * buf);

static void
gst_vosk_load_model_async (gpointer thread_data,
                           gpointer element);

/* Note : audio rate is handled by the application with the use of caps */

static void
gst_vosk_finalize (GObject *object)
{
  GstVosk *vosk = GST_VOSK (object);
#ifdef HAVE_RNNOISE
  // Free RNNoise state
  if (vosk->denoise_state) {
    rnnoise_destroy(vosk->denoise_state);
    vosk->denoise_state = NULL;
  }

  // Free denoise buffers
  if (vosk->denoise_input_buffer) {
    g_free(vosk->denoise_input_buffer);
    vosk->denoise_input_buffer = NULL;
  }

  if (vosk->denoise_output_buffer) {
        g_free(vosk->denoise_output_buffer);
        vosk->denoise_output_buffer = NULL;
    }

  g_mutex_clear(&vosk->denoise_mutex);
#endif

  if (vosk->model_path) {
    g_free (vosk->model_path);
    vosk->model_path = NULL;
  }

  g_thread_pool_free(vosk->thread_pool, TRUE, TRUE);
  vosk->thread_pool=NULL;

  GST_DEBUG_OBJECT (vosk, "finalizing.");
}

static void
gst_vosk_class_init (GstVoskClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;

  gobject_class->set_property = gst_vosk_set_property;
  gobject_class->get_property = gst_vosk_get_property;
  gobject_class->finalize = gst_vosk_finalize;

  g_object_class_install_property (gobject_class, PROP_USE_SIGNALS,
      g_param_spec_boolean ("use-signals", _("Emit GObject signals"), _("Use GObject signals instead of Gstreamer messages to pass the results of recognition"),
          FALSE, G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class, PROP_SPEECH_MODEL,
      g_param_spec_string ("speech-model", _("Speech Model"), _("Location (path) of the speech model"),
          DEFAULT_SPEECH_MODEL, G_PARAM_READWRITE|GST_PARAM_MUTABLE_READY));
#ifdef HAVE_RNNOISE
  g_object_class_install_property (gobject_class, PROP_ENABLE_DENOISE,
      g_param_spec_boolean ("enable-denoise", _("Enable Noise Reduction"), _("Enable RNNoise-based noise reduction"),
          DEFAULT_ENABLE_DENOISE, G_PARAM_READWRITE|GST_PARAM_MUTABLE_READY));
#endif
  g_object_class_install_property (gobject_class, PROP_ALTERNATIVES,
      g_param_spec_int ("alternatives", _("Alternative Number"), _("Number of alternative results returned"),
          0, 100, DEFAULT_ALTERNATIVE_NUM, G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class, PROP_CURRENT_FINAL_RESULTS,
      g_param_spec_string ("current-final-results", _("Get recognizer's current final results"), _("Force the recognizer to return final results"),
          NULL, G_PARAM_READABLE));

  g_object_class_install_property (gobject_class, PROP_CURRENT_RESULTS,
      g_param_spec_string ("current-results", _("Get recognizer's current results"), _("Force the recognizer to return results"),
          NULL, G_PARAM_READABLE));

  g_object_class_install_property (gobject_class, PROP_PARTIAL_RESULTS_INTERVAL,
      g_param_spec_int64 ("partial-results-interval", _("Minimum time interval between partial results"), _("Set the minimum time interval between partial results (in milliseconds). Set -1 to disable partial results"),
          -1,G_MAXINT64, 0, G_PARAM_READWRITE));

  signals[RESULT] =
    g_signal_new ("result",
                  G_OBJECT_CLASS_TYPE (gobject_class),
                  G_SIGNAL_RUN_LAST | G_SIGNAL_NO_RECURSE,
                  0, NULL, NULL,
                  g_cclosure_marshal_VOID__STRING,
                  G_TYPE_NONE,
                  1,
                  G_TYPE_STRING);

  gst_element_class_set_details_simple(gstelement_class,
    "vosk",
    "Filter/Audio",
    _("Performs speech recognition using libvosk"),
    "Philippe Rouquier <bonfire-app@wanadoo.fr>");

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&src_factory));
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&sink_factory));

  gstelement_class->change_state = gst_vosk_change_state;
}

static void
gst_vosk_init (GstVosk * vosk)
{
  vosk->sinkpad = gst_pad_new_from_static_template (&sink_factory, "sink");
  gst_pad_set_event_function (vosk->sinkpad,
                              GST_DEBUG_FUNCPTR(gst_vosk_sink_event));
  gst_pad_set_chain_function (vosk->sinkpad,
                              GST_DEBUG_FUNCPTR(gst_vosk_chain));
  GST_PAD_SET_PROXY_CAPS (vosk->sinkpad);
  gst_element_add_pad (GST_ELEMENT (vosk), vosk->sinkpad);

  vosk->srcpad = gst_pad_new_from_static_template (&src_factory, "src");
  GST_PAD_SET_PROXY_CAPS (vosk->srcpad);
  gst_element_add_pad (GST_ELEMENT (vosk), vosk->srcpad);

  if (!gst_debug_is_active())
    vosk_set_log_level (-1);

  vosk->rate = 0.0;
  vosk->alternatives = DEFAULT_ALTERNATIVE_NUM;
  vosk->model_path = g_strdup(DEFAULT_SPEECH_MODEL);
#ifdef HAVE_RNNOISE
  vosk->enable_denoise = DEFAULT_ENABLE_DENOISE;
  vosk->denoise_state = NULL;
  vosk->denoise_input_buffer = NULL;
  vosk->denoise_buffer_size = 0;
  vosk->denoise_buffer_pos = 0;
  vosk->denoise_initialized = FALSE;

  g_mutex_init(&vosk->denoise_mutex);

  vosk->denoise_output_buffer = NULL;
  vosk->denoise_output_pos = 0;
#endif
  vosk->thread_pool=g_thread_pool_new((GFunc) gst_vosk_load_model_async,
                                      vosk,
                                      1,
                                      FALSE,
                                      NULL);
}

static void
gst_vosk_reset (GstVosk *vosk)
{
  if (vosk->recognizer) {
    vosk_recognizer_free (vosk->recognizer);
    vosk->recognizer = NULL;
  }

  if (vosk->prev_partial) {
    g_free (vosk->prev_partial);
    vosk->prev_partial = NULL;
  }
#ifdef HAVE_RNNOISE
  // Reset denoise state
  g_mutex_lock(&vosk->denoise_mutex);
  if (vosk->denoise_input_buffer) {
    vosk->denoise_buffer_pos = 0;
    memset(vosk->denoise_input_buffer, 0, vosk->denoise_buffer_size * sizeof(gfloat));
  }
  vosk->denoise_initialized = FALSE;
  g_mutex_unlock(&vosk->denoise_mutex);

  GST_DEBUG_OBJECT(vosk, "Vosk reset completed");
#endif

  vosk->last_processed_time=GST_CLOCK_TIME_NONE;
  vosk->rate=0.0;
}

static void
gst_vosk_cancel_model_loading(GstVosk *vosk)
{
  GST_VOSK_LOCK(vosk);
  if (vosk->current_operation) {
    g_cancellable_cancel(vosk->current_operation);
    g_object_unref(vosk->current_operation);
    vosk->current_operation=NULL;
  }
  GST_VOSK_UNLOCK(vosk);
}

static gint
gst_vosk_get_rate(GstVosk *vosk)
{
  GstStructure *caps_struct;
  GstCaps *caps;
  gint rate = 0;

  caps=gst_pad_get_current_caps(vosk->sinkpad);
  if (caps == NULL) {
    GST_INFO_OBJECT (vosk, "no capabilities set on sink pad.");
    return 0;
  }

  caps_struct = gst_caps_get_structure (caps, 0);
  if (caps_struct == NULL) {
    GST_INFO_OBJECT (vosk, "no capabilities structure.");
    return 0;
  }

  if (gst_structure_get_int (caps_struct, "rate", &rate) == FALSE) {
    GST_INFO_OBJECT (vosk, "no rate set in the capabilities");
    return 0;
  }

  return rate;
}

#ifdef HAVE_RNNOISE
static gboolean
gst_vosk_init_denoise(GstVosk *vosk)
{
  gint rate;

  if (!vosk->enable_denoise || vosk->denoise_initialized)
    return TRUE;

  rate = gst_vosk_get_rate(vosk);
  if (rate <= 0) {
    GST_DEBUG_OBJECT(vosk, "Sample rate not available, deferring denoise initialization");
    return FALSE;
  }

  g_mutex_lock(&vosk->denoise_mutex);

  // Clean up existing state
  if (vosk->denoise_state) {
    rnnoise_destroy(vosk->denoise_state);
    vosk->denoise_state = NULL;
  }

  if (vosk->denoise_input_buffer) {
    g_free(vosk->denoise_input_buffer);
    vosk->denoise_input_buffer = NULL;
  }

  // Create RNNoise state
  vosk->denoise_state = rnnoise_create(NULL);
  if (!vosk->denoise_state) {
    GST_WARNING_OBJECT(vosk, "Failed to create RNNoise state");
    g_mutex_unlock(&vosk->denoise_mutex);
    return FALSE;
  }

  // Allocate input buffer for accumulating samples
  vosk->denoise_buffer_size = RNNOISE_FRAME_SIZE * 2; // Buffer 2 frames
  vosk->denoise_input_buffer = g_malloc0(vosk->denoise_buffer_size * sizeof(gfloat));
  vosk->denoise_buffer_pos = 0;

  // Allocate output buffer for storing processed frames
  vosk->denoise_output_buffer = g_malloc0(vosk->denoise_buffer_size * sizeof(gfloat));
  vosk->denoise_output_pos = 0;
  vosk->denoise_initialized = TRUE;

  g_mutex_unlock(&vosk->denoise_mutex);

  GST_INFO_OBJECT(vosk, "RNNoise initialized successfully (input rate: %d Hz)", rate);
  return TRUE;
}

static void
gst_vosk_process_denoise_frame(GstVosk *vosk, gfloat *frame_data)
{
  if (!vosk->denoise_state || !vosk->denoise_initialized)
    return;

  rnnoise_process_frame(vosk->denoise_state, frame_data, frame_data);
}

static void
gst_vosk_convert_s16_to_float(gint16 *input, gfloat *output, gsize samples)
{
  for (gsize i = 0; i < samples; i++) {
    output[i] = (gfloat)input[i] ;
  }
}

static void
gst_vosk_convert_float_to_s16(gfloat *input, gint16 *output, gsize samples)
{
  for (gsize i = 0; i < samples; i++) {
    gfloat sample = input[i] ;
    output[i] = (gint16)CLAMP(sample, -32768.0f, 32767.0f);
  }
}

static void
gst_vosk_apply_denoise(GstVosk *vosk, GstMapInfo *info)
{
    if (!vosk->enable_denoise || !vosk->denoise_initialized) {
        return;
    }

    g_mutex_lock(&vosk->denoise_mutex);

    if (vosk->rate != 48000) {
        g_mutex_unlock(&vosk->denoise_mutex);
        return;
    }

    gint16 *s16_buffer = (gint16 *)info->data;
    gsize s16_buffer_samples = info->size / sizeof(gint16);

    gsize samples_read = 0;
    while (samples_read < s16_buffer_samples) {
        gsize to_copy = MIN(RNNOISE_FRAME_SIZE - vosk->denoise_buffer_pos, 
                            s16_buffer_samples - samples_read);

        gst_vosk_convert_s16_to_float(s16_buffer + samples_read,
                                      vosk->denoise_input_buffer + vosk->denoise_buffer_pos,
                                      to_copy);
        samples_read += to_copy;
        vosk->denoise_buffer_pos += to_copy;

        // If we have a full frame, denoise it and move it to the output buffer
        if (vosk->denoise_buffer_pos == RNNOISE_FRAME_SIZE) {
            gst_vosk_process_denoise_frame(vosk, vosk->denoise_input_buffer);

            if ((vosk->denoise_output_pos + RNNOISE_FRAME_SIZE) <= vosk->denoise_buffer_size) {
                // Append the clean frame to our "ready" output buffer
                memcpy(vosk->denoise_output_buffer + vosk->denoise_output_pos,
                       vosk->denoise_input_buffer,
                       RNNOISE_FRAME_SIZE * sizeof(gfloat));
                vosk->denoise_output_pos += RNNOISE_FRAME_SIZE;
            } else {
                GST_WARNING_OBJECT(vosk, "Denoise output buffer is full. Dropping a processed frame to prevent memory corruption.");
            }

            vosk->denoise_buffer_pos = 0; // Reset input buffer
        }
    }

    gsize samples_to_write = MIN(vosk->denoise_output_pos, s16_buffer_samples);

    if (samples_to_write > 0) {
        gst_vosk_convert_float_to_s16(vosk->denoise_output_buffer,
                                      s16_buffer,
                                      samples_to_write);

        // Remove the data we just used from the output buffer by shifting the remainder
        vosk->denoise_output_pos -= samples_to_write;
        if (vosk->denoise_output_pos > 0) {
            memmove(vosk->denoise_output_buffer,
                    vosk->denoise_output_buffer + samples_to_write,
                    vosk->denoise_output_pos * sizeof(gfloat));
        }
    }

    // If we don't have enough clean audio to fill the whole buffer, fill the rest with silence.
    if (samples_to_write < s16_buffer_samples) {
        gsize silence_samples = s16_buffer_samples - samples_to_write;
        memset(s16_buffer + samples_to_write, 0, silence_samples * sizeof(gint16));
    }

    g_mutex_unlock(&vosk->denoise_mutex);
}
#endif

static gboolean
gst_vosk_recognizer_new (GstVosk *vosk, VoskModel *model)
{
  vosk->rate = gst_vosk_get_rate(vosk);
  if (vosk->rate <= 0.0) {
    GST_INFO_OBJECT (vosk, "rate not set yet: no recognizer created.");
    return FALSE;
  }

  GST_INFO_OBJECT (vosk, "current rate is %f", vosk->rate);

  if (model == NULL) {
    GST_INFO_OBJECT (vosk, "no model provided.");
    return FALSE;
  }
#ifdef HAVE_RNNOISE
  // Setup denoise after we have the sample rate
  if (vosk->enable_denoise) {
    gst_vosk_init_denoise(vosk);
  }
#endif
  GST_INFO_OBJECT (vosk, "creating recognizer (rate = %f).", vosk->rate);
  vosk->recognizer = vosk_recognizer_new (model, vosk->rate);
  vosk_recognizer_set_max_alternatives (vosk->recognizer, vosk->alternatives);
  return TRUE;
}

typedef struct {
  gchar *path;
  GCancellable *cancellable;
} GstVoskThreadData;

static void
gst_vosk_load_model_async (gpointer thread_data,
                           gpointer element)
{
  GstVoskThreadData *status = thread_data;
  GstVosk *vosk = GST_VOSK (element);
  GstMessage *message;
  VoskModel *model;

  /* There can be only one model loading at a time. Even when loading has been
   * cancelled for one model while it is waiting to be loaded.
   * In this latter case, wait for it to start, notice it was cancelled and
   * leave.*/

  /* Thread might have been cancelled while waiting, so check that */
  if (g_cancellable_is_cancelled (status->cancellable)) {
    GST_INFO_OBJECT (vosk, "model creation cancelled without even trying (%s).", status->path);
    /* Note: don't use condition, it's not our problem any more */
    goto clean;
  }

  GST_INFO_OBJECT (vosk, "creating model %s.", status->path);

  /* This is why we do all this. Depending on the model size it can take a long
   * time before it returns. */
  model = vosk_model_new (status->path);

  GST_VOSK_LOCK(vosk);

  /* This is a point of no return for loading model */
  g_object_unref(vosk->current_operation);
  vosk->current_operation=NULL;

  /* The next statement should be protected with a lock so that we are not
   * cancelled right after checking cancel status and before setting model.
   * Otherwise, a stale model could remain. */
  if (g_cancellable_is_cancelled (status->cancellable)) {
    GST_VOSK_UNLOCK(vosk);

    GST_INFO_OBJECT (vosk, "model creation cancelled (%s).", status->path);
    vosk_model_free (model);

    /* Note: don't use condition, not our problem anymore */
    goto clean;
  }

  /* Do this here to make sure the following is still relevant */
  if (!model) {
    GST_VOSK_UNLOCK(vosk);

    GST_ERROR_OBJECT(vosk, "could not create model object for %s.", status->path);
    GST_ELEMENT_ERROR(GST_ELEMENT(vosk),
                      RESOURCE,
                      NOT_FOUND,
                      ("model could not be loaded"),
                      ("an error was encountered while loading model (%s)", status->path));

    GST_STATE_LOCK(vosk);
    gst_element_abort_state (GST_ELEMENT(vosk));
    GST_STATE_UNLOCK(vosk);

    goto clean;
  }

  GST_INFO_OBJECT (vosk, "model ready (%s).", status->path);

  /* This is the only place where vosk->recognizer can be set and only one
   * thread at a time can do it. */
  gst_vosk_recognizer_new(vosk, model);
  /* Unreference the model (it does not destroy it) */
  vosk_model_free (model);

  GST_VOSK_UNLOCK(vosk);

  GST_INFO_OBJECT (vosk, "async state change successfully completed.");
  message = gst_message_new_async_done (GST_OBJECT_CAST (vosk),
                                        GST_CLOCK_TIME_NONE);
  gst_element_post_message (element, message);

  /* This is needed to change the state of the element (and the pipeline).*/
  GST_STATE_LOCK (element);
  gst_element_continue_state (element, GST_STATE_CHANGE_SUCCESS);
  GST_STATE_UNLOCK (element);

clean:

  g_cancellable_cancel(status->cancellable);
  g_object_unref(status->cancellable);
  g_free(status->path);
  g_free(status);
}

static GstStateChangeReturn
gst_vosk_check_model_path(GstElement *element)
{
  GstVosk *vosk = GST_VOSK (element);
  GstVoskThreadData *thread_data;
  GstMessage *message;

  if (!vosk->model_path) {
      GST_ELEMENT_ERROR(vosk,
                        RESOURCE,
                        NOT_FOUND,
                        ("model could not be loaded"),
                        ("there is not model set"));
    return GST_STATE_CHANGE_FAILURE;
  }

  GST_VOSK_LOCK(vosk);

  vosk->last_processed_time=GST_CLOCK_TIME_NONE;

  if(vosk->recognizer) {
    GST_VOSK_UNLOCK(vosk);
    return GST_STATE_CHANGE_SUCCESS;
  }

  /* Start loading a new model */
  vosk->current_operation=g_cancellable_new();

  GST_VOSK_UNLOCK(vosk);

  thread_data=g_new0(GstVoskThreadData, 1);
  thread_data->cancellable=g_object_ref(vosk->current_operation);
  thread_data->path=g_strdup(vosk->model_path);
  g_thread_pool_push(vosk->thread_pool,
                     thread_data,
                     NULL);

  message = gst_message_new_async_start (GST_OBJECT_CAST (element));
  gst_element_post_message (element, message);
  return GST_STATE_CHANGE_ASYNC;
}

static GstStateChangeReturn
gst_vosk_change_state (GstElement *element, GstStateChange transition)
{
  GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;
  GstVosk *vosk = GST_VOSK (element);

  GST_INFO_OBJECT (vosk, "State changed %s", gst_state_change_get_name(transition));
  switch (transition) {
    case GST_STATE_CHANGE_READY_TO_PAUSED:
    case GST_STATE_CHANGE_PAUSED_TO_PAUSED:
      ret=gst_vosk_check_model_path(element);
      if (ret == GST_STATE_CHANGE_FAILURE)
        return GST_STATE_CHANGE_FAILURE;
      break;

    default:
      break;
  }

  if (GST_ELEMENT_CLASS (parent_class)->change_state (element, transition) == GST_STATE_CHANGE_FAILURE){
    GST_DEBUG_OBJECT (vosk, "State change failure");
    return GST_STATE_CHANGE_FAILURE;
  }

  switch (transition) {
    case GST_STATE_CHANGE_READY_TO_READY:
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      gst_vosk_cancel_model_loading(vosk);

      /* Take the stream lock and wait for it to end */
      GST_PAD_STREAM_LOCK(vosk->sinkpad);
      gst_vosk_reset (vosk);
      GST_PAD_STREAM_UNLOCK(vosk->sinkpad);
      break;

    default:
      break;
  }

  GST_DEBUG_OBJECT (vosk, "State change completed");
  return ret;
}

static void
gst_vosk_set_num_alternatives(GstVosk *vosk)
{
  GST_VOSK_LOCK(vosk);

  if (vosk->recognizer)
    vosk_recognizer_set_max_alternatives (vosk->recognizer, vosk->alternatives);
  else
    GST_LOG_OBJECT (vosk, "No recognizer to set num alternatives.");

  GST_VOSK_UNLOCK(vosk);
}

static void
gst_vosk_set_model_path (GstVosk *vosk,
                         const gchar *model_path)
{
  GstState state;

  /* This property can only be changed in the READY state */
  GST_OBJECT_LOCK(vosk);
  state = GST_STATE(vosk);
  if (state != GST_STATE_READY && state != GST_STATE_NULL) {
    GST_INFO_OBJECT (vosk, "Changing the `speech-model' property can only "
                           "be done in NULL or READY state");
    GST_OBJECT_UNLOCK(vosk);
    return;
  }
  GST_OBJECT_UNLOCK(vosk);

  GST_INFO_OBJECT (vosk, "new path for model %s", model_path);
  if(!g_strcmp0 (model_path, vosk->model_path))
    return;

  if (vosk->model_path)
    g_free (vosk->model_path);

  vosk->model_path = g_strdup (model_path);
}

static void
gst_vosk_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstVosk *vosk = GST_VOSK (object);

  switch (prop_id) {
    case PROP_USE_SIGNALS:
      vosk->use_signals=g_value_get_boolean (value);
      break;

    case PROP_SPEECH_MODEL:
      gst_vosk_set_model_path(vosk, g_value_get_string (value));
      break;
#ifdef HAVE_RNNOISE
    case PROP_ENABLE_DENOISE:
      vosk->enable_denoise = g_value_get_boolean(value);
      vosk->denoise_initialized = FALSE; // Force re-initialization
      GST_INFO_OBJECT(vosk, "Denoise %s", vosk->enable_denoise ? "enabled" : "disabled");
      break;
#endif
    case PROP_ALTERNATIVES:
      if (vosk->alternatives == g_value_get_int (value))
        return;

      vosk->alternatives = g_value_get_int(value);
      gst_vosk_set_num_alternatives (vosk);
      break;

    case PROP_PARTIAL_RESULTS_INTERVAL:
      vosk->partial_time_interval=g_value_get_int64(value) * GST_MSECOND;
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

/*
 * MUST be called with lock held
 */
static const gchar *
gst_vosk_final_result (GstVosk *vosk)
{
  const gchar *json_txt = NULL;

  GST_INFO_OBJECT(vosk, "getting final result");

  if (G_UNLIKELY(!vosk->recognizer)) {
    GST_DEBUG_OBJECT(vosk, "no recognizer available");
    return NULL;
  }

  PROTECT_FROM_LOCALE_BUG_START

  json_txt = vosk_recognizer_final_result (vosk->recognizer);

  PROTECT_FROM_LOCALE_BUG_END

  if (vosk->prev_partial) {
    g_free (vosk->prev_partial);
    vosk->prev_partial = NULL;
  }

  GST_INFO_OBJECT(vosk, "final results");

  if (!json_txt ||
      !strcmp(json_txt, VOSK_EMPTY_TEXT_RESULT) ||
      !strcmp(json_txt, VOSK_EMPTY_TEXT_RESULT_ALT))
    return NULL;

  return json_txt;
}

static const gchar *
gst_vosk_result (GstVosk *vosk)
{
  const char *json_txt;

  if (G_UNLIKELY(!vosk->recognizer)) {
    GST_DEBUG_OBJECT(vosk, "no recognizer available");
    return NULL;
  }

  PROTECT_FROM_LOCALE_BUG_START

  json_txt = vosk_recognizer_result (vosk->recognizer);

  PROTECT_FROM_LOCALE_BUG_END

  if (vosk->prev_partial) {
    g_free (vosk->prev_partial);
    vosk->prev_partial = NULL;
  }

  /* Don't send message if empty */
  if (!json_txt || !strcmp(json_txt, VOSK_EMPTY_TEXT_RESULT))
    return NULL;

  return json_txt;
}

static void
gst_vosk_get_property (GObject *object,
                       guint prop_id,
                       GValue *prop_value,
                       GParamSpec *pspec)
{
  GstVosk *vosk = GST_VOSK (object);

  switch (prop_id) {
    case PROP_USE_SIGNALS:
      g_value_set_boolean (prop_value, vosk->use_signals);
      break;

    case PROP_SPEECH_MODEL:
      g_value_set_string (prop_value, vosk->model_path);
      break;
#ifdef HAVE_RNNOISE
    case PROP_ENABLE_DENOISE:
      g_value_set_boolean(prop_value, vosk->enable_denoise);
      break;
#endif
    case PROP_ALTERNATIVES:
      g_value_set_int(prop_value, vosk->alternatives);
      break;

    case PROP_CURRENT_FINAL_RESULTS:
      GST_VOSK_LOCK(vosk);
      g_value_set_string (prop_value, gst_vosk_final_result(vosk));
      GST_VOSK_UNLOCK(vosk);
      break;

    case PROP_CURRENT_RESULTS:
      GST_VOSK_LOCK(vosk);
      g_value_set_string (prop_value, gst_vosk_result(vosk));
      GST_VOSK_UNLOCK(vosk);
      break;

    case PROP_PARTIAL_RESULTS_INTERVAL:
      g_value_set_int64(prop_value, vosk->partial_time_interval / GST_MSECOND);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_vosk_message_new (GstVosk *vosk, const gchar *text_results)
{
  if (!text_results)
    return;

  if (vosk->use_signals)
    g_signal_emit(vosk, signals[RESULT], 0, text_results);
  else {
    GstMessage *msg;
    GstStructure *contents;
    GValue value = G_VALUE_INIT;

    contents = gst_structure_new_empty ("vosk");

    g_value_init (&value, G_TYPE_STRING);
    g_value_set_string (&value, text_results);

    gst_structure_set_value (contents, "current-result", &value);
    g_value_unset (&value);

    msg = gst_message_new_element (GST_OBJECT (vosk), contents);
    gst_element_post_message (GST_ELEMENT (vosk), msg);
  }
}

inline static void
gst_vosk_final_result_msg (GstVosk *vosk)
{
  const gchar *json_txt = NULL;

  json_txt = gst_vosk_final_result(vosk);
  if (json_txt)
    gst_vosk_message_new (vosk, json_txt);
}

static void
gst_vosk_flush(GstVosk *vosk)
{
  GST_INFO_OBJECT (vosk, "flushing");

  GST_VOSK_LOCK(vosk);

  if (vosk->recognizer)
    vosk_recognizer_reset(vosk->recognizer);
  else
    GST_DEBUG_OBJECT (vosk, "no recognizer to flush");

  GST_VOSK_UNLOCK(vosk);
}

static gboolean
gst_vosk_sink_event (GstPad *pad,
                     GstObject *parent,
                     GstEvent * event)
{
  GstVosk *vosk;

  vosk = GST_VOSK (parent);

  GST_LOG_OBJECT (vosk, "Received %s event: %" GST_PTR_FORMAT,
                  GST_EVENT_TYPE_NAME (event), event);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_FLUSH_START:
      gst_vosk_flush(vosk);
      break;

    case GST_EVENT_EOS:
      /* Cancel any ongoing model loading */
      gst_vosk_cancel_model_loading(vosk);

      /* Wait for the stream to complete */
      GST_PAD_STREAM_LOCK(vosk->sinkpad);
      gst_vosk_final_result_msg(vosk);
      GST_PAD_STREAM_UNLOCK(vosk->sinkpad);

      GST_DEBUG_OBJECT (vosk, "EOS stop event");
      break;

    default:
      break;
  }

  return gst_pad_event_default (pad, parent, event);
}

/*
 * The following functions are only called by gst_vosk_chain().
 * Which means that lock is held.
 */
inline static void
gst_vosk_result_msg (GstVosk *vosk)
{
  const gchar *json_txt;

  json_txt=gst_vosk_result(vosk);
  if (json_txt)
    gst_vosk_message_new (vosk, json_txt);
}

static void
gst_vosk_partial_result (GstVosk *vosk)
{
  const char *json_txt;

  /* NOTE: surprisingly this function can return "text" results. Mute them if
   * empty. */
  json_txt = vosk_recognizer_partial_result (vosk->recognizer);
  if (!json_txt ||
      !strcmp(json_txt, VOSK_EMPTY_PARTIAL_RESULT) ||
      !strcmp(json_txt, VOSK_EMPTY_TEXT_RESULT_ALT))
    return;

  /* To avoid posting message unnecessarily, make sure there is a change. */
  if (g_strcmp0 (json_txt, vosk->prev_partial) == 0)
      return;

  g_free (vosk->prev_partial);
  vosk->prev_partial = g_strdup (json_txt);

  gst_vosk_message_new (vosk, json_txt);
}


static void
gst_vosk_handle_buffer(GstVosk *vosk, GstBuffer *buf)
{
  GstClockTimeDiff diff_time;
  GstClockTime current_time;
  GstMapInfo info;
  int result;

#ifndef HAVE_RNNOISE
  gst_buffer_map(buf, &info, GST_MAP_READ);
  if (G_UNLIKELY(info.size == 0))
    return;
#endif

#ifdef HAVE_RNNOISE
  // Map buffer for read/write to allow in-place denoising
  if (!gst_buffer_map(buf, &info, GST_MAP_READWRITE)) {
    GST_WARNING_OBJECT(vosk, "Failed to map buffer");
    return;
  }

  if (G_UNLIKELY(info.size == 0)) {
    gst_buffer_unmap(buf, &info);
    return;
  }

  // Apply denoising if enabled
  gst_vosk_apply_denoise(vosk, &info);
#endif

  result = vosk_recognizer_accept_waveform (vosk->recognizer,
                                            (gchar*) info.data,
                                            info.size);
  if (result == -1) {
    GST_ERROR_OBJECT (vosk, "accept_waveform error");
    return;
  }

  current_time = gst_element_get_current_running_time(GST_ELEMENT(vosk));
  diff_time = GST_CLOCK_DIFF(GST_BUFFER_PTS(buf), current_time);

  GST_LOG_OBJECT (vosk, "buffer time=%"GST_TIME_FORMAT" current time=%"GST_TIME_FORMAT" diff=%li " \
                  "(buffer size %lu)",
                  GST_TIME_ARGS(GST_BUFFER_PTS(buf)),
                  GST_TIME_ARGS(current_time),
                  diff_time,
                  info.size);

  /* We want to catch up when we are behind (500 milliseconds) but also try
   * to get a result now and again (every half second) at least.
   * Reminder : number of bytes per second = 16 bits * rate / 8 bits
   * so 1/100 of a second = number of bytes / 100.
   * It means 5 buffers approx. */
  if (diff_time > (GST_SECOND / 2)) {
    GST_DEBUG_OBJECT (vosk, "we are late %"GST_TIME_FORMAT", catching up",
                     GST_TIME_ARGS(diff_time));

    /* Check results every half second nevertheless. */
    diff_time=GST_CLOCK_DIFF(vosk->last_processed_time, GST_BUFFER_PTS(buf));
    GST_DEBUG_OBJECT (vosk, "%"GST_TIME_FORMAT" elapsed since last processed buffer",
                     GST_TIME_ARGS(diff_time));
    if (diff_time < ((GST_SECOND << 1) / 10))
      return;

    GST_INFO_OBJECT (vosk, "forcing result checking");
  }

  vosk->last_processed_time=GST_BUFFER_PTS(buf);

  if (result == 1) {
    GST_LOG_OBJECT (vosk, "checking result");
    gst_vosk_result_msg(vosk);
    vosk->last_partial=GST_BUFFER_PTS (buf);
    return;
  }

  if (vosk->partial_time_interval < 0)
    return;

  diff_time=GST_CLOCK_DIFF(vosk->last_partial, GST_BUFFER_PTS (buf));
  if (vosk->partial_time_interval < diff_time) {
    GST_LOG_OBJECT (vosk, "checking partial result");
    gst_vosk_partial_result(vosk);
    vosk->last_partial=GST_BUFFER_PTS(buf);
  }
#ifdef HAVE_RNNOISE
   gst_buffer_unmap(buf, &info);
#endif
}

static GstFlowReturn
gst_vosk_chain (GstPad *sinkpad,
                GstObject *parent,
                GstBuffer *buf)
{
  GstVosk *vosk = GST_VOSK (parent);

  GST_LOG_OBJECT (vosk, "data received");

  GST_VOSK_LOCK(vosk);

  if (G_LIKELY(vosk->recognizer)) {
    if (vosk->last_processed_time == GST_CLOCK_TIME_NONE) {
      vosk->last_processed_time=GST_BUFFER_PTS(buf);
      GST_INFO_OBJECT (vosk, "started with no PREROLL state, first buffer received");
    }

    gst_vosk_handle_buffer(vosk, buf);
  }
  else {
    /* While transitioning from READY to PAUSED, there might be at least one
     * buffer that is chained for PREROLL while the model is being loaded.
     * Just ignore it and pass it to next element but get its time to know that
     * we are in preroll. */
    if (vosk->last_processed_time == GST_CLOCK_TIME_NONE) {
      vosk->last_processed_time=GST_BUFFER_PTS(buf);
      GST_INFO_OBJECT (vosk, "PREROLL state, first buffer received");
    }
    else
      GST_WARNING_OBJECT (vosk, "dropping buffer, streaming has started and recognizer is not ready yet");
  }

  GST_VOSK_UNLOCK(vosk);

  GST_LOG_OBJECT (vosk, "chaining data");
  gst_buffer_ref(buf);
  return gst_pad_push (vosk->srcpad, buf);
}

gboolean
gst_vosk_plugin_init (GstPlugin *vosk_plugin)
{
  GST_DEBUG_CATEGORY_INIT (gst_vosk_debug, "vosk",
      0, "Performs speech recognition using libvosk");

  return gst_element_register (vosk_plugin, "vosk", GST_RANK_NONE, GST_TYPE_VOSK);
}

GST_PLUGIN_DEFINE (
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    vosk,
    "Performs speech recognition using libvosk",
    gst_vosk_plugin_init,
    PACKAGE_VERSION,
    PACKAGE_LICENSE,
    PACKAGE,
    "http://gstreamer.net/"
)
