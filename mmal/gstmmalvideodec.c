/*
 * Copyright (C) 2015, YouView TV Ltd.
 *   Author: John Sadler <john.sadler@youview.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 *
 */

#include "gstmmalvideodec.h"
#include "gstmmalmemory.h"
#include "gstmmalutil.h"

#include <gst/gst.h>

#include <string.h>
#include <stdio.h>

#include <bcm_host.h>

#include <interface/mmal/util/mmal_default_components.h>
#include <interface/mmal/util/mmal_util.h>
#include <interface/mmal/util/mmal_util_params.h>


GST_DEBUG_CATEGORY_STATIC (gst_mmal_video_dec_debug_category);
#define GST_CAT_DEFAULT gst_mmal_video_dec_debug_category

#define gst_mmal_video_dec_parent_class parent_class

#define GST_MMAL_VIDEO_DEC_EXTRA_OUTPUT_BUFFERS_OPAQUE_MODE 20

#define GST_MMAL_VIDEO_DEC_INPUT_BUFFER_WAIT_FOR_MS 2000

/* N.B. Drain timeout is quite high because with low-bitrate, low-fps stream, we
   can end-up with many input frames buffered ahead of the EOS, and we want to
   wait for those to be sent downstream.
 */
#define GST_MMAL_VIDEO_DEC_DRAIN_TIMEOUT_MS 10000

static GstStaticPadTemplate gst_mmal_video_dec_src_factory =
    GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE_WITH_FEATURES
        (GST_CAPS_FEATURE_MEMORY_MMAL_OPAQUE,
            "{ I420 }") ";" GST_VIDEO_CAPS_MAKE ("{ I420 }")));

struct MappedBuffer_;

struct MappedBuffer_
{
  GstBuffer *buffer;
  GstMapInfo map_info;

  /* N.B. The linked-list structure serves the same purpose as a refcount.
     I don't want to map & unmap the buffer n times, and due to MMAL restriction
     we may have to pass the same GstBuffer into multiple MMAL buffer headers.
   */
  struct MappedBuffer_ *next;
  struct MappedBuffer_ *previous;

};

typedef struct MappedBuffer_ MappedBuffer;


/* prototypes */


/*
From GstVideoDecoder:

The bare minimum that a functional subclass needs to implement is:

Provide pad templates

Inform the base class of output caps via gst_video_decoder_set_output_state

Parse input data, if it is not considered packetized from upstream Data will be
provided to parse which should invoke gst_video_decoder_add_to_frame and
gst_video_decoder_have_frame to separate the data belonging to each video frame.

Accept data in handle_frame and provide decoded results to
gst_video_decoder_finish_frame, or call gst_video_decoder_drop_frame.
*/

static void gst_mmal_video_dec_finalize (GObject * object);

static GstStateChangeReturn gst_mmal_video_dec_change_state (GstElement *
    element, GstStateChange transition);

static gboolean gst_mmal_video_dec_open (GstVideoDecoder * decoder);
static gboolean gst_mmal_video_dec_close (GstVideoDecoder * decoder);
static gboolean gst_mmal_video_dec_start (GstVideoDecoder * decoder);
static gboolean gst_mmal_video_dec_stop (GstVideoDecoder * decoder);
static gboolean gst_mmal_video_dec_set_format (GstVideoDecoder * decoder,
    GstVideoCodecState * state);
static gboolean gst_mmal_video_dec_sink_event (GstVideoDecoder * decoder,
    GstEvent * event);
static GstFlowReturn gst_mmal_video_dec_send_codec_data (GstMMALVideoDec * self,
    GstBuffer * codec_data, int64_t pts_microsec, int64_t dts_microsec);
static GstFlowReturn gst_mmal_video_dec_handle_frame (GstVideoDecoder * decoder,
    GstVideoCodecFrame * frame);

static GstFlowReturn gst_mmal_video_dec_finish (GstVideoDecoder * decoder);

static gboolean gst_mmal_video_dec_flush_start (GstMMALVideoDec * self,
    gboolean stop);
static gboolean gst_mmal_video_dec_flush_stop (GstMMALVideoDec * self,
    gboolean stop);

static GstFlowReturn gst_mmal_video_dec_drain (GstMMALVideoDec * self);

static GstVideoCodecFrame
    * gst_mmal_video_dec_output_find_nearest_frame (MMAL_BUFFER_HEADER_T * buf,
    GList * frames);

static gboolean gst_mmal_video_dec_output_decide_allocation (GstVideoDecoder
    * decoder, GstQuery * query);

static gboolean gst_mmal_video_dec_output_populate_output_port (GstMMALVideoDec
    * self);

static GstFlowReturn gst_mmal_video_dec_output_update_src_caps (GstMMALVideoDec
    * self);

static GstFlowReturn
gst_mmal_video_dec_output_reconfigure_output_port (GstMMALVideoDec * self,
    gboolean update_src_caps);

static void gst_mmal_video_dec_output_task_loop (GstMMALVideoDec * self);

/* class initialisation */

#define DEBUG_INIT \
  GST_DEBUG_CATEGORY_INIT (gst_mmal_video_dec_debug_category, "mmalvideodec", 0, \
      "debug category for gst-mmal video decoder base class");


G_DEFINE_ABSTRACT_TYPE_WITH_CODE (GstMMALVideoDec, gst_mmal_video_dec,
    GST_TYPE_VIDEO_DECODER, DEBUG_INIT);


static void
gst_mmal_video_dec_class_init (GstMMALVideoDecClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstVideoDecoderClass *video_decoder_class = GST_VIDEO_DECODER_CLASS (klass);
  GstPadTemplate *pad_template;

  gobject_class->finalize = gst_mmal_video_dec_finalize;

  element_class->change_state =
      GST_DEBUG_FUNCPTR (gst_mmal_video_dec_change_state);

  video_decoder_class->open = GST_DEBUG_FUNCPTR (gst_mmal_video_dec_open);
  video_decoder_class->close = GST_DEBUG_FUNCPTR (gst_mmal_video_dec_close);
  video_decoder_class->start = GST_DEBUG_FUNCPTR (gst_mmal_video_dec_start);
  video_decoder_class->stop = GST_DEBUG_FUNCPTR (gst_mmal_video_dec_stop);
  video_decoder_class->sink_event =
      GST_DEBUG_FUNCPTR (gst_mmal_video_dec_sink_event);

  /* Optional. Called to request subclass to dispatch any pending remaining data
     at EOS. Sub-classes can refuse to decode new data after.
   */
  video_decoder_class->finish = GST_DEBUG_FUNCPTR (gst_mmal_video_dec_finish);

  /* GstVideoDecoder vmethods */
  video_decoder_class->set_format =
      GST_DEBUG_FUNCPTR (gst_mmal_video_dec_set_format);

  video_decoder_class->handle_frame =
      GST_DEBUG_FUNCPTR (gst_mmal_video_dec_handle_frame);

  video_decoder_class->decide_allocation =
      GST_DEBUG_FUNCPTR (gst_mmal_video_dec_output_decide_allocation);

  /* src pad  N.B. Sink is done in derived class, since it knows format. */
  pad_template = gst_static_pad_template_get (&gst_mmal_video_dec_src_factory);
  gst_element_class_add_pad_template (element_class, pad_template);

}


static void
gst_mmal_video_dec_init (GstMMALVideoDec * self)
{
  gst_video_decoder_set_packetized (GST_VIDEO_DECODER (self), TRUE);

  self->dec = NULL;
  self->started = FALSE;
  self->output_flow_ret = GST_FLOW_OK;

  self->draining = FALSE;
  g_mutex_init (&self->drain_lock);
  g_cond_init (&self->drain_cond);

  self->input_state = NULL;
  self->codec_data = NULL;
  self->input_buffer_pool = NULL;
  self->last_upstream_ts = 0;

  self->caps_changed = FALSE;
  self->output_buffer_pool_opaque = NULL;
  self->output_buffer_pool_plain = NULL;
  self->output_buffer_pool = NULL;
  self->decoded_frames_queue = NULL;
  self->output_buffer_flags = 0;
  self->opaque = FALSE;
  self->output_reconfigured = FALSE;

  self->flushing = FALSE;
}


/**
 * This is called on MMAL thread when the control port has something to tell us.
 *
 * We just log the messages at the moment as the only useful thing we are
 * getting on this callback is error notifications.
 */
static void
gst_mmal_video_dec_control_port_callback (MMAL_PORT_T * port,
    MMAL_BUFFER_HEADER_T * buffer)
{
  MMAL_STATUS_T status;

  GstMMALVideoDec *self = (GstMMALVideoDec *) port->userdata;

  if (buffer->cmd == MMAL_EVENT_ERROR) {

    status = (MMAL_STATUS_T) * (uint32_t *) buffer->data;

    GST_ERROR_OBJECT (self, "Control port signalled error: %x, %s",
        status, mmal_status_to_string (status));

  } else {
    GST_DEBUG_OBJECT (self, "Control port signalled other cmd: %x",
        buffer->cmd);
  }

  mmal_buffer_header_release (buffer);
}


/**
 * Called by GstVideoDecoder baseclass on transition NULL->READY
 *
 * Should allocate "non-stream-specific" resources.
 */
static gboolean
gst_mmal_video_dec_open (GstVideoDecoder * decoder)
{
  GstMMALVideoDec *self = GST_MMAL_VIDEO_DEC (decoder);

  GST_DEBUG_OBJECT (self, "Opening decoder...");

  bcm_host_init ();

  /* Create the video decoder component on VideoCore */
  if (mmal_component_create (MMAL_COMPONENT_DEFAULT_VIDEO_DECODER, &self->dec)
      != MMAL_SUCCESS) {
    GST_ERROR_OBJECT (self, "Failed to create MMAL decoder component.");
    return FALSE;
  }

  self->dec->control->userdata = (void *) self;

  if (mmal_port_enable (self->dec->control,
          gst_mmal_video_dec_control_port_callback) != MMAL_SUCCESS) {

    GST_ERROR_OBJECT (self, "Failed to enable decoder control port!");
    return FALSE;
  }

  self->decoded_frames_queue = mmal_queue_create ();

  self->started = FALSE;
  self->flushing = FALSE;

  GST_DEBUG_OBJECT (self, "Opened decoder.");

  return TRUE;
}


/**
 * Called by GstVideoDecoder baseclass on transition READY->NULL
 *
 * Should free "non-stream-specific" resources.
 */
static gboolean
gst_mmal_video_dec_close (GstVideoDecoder * decoder)
{
  GstMMALVideoDec *self = GST_MMAL_VIDEO_DEC (decoder);

  GST_DEBUG_OBJECT (self, "Closing decoder...");

  if (self->dec->control->is_enabled
      && mmal_port_disable (self->dec->control) != MMAL_SUCCESS) {
    /* Log the error, but there's not much we can do at this point. */
    GST_ERROR_OBJECT (self, "Failed to disable decoder control port!");
  }

  if (self->dec->is_enabled && mmal_component_disable (self->dec) !=
      MMAL_SUCCESS) {

    GST_ERROR_OBJECT (self, "Failed to disable decoder component!");
    return FALSE;
  }

  GST_DEBUG_OBJECT (self, "Freeing decoded frames queue...");

  if (self->decoded_frames_queue) {
    mmal_queue_destroy (self->decoded_frames_queue);
    self->decoded_frames_queue = NULL;
  }

  GST_DEBUG_OBJECT (self, "Freeing output buffer pool...");

  if (self->output_buffer_pool_opaque != NULL) {
    uint32_t output_buffers;

    output_buffers = mmal_queue_length (self->output_buffer_pool_opaque->queue);

    if (output_buffers != self->dec->output[0]->buffer_num) {
      GST_ERROR_OBJECT (self,
          "Failed to reclaim all output buffers (%u out of %u)",
          output_buffers, self->dec->output[0]->buffer_num);
    }

    mmal_port_pool_destroy (self->dec->output[0],
        self->output_buffer_pool_opaque);
    self->output_buffer_pool_opaque = NULL;
  }

  if (self->output_buffer_pool_plain != NULL) {
    mmal_pool_destroy (self->output_buffer_pool_plain);
    self->output_buffer_pool_plain = NULL;
  }

  mmal_component_destroy (self->dec);

  bcm_host_deinit ();

  self->dec = NULL;

  GST_DEBUG_OBJECT (self, "Closed decoder.");

  return TRUE;
}


static void
gst_mmal_video_dec_finalize (GObject * object)
{
  GstMMALVideoDec *self = GST_MMAL_VIDEO_DEC (object);

  g_mutex_clear (&self->drain_lock);
  g_cond_clear (&self->drain_cond);

  G_OBJECT_CLASS (gst_mmal_video_dec_parent_class)->finalize (object);
}

static GstStateChangeReturn
gst_mmal_video_dec_change_state (GstElement * element,
    GstStateChange transition)
{
  if (transition == GST_STATE_CHANGE_PAUSED_TO_READY) {
    GstMMALVideoDec *self = GST_MMAL_VIDEO_DEC (element);
    gboolean flush_started;

    GST_VIDEO_DECODER_STREAM_LOCK (self);

    /* flush will stop the output side */
    flush_started = gst_mmal_video_dec_flush_start (self, TRUE);

    GST_VIDEO_DECODER_STREAM_UNLOCK (self);

    if (!flush_started) {
      return GST_STATE_CHANGE_FAILURE;
    }
  }

  return GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);
}

/**
 * Called by GstVideoDecoder baseclass on transition from READY->PAUSED
 *
 * We can acquire stream-specific resources.
 *
 * We don't start output task until we see first input frame in:
 * gst_mmal_video_dec_handle_frame()
 */
static gboolean
gst_mmal_video_dec_start (GstVideoDecoder * decoder)
{
  GstMMALVideoDec *self = GST_MMAL_VIDEO_DEC (decoder);

  GST_DEBUG_OBJECT (self, "Starting decoder...");

  self->last_upstream_ts = 0;

  self->output_flow_ret = GST_FLOW_OK;
  g_mutex_lock (&self->drain_lock);
  self->draining = FALSE;
  g_mutex_unlock (&self->drain_lock);

  GST_PAD_STREAM_LOCK (GST_VIDEO_DECODER_SRC_PAD (self));

  /* N.B. This is actually set in handle_frame() */
  self->started = FALSE;
  self->flushing = FALSE;

  self->caps_changed = FALSE;
  self->output_reconfigured = FALSE;
  self->output_buffer_flags = 0;

  self->dec->output[0]->userdata = (void *) self;

  GST_PAD_STREAM_UNLOCK (GST_VIDEO_DECODER_SRC_PAD (self));

  return TRUE;
}


/**
 * Called by GstVideoDecoder baseclass on transition from PAUSED->READY.
 *
 * We should free stream-specific resources.
 */
static gboolean
gst_mmal_video_dec_stop (GstVideoDecoder * decoder)
{
  gboolean success = TRUE;

  GstMMALVideoDec *self;

  GST_VIDEO_DECODER_STREAM_LOCK (decoder);

  self = GST_MMAL_VIDEO_DEC (decoder);

  GST_DEBUG_OBJECT (self, "Stopping decoder...");

  /* Full flush will stop the output side. */
  gst_mmal_video_dec_flush_stop (self, TRUE);

  self->output_flow_ret = GST_FLOW_FLUSHING;

  gst_buffer_replace (&self->codec_data, NULL);

  if (self->input_state) {
    gst_video_codec_state_unref (self->input_state);
    self->input_state = NULL;
  }

  GST_DEBUG_OBJECT (self, "Disabling input port...");

  gst_video_decoder_set_needs_format (decoder, FALSE);

  /* Disable ports and free-up buffer pools. */
  if (self->dec->input[0]->is_enabled &&
      mmal_port_disable (self->dec->input[0]) != MMAL_SUCCESS) {

    GST_ERROR_OBJECT (self, "Failed to disable input port!");
    success = FALSE;
  }

  GST_DEBUG_OBJECT (self, "Freeing input buffer pool...");

  if (self->input_buffer_pool != NULL) {
    mmal_pool_destroy (self->input_buffer_pool);
    self->input_buffer_pool = NULL;
  }

  GST_PAD_STREAM_LOCK (GST_VIDEO_DECODER_SRC_PAD (self));

  GST_DEBUG_OBJECT (self, "Disabling output port...");

  if (self->dec->output[0]->is_enabled &&
      mmal_port_disable (self->dec->output[0]) != MMAL_SUCCESS) {

    GST_ERROR_OBJECT (self, "Failed to disable output port!");
    success = FALSE;
  }

  self->output_buffer_pool = NULL;

  GST_PAD_STREAM_UNLOCK (GST_VIDEO_DECODER_SRC_PAD (self));

  GST_DEBUG_OBJECT (self, "Stopped decoder.");

  GST_VIDEO_DECODER_STREAM_UNLOCK (decoder);

  return success;
}


/**
 * This callback is called by MMAL when it's finished with an input buffer.
 * It's associated with the input port at the time the port is enabled.
 */
static void
gst_mmal_video_dec_mmal_return_input_buffer_to_pool (MMAL_PORT_T * port,
    MMAL_BUFFER_HEADER_T * buffer)
{
  MappedBuffer *mb = NULL;

  g_return_if_fail (port != NULL);
  g_return_if_fail (buffer != NULL);

  GST_TRACE_OBJECT (GST_ELEMENT (port->userdata), "Input port callback: %p",
      buffer);
  /* We're using the GstBuffer directly, rather than copying into an MMAL
     allocated buffer.  So we need to unmap and unref the GstBuffer once the
     decoder is done with it.

     N.B. We `user_data` will be NULL in case buffer had no GstBuffer attached,
     (i.e. when EOS buffer sent from drain()).
   */
  if (!buffer->cmd && buffer->user_data) {

    mb = (MappedBuffer *) buffer->user_data;

    /* g_return_if_fail() will not decrement reference count of mmal buffer */
    if (mb == NULL) {
      goto done;
    }

    /* Was this the last MMAL buffer header pointing at our mapped GST buffer?
       If so, we can unmap it.

       Incidentally, we can have several MMAL headers pointing to different
       offsets in the same GST buffer mapping since we must tell MMAL upfront
       how big the input buffers are, and our GST buffer might exceed that size.
     */
    if (mb->next == NULL && mb->previous == NULL) {

      gst_buffer_unmap (mb->buffer, &mb->map_info);
      gst_buffer_unref (mb->buffer);

    } else {

      /* Not the last reference. Unmap later. */
      if (mb->previous) {
        mb->previous->next = mb->next;
      }

      if (mb->next) {
        mb->next->previous = mb->previous;
      }
    }

    /* Restore original MMAL buffer payload we used for storing buffer info */
    buffer->data = (uint8_t *) mb;
    buffer->length = buffer->alloc_size = sizeof (*mb);
  }

  /* This drops the refcount of the buffer header.  It will be returned to it's
     pool when the refcount reaches zero.
   */
done:
  mmal_buffer_header_release (buffer);
}


/**
 * This callback is called by MMAL when a frame has been decoded.
 * It's associated with the output port at the time the port is enabled.
 */
static void
gst_mmal_video_dec_mmal_queue_decoded_frame (MMAL_PORT_T * port,
    MMAL_BUFFER_HEADER_T * buffer)
{
  GstMMALVideoDec *self = GST_MMAL_VIDEO_DEC (port->userdata);

  GST_TRACE_OBJECT (self, "Queuing decoded buffer %p, PTS %" GST_TIME_FORMAT,
      buffer, GST_TIME_ARGS (gst_util_uint64_scale (buffer->pts, GST_SECOND,
              G_USEC_PER_SEC)));
  mmal_queue_put (self->decoded_frames_queue, buffer);
}


/**
 * We implement this in order to negotiate MMAL opaque buffers, where downstream
 * supports it.
 */
static gboolean
gst_mmal_video_dec_output_decide_allocation (GstVideoDecoder * decoder,
    GstQuery * query)
{
  GstMMALVideoDec *self = GST_MMAL_VIDEO_DEC (decoder);
  GstBufferPool *pool;
  GstStructure *config;

  GstCaps *caps;
  gint i, n;
  GstVideoInfo info;

  self->opaque = FALSE;

  GST_DEBUG_OBJECT (self, "Deciding allocation...");

  gst_query_parse_allocation (query, &caps, NULL);

  if (caps && gst_video_info_from_caps (&info, caps)
      && info.finfo->format == GST_VIDEO_FORMAT_I420) {

    gboolean found = FALSE;
    GstCapsFeatures *feature = gst_caps_get_features (caps, 0);

    /* Prefer an MMAL Opaque allocator if available and we want to use it */
    n = gst_query_get_n_allocation_params (query);

    GST_DEBUG_OBJECT (self, "Looking for opaque allocator...");

    for (i = 0; i < n; i++) {

      GstAllocator *allocator;
      GstAllocationParams params;

      gst_query_parse_nth_allocation_param (query, i, &allocator, &params);

      if (allocator) {

        if (g_strcmp0 (allocator->mem_type, GST_MMAL_OPAQUE_MEMORY_TYPE) == 0) {

          found = TRUE;

          gst_query_set_nth_allocation_param (query, 0, allocator, &params);
          while (gst_query_get_n_allocation_params (query) > 1) {
            gst_query_remove_nth_allocation_param (query, 1);
          }
        }

        gst_object_unref (allocator);

        if (found) {
          /* We will use opaque buffers. */
          GST_DEBUG_OBJECT (self, "Negotiated to use opaque buffers.");
          self->opaque = TRUE;
          break;
        }
      }
    }

    /* If try to negotiate with caps feature memory:MMALOpaque
       and if allocator is not of type memory MMALOpaque then fails.
       Should not have negotiated MMAL Opaque in allocation query caps
       otherwise.
     */
    if (feature
        && gst_caps_features_contains (feature,
            GST_CAPS_FEATURE_MEMORY_MMAL_OPAQUE) && !found) {

      GST_WARNING_OBJECT (self, "Caps indicate MMAL opaque buffers supported, "
          "but allocator not found!");
      return FALSE;
    }
  }

  if (!GST_VIDEO_DECODER_CLASS
      (gst_mmal_video_dec_parent_class)->decide_allocation (decoder, query)) {
    return FALSE;
  }

  g_assert (gst_query_get_n_allocation_pools (query) > 0);
  gst_query_parse_nth_allocation_pool (query, 0, &pool, NULL, NULL, NULL);
  g_assert (pool != NULL);

  config = gst_buffer_pool_get_config (pool);

  if (gst_query_find_allocation_meta (query, GST_VIDEO_META_API_TYPE, NULL)) {
    gst_buffer_pool_config_add_option (config,
        GST_BUFFER_POOL_OPTION_VIDEO_META);
  }

  if (self->opaque) {

    /* Downstream proposed MMAL allocator, above.

       In this case, we override the choice of buffer pool with a specialised
       one that takes care of releasing MMAL buffers when the GstBuffer is
       returned to the pool.

       Downstream *could* provide this specialised pool as well, but we're
       providing the MMAL buffer headers, so this seems to make sense.

       I wish we didn't need this specialised pool, and could just use the
       allocator, but I don't know of another way to unref the mmal header
       when the buffer is returned to pool (except perhaps some hack with a
       custom meta on the buffer).
     */

    GstAllocator *allocator;
    guint config_size;
    guint config_min_buffers;
    guint config_max_buffers;
    GstAllocationParams allocator_params;

    GST_DEBUG_OBJECT (self, "Creating new output buffer pool...");

    gst_buffer_pool_config_get_params (config, &caps, &config_size,
        &config_min_buffers, &config_max_buffers);

    gst_buffer_pool_config_get_allocator (config, &allocator,
        &allocator_params);

    gst_object_unref (pool);
    pool = gst_mmal_opaque_buffer_pool_new ();

    gst_buffer_pool_config_set_params (config, caps, config_size,
        config_min_buffers, config_max_buffers);

    gst_buffer_pool_config_set_allocator (config, allocator, &allocator_params);

    gst_query_set_nth_allocation_pool (query, 0, pool, config_size,
        config_min_buffers, config_max_buffers);
  }
  /* end-if (opaque) */

  gst_buffer_pool_set_config (pool, config);
  gst_object_unref (pool);

  return TRUE;
}


/**
 * This is called from gst_mmal_video_dec_output_reconfigure_output_port()
 * and also directly gst_mmal_video_dec_output_task_loop().
 *
 * The second case is because once decoder has decoded first frame following
 * a format change, it can tell us more about the output format.  Specifically,
 * we are looking for framerate and interlace mode.
 *
 * Incidentally, we trust the decoder to tell us framerate and interlace mode
 * because it can work this out from the stream headers, whereas sink caps from
 * upstream can easily be wrong.
 *
 * This function will be called *with* AND *without* decoder stream lock held.
 *
 */
static GstFlowReturn
gst_mmal_video_dec_output_update_src_caps (GstMMALVideoDec * self)
{
  GstVideoCodecState *state = NULL;
  MMAL_ES_FORMAT_T *output_format = self->dec->output[0]->format;
  MMAL_PARAMETER_VIDEO_INTERLACE_TYPE_T interlace_type;
  GstFlowReturn flow_ret = GST_FLOW_OK;
  GstVideoFormat format;
  GstVideoInterlaceMode interlace_mode = GST_VIDEO_INTERLACE_MODE_PROGRESSIVE;

  /* Without this, the framerate will not be up-to-date when we read it from
     output port format.
   */
  if (mmal_port_format_commit (self->dec->output[0]) != MMAL_SUCCESS) {

    /* N.B. We don't error out on this as we're only doing it here so we can
       read-back framerate, and there's not a lot we can do about it anyway.
     */
    GST_WARNING_OBJECT (self, "Failed to commit output format!");
  }

  format = gst_mmal_video_get_format_from_mmal (output_format->encoding);

  if (format == GST_VIDEO_FORMAT_UNKNOWN) {

    GST_ERROR_OBJECT (self, "Unsupported color format: %lu",
        (unsigned long) output_format->encoding);

    flow_ret = GST_FLOW_NOT_NEGOTIATED;
    goto error;
  }

  self->output_buffer_flags = 0;

  /* Query whether interlaced. */
  interlace_type.hdr.id = MMAL_PARAMETER_VIDEO_INTERLACE_TYPE;
  interlace_type.hdr.size = sizeof (MMAL_PARAMETER_VIDEO_INTERLACE_TYPE_T);

  if (mmal_port_parameter_get (self->dec->output[0], &interlace_type.hdr) !=
      MMAL_SUCCESS) {

    GST_ERROR_OBJECT (self, "Failed to query interlace type!");

  } else {

    /* N.B. Although MMAL interlace mode tells us field order, I'm not sure
       there is anywhere for us to put that in the vinfo?

       In any case, progressive and interlaced frames can both appear in Mixed
       mode, so we will need to look at each output buffer and set appropriate
       buffer flags on Gst output buffer.

       NOTE: There seems to be a problem with flags on MMAL output buffers.
       Deinterlace flags are not set on buffers, even when the output is clearly
       interlaced.  So all we can do for now is to take what the decoder tells
       us here to derive flags we will apply to all output GstBuffers that
       follow.
     */
    switch (interlace_type.eMode) {

      case MMAL_InterlaceFieldSingleUpperFirst:
        /* Fields are stored in one buffer, use the frame ID to get access to the
           required field.

           Each field has only half the amount of lines as noted in the
           height property. This mode requires multiple GstVideoMeta metadata
           to describe the fields.
         */
        GST_DEBUG_OBJECT (self, "InterlaceFieldSingleUpperFirst");
        interlace_mode = GST_VIDEO_INTERLACE_MODE_FIELDS;
        self->output_buffer_flags |= GST_VIDEO_BUFFER_FLAG_INTERLACED;
        self->output_buffer_flags |= GST_VIDEO_BUFFER_FLAG_TFF;
        break;

      case MMAL_InterlaceFieldSingleLowerFirst:
        GST_DEBUG_OBJECT (self, "InterlaceFieldSingleLowerFirst");
        interlace_mode = GST_VIDEO_INTERLACE_MODE_FIELDS;
        self->output_buffer_flags |= GST_VIDEO_BUFFER_FLAG_INTERLACED;
        break;

      case MMAL_InterlaceFieldsInterleavedUpperFirst:
        /* Fields are interleaved in one video frame. Extra buffer flags
           describe the field order.
         */
        GST_DEBUG_OBJECT (self, "InterlaceFieldsInterleavedUpperFirst");
        interlace_mode = GST_VIDEO_INTERLACE_MODE_INTERLEAVED;
        self->output_buffer_flags |= GST_VIDEO_BUFFER_FLAG_INTERLACED;
        self->output_buffer_flags |= GST_VIDEO_BUFFER_FLAG_TFF;
        break;

      case MMAL_InterlaceFieldsInterleavedLowerFirst:
        GST_DEBUG_OBJECT (self, "InterlaceFieldsInterleavedLowerFirst");
        interlace_mode = GST_VIDEO_INTERLACE_MODE_INTERLEAVED;
        self->output_buffer_flags |= GST_VIDEO_BUFFER_FLAG_INTERLACED;
        break;

      case MMAL_InterlaceMixed:
        /* Frames contains both interlaced and progressive video, the buffer flags
           describe the frame and fields.
         */
        GST_DEBUG_OBJECT (self, "InterlaceMixed");
        interlace_mode = GST_VIDEO_INTERLACE_MODE_MIXED;
        self->output_buffer_flags |= GST_VIDEO_BUFFER_FLAG_INTERLACED;
        break;

      case MMAL_InterlaceProgressive:
      default:
        /* All frames are progressive */
        GST_DEBUG_OBJECT (self, "InterlaceProgressive");
        interlace_mode = GST_VIDEO_INTERLACE_MODE_PROGRESSIVE;
        break;
    }

    if (interlace_type.bRepeatFirstField) {
      GST_DEBUG_OBJECT (self, "Repeat first field");
      self->output_buffer_flags |= GST_VIDEO_BUFFER_FLAG_RFF;
    }
  }

  /* Now we need the lock.  N.B. The lock uses recursive mutex, so if we
     entered this function with the lock already held, that should still be OK
   */
  GST_VIDEO_DECODER_STREAM_LOCK (self);

  state = gst_video_decoder_set_output_state (GST_VIDEO_DECODER (self),
      format, output_format->es->video.crop.width,
      output_format->es->video.crop.height, self->input_state);

  /* Take framerate from output port for src caps. */
  GST_VIDEO_INFO_FPS_N (&state->info) = output_format->es->video.frame_rate.num;
  GST_VIDEO_INFO_FPS_D (&state->info) = output_format->es->video.frame_rate.den;

  GST_VIDEO_INFO_INTERLACE_MODE (&state->info) = interlace_mode;

  /* The negotiation with downstream.

     We always want to use opaque buffers if we can negotiate that.  However,
     GstVideoDecoder won't include the needed feature in the caps, so we have
     to do that ourselves here.  We first try negotiating opaque then, if that
     fails, we remove the opaque feature and try to negotiate "plain" buffers.
   */
  if (state->caps) {
    gst_caps_unref (state->caps);
  }

  state->caps = gst_video_info_to_caps (&state->info);

  gst_caps_set_features (state->caps, 0,
      gst_caps_features_new (GST_CAPS_FEATURE_MEMORY_MMAL_OPAQUE, NULL));

  GST_DEBUG_OBJECT (self, "Attempting to negotiate opaque buffers...");

  if (!gst_video_decoder_negotiate (GST_VIDEO_DECODER (self))) {

    GST_INFO_OBJECT (self, "Failed to negotiate opaque with downstream.");

    gst_caps_replace (&state->caps, gst_video_info_to_caps (&state->info));

    if (!gst_video_decoder_negotiate (GST_VIDEO_DECODER (self))) {

      GST_ERROR_OBJECT (self, "Failed to negotiate caps with downstream.");

      gst_video_codec_state_unref (state);

      flow_ret = GST_FLOW_NOT_NEGOTIATED;
      GST_VIDEO_DECODER_STREAM_UNLOCK (self);
      goto error;
    }
  }

  GST_VIDEO_DECODER_STREAM_UNLOCK (self);

  /* Prevent re-entry into this function from task loop.
     NOTE: caps_changed is protected by src pad mutex, not the main decoder
     mutex.
   */
  self->caps_changed = FALSE;

  return flow_ret;

error:
  /* N.B. would release stream lock here, if acquired. */
  return flow_ret;
}


/**
 * Drain the decoder.
 *
 * CHECKME:  This is not hooked-up as a GstVideoDecoder vmethod yet.  gst-omx
 * doesn't seem to do this either.  Should we?
 *
 * We push EOS into the decoder input port, and wait for it to pop-out of the
 * output port.
 *
 * From GstVideoDecoder docs:
 *
 * "Optional. Called to request subclass to decode any data it can at this point
 * but that more data may arrive after. (e.g. at segment end). Sub-classes
 * should be prepared to handle new data afterward, or seamless segment
 * processing will break."
 *
 * This will be called *with* decoder stream lock held.
 */
static GstFlowReturn
gst_mmal_video_dec_drain (GstMMALVideoDec * self)
{
  MMAL_BUFFER_HEADER_T *buffer = NULL;
  GstFlowReturn flow_ret = GST_FLOW_OK;

  gint64 wait_until;

  if (!self->started) {
    GST_DEBUG_OBJECT (self, "Output thread not started yet. Nothing to drain.");
    return GST_FLOW_OK;
  }

  if (self->input_buffer_pool == NULL) {
    /* We probably haven't set initial format yet.  Don't worry. */
    return GST_FLOW_OK;

  } else {

    /* Hold this mutex before sending EOS buffer, otherwise we have a race. */
    g_mutex_lock (&self->drain_lock);
    self->draining = TRUE;

    /* Yield the "big lock" before blocking on input buffer avail. */
    GST_VIDEO_DECODER_STREAM_UNLOCK (self);

    /* Grab a buffer */
    GST_DEBUG_OBJECT (self, "Waiting for input buffer...");

    if ((buffer = mmal_queue_timedwait (self->input_buffer_pool->queue,
                GST_MMAL_VIDEO_DEC_INPUT_BUFFER_WAIT_FOR_MS)) == NULL) {

      GST_ERROR_OBJECT (self, "Failed to acquire input buffer!");
      flow_ret = GST_FLOW_ERROR;
      goto no_input_buffer;
    }

    GST_DEBUG_OBJECT (self, "Got input buffer.");

    /* "Resets all variables to default values" */
    mmal_buffer_header_reset (buffer);
    buffer->cmd = 0;

    buffer->flags |= MMAL_BUFFER_HEADER_FLAG_EOS;

    /* Avoid trying to unref GstBuffer when releasing MMAL input buffer.
       See: gst_mmal_video_dec_mmal_return_input_buffer_to_pool().
     */
    buffer->user_data = NULL;

    buffer->pts = GST_TIME_AS_USECONDS (self->last_upstream_ts);

    GST_DEBUG_OBJECT (self, "Sending EOS with pts: %" GST_TIME_FORMAT,
        GST_TIME_ARGS (self->last_upstream_ts));

    if (mmal_port_send_buffer (self->dec->input[0], buffer) != MMAL_SUCCESS) {

      GST_ERROR_OBJECT (self, "Failed to send input buffer to decoder!");

      mmal_buffer_header_release (buffer);

      flow_ret = GST_FLOW_ERROR;
      goto drain_failed;
    }

    /* OK, so now we've sent EOS to decoder.  Wait for an EOS buffer to plop
       out the other end.  This will be picked-up by the output thread, which
       we're hoping will then signal the condition variable!

       N.B. there may be pending output frames in front of the EOS, which will
       be sent downstream.
     */

    wait_until = g_get_monotonic_time () +
        GST_MMAL_VIDEO_DEC_DRAIN_TIMEOUT_MS * G_TIME_SPAN_MILLISECOND;

    if (!g_cond_wait_until (&self->drain_cond, &self->drain_lock, wait_until)) {

      GST_ERROR_OBJECT (self, "Drain failed (timeout)!");

      flow_ret = GST_FLOW_ERROR;
      goto drain_failed;
    } else if (g_atomic_int_get (&self->flushing)) {

      GST_DEBUG_OBJECT (self, "Flushing: not drained.");
      flow_ret = GST_FLOW_FLUSHING;

    } else {

      GST_DEBUG_OBJECT (self, "Drained decoder.");
    }

  drain_failed:
    self->draining = FALSE;
    g_mutex_unlock (&self->drain_lock);

  no_input_buffer:
    GST_VIDEO_DECODER_STREAM_LOCK (self);

    return flow_ret;
  }
}


/**
 * (Optional) vmethod called by GstVideoDecoder at EOS.
 *
 * We want to drain and pause output task at EOS.
 */
static GstFlowReturn
gst_mmal_video_dec_finish (GstVideoDecoder * decoder)
{
  GstMMALVideoDec *self = GST_MMAL_VIDEO_DEC (decoder);

  return gst_mmal_video_dec_drain (self);
}


/**
 * This is called by the GstVideoDecoder baseclass to let us know the input
 * format has changed (i.e. when set_caps() is called on base).
 *
 * This will be called *with* decoder stream lock held.
 *
 * NOTE: We only deal with configuring the input side in this function,
 * including input port configuration.  The output side configuration is
 * done on the output thread - triggered by "caps_changed" flag, which we set
 * here.
 */
static gboolean
gst_mmal_video_dec_set_format (GstVideoDecoder * decoder,
    GstVideoCodecState * state)
{
  GstMMALVideoDec *self;
  GstMMALVideoDecClass *klass;
  GstVideoInfo *info = &state->info;
  gboolean is_format_change = FALSE;
  MMAL_PORT_T *input_port = NULL;
  MMAL_ES_FORMAT_T *input_format = NULL;

  self = GST_MMAL_VIDEO_DEC (decoder);
  klass = GST_MMAL_VIDEO_DEC_GET_CLASS (decoder);

  GST_DEBUG_OBJECT (self, "Setting new caps %" GST_PTR_FORMAT, state->caps);

  input_port = self->dec->input[0];

  input_format = input_port->format;

  /* Check if the caps change is a real format change or if only irrelevant
     parts of the caps have changed or nothing at all.
   */
  is_format_change |= input_format->es->video.width != info->width;
  is_format_change |= input_format->es->video.height != info->height;

  is_format_change |=
      (input_format->es->video.frame_rate.num == 0 && info->fps_n != 0) ||
      (input_format->es->video.frame_rate.num !=
      (info->fps_n << 16) / (info->fps_d));

  is_format_change |= (self->codec_data != state->codec_data);

  /* Ask derived class as well */
  if (klass->is_format_change) {
    is_format_change |= klass->is_format_change (self, input_port, state);
  }

  if (is_format_change) {

    GstFlowReturn flow_ret;

    GST_INFO_OBJECT (self, "The input format has changed.  Reconfiguring...");

    if (self->input_state != NULL) {
      gst_video_codec_state_unref (self->input_state);
      self->input_state = NULL;
    }

    self->input_state = gst_video_codec_state_ref (state);

    GST_DEBUG_OBJECT (self, "Draining decoder...");

    flow_ret = gst_mmal_video_dec_drain (self);
    if (flow_ret == GST_FLOW_FLUSHING) {

      GST_DEBUG_OBJECT (self, "Flushing: deferring format change...");
      gst_video_decoder_set_needs_format (decoder, TRUE);
      return TRUE;
    } else if (flow_ret != GST_FLOW_OK) {

      GST_ERROR_OBJECT (self, "Drain failed!");
      return FALSE;
    }

    /*
       If ports already enabled, then I guess we need to disable them.
       IIRC, VLC code first tries setting format without disable, then
       falls-back to disabling if that fails.
     */

    if (input_port->is_enabled &&
        mmal_port_disable (input_port) != MMAL_SUCCESS) {

      GST_ERROR_OBJECT (self, "Failed to disable input port!");
      return FALSE;
    }

    if (mmal_port_flush (input_port) != MMAL_SUCCESS) {
      GST_ERROR_OBJECT (self, "Failed to flush input port.");
    }

    gst_buffer_replace (&self->codec_data, state->codec_data);

    input_format->es->video.crop.x = 0;
    input_format->es->video.crop.y = 0;
    input_format->es->video.crop.width = info->width;
    input_format->es->video.crop.height = info->height;

    /* N.B. These are the alignments XBMC applies. */
    input_format->es->video.width = GST_ROUND_UP_32 (info->width);
    input_format->es->video.height = GST_ROUND_UP_16 (info->height);

    /* Don't provide any hint on framerate. Upstream sometimes gets it wrong.
       Let the decoder figure it out.
     */
    input_format->es->video.frame_rate.num = 0;
    input_format->es->video.frame_rate.den = 0;

    input_format->es->video.par.num = info->par_n;
    input_format->es->video.par.den = info->par_d;
    input_format->type = MMAL_ES_TYPE_VIDEO;
    input_format->flags = MMAL_ES_FORMAT_FLAG_FRAMED;

    /* Codec data will be send as an extra buffer before actual frame data */
    input_format->extradata_size = 0;

    /* Derived class should set the encoding here. */
    if (klass->set_format) {
      if (!klass->set_format (self, input_port, state)) {
        GST_ERROR_OBJECT (self, "Subclass failed to set the new format");
        return FALSE;
      }
    }

    if (mmal_port_format_commit (input_port) != MMAL_SUCCESS) {

      GST_ERROR_OBJECT (self, "Failed to commit new input port format!");
      return FALSE;
    }

    GST_DEBUG_OBJECT (self, "Buffers recommended (in): %u",
        input_port->buffer_num_recommended);

    input_port->buffer_num = input_port->buffer_num_recommended;
    input_port->buffer_size = input_port->buffer_size_recommended;

    /*
       An existing pool can be resized.

       Note that in practice we should not need to ever resize the input pool as
       the decoder always seems to request 20 buffers of 80K each.

       XBMC sources are interesting in that they ignore the recommended values
       and instead allocate only 2 x 1MB buffers to "reduce latency".  Perhaps
       that is worth some experimentation.

       In order to avoid copying the input GstBuffers, we ask MMAL to allocate
       payload buffers just big enough to hold some housekeeping data relating
       to the mapped GstBuffer memories.  We don't bother to reallocate the
       input buffer pool on format change, since the payload size and number of
       buffer headers will never change in practice.
     */

    if (self->input_buffer_pool == NULL) {

      self->input_buffer_pool = mmal_pool_create (input_port->buffer_num,
          sizeof (MappedBuffer));
    }

    input_port->userdata = (void *) self;
    if (mmal_port_enable (input_port,
            &gst_mmal_video_dec_mmal_return_input_buffer_to_pool) !=
        MMAL_SUCCESS) {

      GST_ERROR_OBJECT (self, "Failed to enable input port!");
      return FALSE;
    }

    /* This will trigger reconfiguration of the output side, on the output
       thread.
     */
    GST_PAD_STREAM_LOCK (GST_VIDEO_DECODER_SRC_PAD (self));
    self->caps_changed = TRUE;
    GST_PAD_STREAM_UNLOCK (GST_VIDEO_DECODER_SRC_PAD (self));

    if (!self->dec->is_enabled && mmal_component_enable (self->dec) !=
        MMAL_SUCCESS) {

      GST_ERROR_OBJECT (self, "Failed to enable decoder component!");
      return FALSE;
    }

    GST_DEBUG_OBJECT (self, "Input port info:");
    gst_mmal_print_port_info (input_port, GST_ELEMENT (self));
  }
  /* end-if (is_format_change) */

  return TRUE;
}

static gboolean
gst_mmal_video_dec_sink_event (GstVideoDecoder * decoder, GstEvent * event)
{
  GstMMALVideoDec *self = GST_MMAL_VIDEO_DEC (decoder);
  gboolean ret =
      GST_VIDEO_DECODER_CLASS (parent_class)->sink_event (decoder, event);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_FLUSH_START:
      GST_VIDEO_DECODER_STREAM_LOCK (decoder);
      ret = ret && gst_mmal_video_dec_flush_start (self, FALSE);
      GST_VIDEO_DECODER_STREAM_UNLOCK (decoder);
      break;

    case GST_EVENT_FLUSH_STOP:
      GST_VIDEO_DECODER_STREAM_LOCK (decoder);
      ret = ret && gst_mmal_video_dec_flush_stop (self, FALSE);
      if (ret && gst_video_decoder_get_needs_format (decoder)) {

        GstVideoCodecState *state = self->input_state;

        g_return_val_if_fail (self->input_state != NULL, FALSE);

        ret = gst_mmal_video_dec_set_format (decoder,
            gst_video_codec_state_ref (state));
        gst_video_codec_state_unref (state);

        if (ret) {
          gst_video_decoder_set_needs_format (decoder, FALSE);
        }
      }
      GST_VIDEO_DECODER_STREAM_UNLOCK (decoder);
      break;

    default:
      break;
  }

  return ret;
}

/**
 * We ask the decoder to release all buffers it is holding back to us.
 * This also stops the output task thread.  It will be re-started from the
 * next call to handle_frame().
 */
static gboolean
gst_mmal_video_dec_flush_start (GstMMALVideoDec * self, gboolean stop)
{
  MMAL_STATUS_T status;

  gboolean started = self->started;
  gboolean ret = FALSE;

  g_atomic_int_set (&self->flushing, TRUE);

  GST_DEBUG_OBJECT (self, "Flush start...");

  if (!self->dec) {
    GST_WARNING_OBJECT (self, "Decoder component not yet created!");
    return TRUE;
  }

  /* This should wake up input stream thread waiting for an input buffer */
  status = mmal_port_flush (self->dec->input[0]);
  if (status != MMAL_SUCCESS) {
    GST_ERROR_OBJECT (self, "Failed to flush input port: %d (%s)",
        status, mmal_status_to_string (status));
    return FALSE;
  }

  g_mutex_lock (&self->drain_lock);
  GST_DEBUG_OBJECT (self, "Flushing: signalling drain done.");
  self->draining = FALSE;
  g_cond_broadcast (&self->drain_cond);
  g_mutex_unlock (&self->drain_lock);

  /* Yield the "big lock" before blocking on input buffer avail (see below).
     It's easier if we release it here, since we re-aquire at the end.
   */
  GST_VIDEO_DECODER_STREAM_UNLOCK (self);

  if (started) {

    MMAL_BUFFER_HEADER_T *buffer = NULL;

    /* We stop the output task, but it can be blocked waiting on decoded frames
       queue.  We don't want to wait for that to time-out, so send a special
       buffer with a user flag set to tell it to wake-up.
     */

    GST_DEBUG_OBJECT (self, "Waiting for input buffer...");

    if ((buffer = mmal_queue_timedwait (self->input_buffer_pool->queue,
                GST_MMAL_VIDEO_DEC_INPUT_BUFFER_WAIT_FOR_MS)) == NULL) {

      GST_ERROR_OBJECT (self, "Failed to acquire input buffer!");
      goto done;
    }

    GST_DEBUG_OBJECT (self, "Got input buffer.");

    /* "Resets all variables to default values" */
    mmal_buffer_header_reset (buffer);
    buffer->cmd = 0;
    buffer->flags |= MMAL_BUFFER_HEADER_FLAG_USER0;

    /* Avoid trying to unref GstBuffer when releasing MMAL input buffer.
       See: gst_mmal_video_dec_return_input_buffer_to_pool().
     */
    buffer->user_data = NULL;

    buffer->pts = GST_TIME_AS_USECONDS (self->last_upstream_ts);

    GST_DEBUG_OBJECT (self, "Sending wakeup buffer with PTS: %" GST_TIME_FORMAT,
        GST_TIME_ARGS (self->last_upstream_ts));

    if (mmal_port_send_buffer (self->dec->input[0], buffer) != MMAL_SUCCESS) {

      GST_ERROR_OBJECT (self, "Failed to send input buffer to decoder!");
      mmal_buffer_header_release (buffer);
      goto done;
    }

    /* Stop the task (and wait for it to stop) */

    GST_DEBUG_OBJECT (self, "Stopping output task...");
    if (gst_pad_stop_task (GST_VIDEO_DECODER_SRC_PAD (self))) {
      GST_DEBUG_OBJECT (self, "Output task stopped.");
      /* No stream lock needed as output task is not running any more */
      self->started = FALSE;
    } else {
      GST_ERROR_OBJECT (self, "Failed to stop output task!");
      goto done;
    }
  }
  /* end-if (already running) */

  ret = TRUE;

done:
  GST_VIDEO_DECODER_STREAM_LOCK (self);

  return ret;
}

static gboolean
gst_mmal_video_dec_flush_stop (GstMMALVideoDec * self, gboolean stop)
{
  MMAL_PORT_T *input_port = NULL;
  MMAL_PORT_T *output_port = NULL;

  GST_DEBUG_OBJECT (self, "Flush stop...");

  if (!self->dec) {
    GST_WARNING_OBJECT (self, "Decoder component not yet created!");
    return TRUE;
  }

  input_port = self->dec->input[0];
  output_port = self->dec->output[0];

  /* OK, so now we don't need to worry about output side. We can fiddle with
     it's state as the output task was stopped in FLUSH_START.
   */

  if (stop && input_port->is_enabled &&
      mmal_port_disable (input_port) != MMAL_SUCCESS) {
    GST_ERROR_OBJECT (self, "Failed to disable input port!");
    goto flush_failed;
  }

  if (mmal_port_flush (input_port) != MMAL_SUCCESS) {
    GST_ERROR_OBJECT (self, "Failed to set flushing on input port!");
    goto flush_failed;
  }

  /* We take this lock to synchronise access to state belonging to output side */
  GST_PAD_STREAM_LOCK (GST_VIDEO_DECODER_SRC_PAD (self));

  if (stop && output_port->is_enabled &&
      mmal_port_disable (output_port) != MMAL_SUCCESS) {
    GST_ERROR_OBJECT (self, "Failed to disable output port!");
    goto output_flush_failed;
  }

  if (mmal_port_flush (output_port) != MMAL_SUCCESS) {
    GST_ERROR_OBJECT (self, "Failed to set flushing on output port!");
    goto output_flush_failed;
  }

  if (self->decoded_frames_queue != NULL) {

    MMAL_BUFFER_HEADER_T *buffer = NULL;

    /* Free any decoded frames */
    while ((buffer = mmal_queue_get (self->decoded_frames_queue))) {

      GST_DEBUG_OBJECT (self, "Freeing decoded frame: %p", buffer);
      mmal_buffer_header_release (buffer);
    }
  }

  if (stop) {
    /* At this point, we expect to have all our buffers back. */
    uint32_t input_buffers;

    if (self->input_buffer_pool != NULL) {

      input_buffers = mmal_queue_length (self->input_buffer_pool->queue);

      if (input_buffers != input_port->buffer_num) {
        GST_ERROR_OBJECT (self,
            "Failed to reclaim all input buffers (%d out of %d)",
            input_buffers, input_port->buffer_num);
        goto output_flush_failed;
      }
    }
  }

  GST_PAD_STREAM_UNLOCK (GST_VIDEO_DECODER_SRC_PAD (self));

  self->last_upstream_ts = 0;
  self->output_flow_ret = GST_FLOW_OK;

  g_atomic_int_set (&self->flushing, FALSE);

  return TRUE;

output_flush_failed:

  GST_PAD_STREAM_UNLOCK (GST_VIDEO_DECODER_SRC_PAD (self));

flush_failed:

  return FALSE;
}

/**
 * This function is adapted from gst-omx.
 *
 * It is a safeguard against "old" frames that have, for whatever reason either
 * never been submitted to the decoder, or no output frame was produced
 * for them.
 *
 * We drop the frames to stop GstVideoDecoder hanging-on to them, and eating-up
 * all the memory.
 *
 * This function will be called from gst_mmal_video_dec_output_task_loop().
 *
 * Will be called *with* decoder stream lock held.
 */
static void
gst_mmal_video_dec_output_clean_older_frames (GstMMALVideoDec * self,
    MMAL_BUFFER_HEADER_T * buf, GList * frames)
{
  GList *l;
  GstClockTime timestamp = gst_util_uint64_scale (buf->pts, GST_SECOND,
      G_USEC_PER_SEC);

  GST_DEBUG_OBJECT (self, "Cleaning older than: %" GST_TIME_FORMAT,
      GST_TIME_ARGS (timestamp));

  if (GST_CLOCK_TIME_IS_VALID (timestamp)) {
    /* We could release all frames stored with pts < timestamp since the
     * decoder will likely output frames in display order */
    for (l = frames; l; l = l->next) {
      GstVideoCodecFrame *tmp = l->data;

      if (tmp->pts < timestamp) {
        gst_video_decoder_release_frame (GST_VIDEO_DECODER (self), tmp);
        GST_LOG_OBJECT (self,
            "Discarding ghost frame %p (#%d) PTS:%" GST_TIME_FORMAT " DTS:%"
            GST_TIME_FORMAT, tmp, tmp->system_frame_number,
            GST_TIME_ARGS (tmp->pts), GST_TIME_ARGS (tmp->dts));
      } else {
        gst_video_codec_frame_unref (tmp);
      }
    }
  } else {
    /* We will release all frames with invalid timestamp because we don't even
     * know if they will be output some day. */
    for (l = frames; l; l = l->next) {
      GstVideoCodecFrame *tmp = l->data;

      if (!GST_CLOCK_TIME_IS_VALID (tmp->pts)) {
        gst_video_decoder_release_frame (GST_VIDEO_DECODER (self), tmp);
        GST_LOG_OBJECT (self,
            "Discarding frame %p (#%d) with invalid PTS:%" GST_TIME_FORMAT
            " DTS:%" GST_TIME_FORMAT, tmp, tmp->system_frame_number,
            GST_TIME_ARGS (tmp->pts), GST_TIME_ARGS (tmp->dts));
      } else {
        gst_video_codec_frame_unref (tmp);
      }
    }
  }

  g_list_free (frames);
}


/**
 * This is lifted from gst-omx
 *
 * The reason it's needed is that the decoder won't always output frames in the
 * order they were presented because decode order does not always equal display
 * order.
 *
 * So, when a frame is decoded, it's not just as simple as calling
 * finish_frame() passing the frame given in the last handle_frame() call.
 *
 * Instead, we need to locate the pending frame with the nearest PTS to the
 * frame that has been decoded.
 *
 * NOTE: We could perhaps avoid this if userdata set on the input buffers were
 * propagated to the output buffers - something to check.
 *
 * This function will be called from gst_mmal_video_dec_output_task_loop().
 *
 * Will be called *with* decoder stream lock held.
 */
GstVideoCodecFrame *
gst_mmal_video_dec_output_find_nearest_frame (MMAL_BUFFER_HEADER_T * buf,
    GList * frames)
{
  GstVideoCodecFrame *best = NULL;
  GstClockTimeDiff best_diff = G_MAXINT64;
  GstClockTime timestamp;
  GList *l;

  timestamp = gst_util_uint64_scale (buf->pts, GST_SECOND, G_USEC_PER_SEC);

  for (l = frames; l; l = l->next) {

    GstVideoCodecFrame *tmp = l->data;
    GstClockTimeDiff diff = ABS (GST_CLOCK_DIFF (timestamp, tmp->pts));

    if (diff < best_diff) {
      best = tmp;
      best_diff = diff;

      if (diff == 0) {
        break;
      }
    }
  }

  if (best) {
    gst_video_codec_frame_ref (best);
  }

  g_list_foreach (frames, (GFunc) gst_video_codec_frame_unref, NULL);
  g_list_free (frames);

  return best;
}


/**
 * Copy MMAL ouput buffer to GstBuffer
 *
 * This is lifted (and adapted) from gst-omx (gstomxvideodec.c), with some
 * simplification, since we are only supporting I420.
 *
 * It is used by gst_mmal_video_dec_output_task_loop(), when not
 * using opaque output buffers and doing dumb copy from decoder output buffer to
 * frame GstBuffer.
 *
 * Will be called *with* decoder stream lock.
 */
static gboolean
gst_mmal_video_dec_output_fill_buffer (GstMMALVideoDec * self,
    MMAL_BUFFER_HEADER_T * inbuf, GstBuffer * outbuf)
{
  GstVideoCodecState *state =
      gst_video_decoder_get_output_state (GST_VIDEO_DECODER (self));

  GstVideoInfo *vinfo = &state->info;

  MMAL_PORT_T *output_port = self->dec->output[0];
  MMAL_ES_FORMAT_T *output_format = output_port->format;

  gboolean ret = FALSE;
  GstVideoFrame frame;

  if (vinfo->width != output_format->es->video.crop.width ||
      vinfo->height != output_format->es->video.crop.height) {
    GST_ERROR_OBJECT (self, "Resolution do not match: port=%ux%u vinfo=%dx%d",
        (guint) output_format->es->video.crop.width,
        (guint) output_format->es->video.crop.height, vinfo->width,
        vinfo->height);
    goto done;
  }

  /* This is the simple case.  The strides and offset of the MMAL & GST buffers
     match, so a single memcpy will suffice.
   */
  if (gst_buffer_get_size (outbuf) == inbuf->length) {

    if (gst_buffer_fill (outbuf, 0,
            inbuf->data + inbuf->offset, inbuf->length) != inbuf->length) {

      GST_ERROR_OBJECT (self, "Failed to fill output buffer");
      goto done;
    }

    ret = TRUE;
    goto done;
  }

  GST_DEBUG_OBJECT (self, "\n"
      "MMAL Buffer size: %d\nGstBuffer size:   %d\n", inbuf->alloc_size,
      gst_buffer_get_size (outbuf));

  /* More complicated case. Different buffer sizes.  Strides and offsets don't
     match.
   */
  if (gst_video_frame_map (&frame, vinfo, outbuf, GST_MAP_WRITE)) {

    const guint nstride = output_format->es->video.width;
    const guint nslice = output_format->es->video.height;

    guint src_stride[GST_VIDEO_MAX_PLANES] = { nstride, 0, };
    guint src_size[GST_VIDEO_MAX_PLANES] = { nstride * nslice, 0, };
    gint dst_width[GST_VIDEO_MAX_PLANES] = { 0, };
    gint dst_height[GST_VIDEO_MAX_PLANES] =
        { GST_VIDEO_INFO_HEIGHT (vinfo), 0, };
    const guint8 *src;
    guint p;

    GST_DEBUG_OBJECT (self, "Different strides.\n"
        "MMAL buffer size: %d\n"
        "GST buffer size: %d\n", inbuf->length, gst_buffer_get_size (outbuf));


    /* These are for I420 */
    dst_width[0] = GST_VIDEO_INFO_WIDTH (vinfo);
    src_stride[1] = nstride / 2;
    src_size[1] = (src_stride[1] * nslice) / 2;
    dst_width[1] = GST_VIDEO_INFO_WIDTH (vinfo) / 2;
    dst_height[1] = GST_VIDEO_INFO_HEIGHT (vinfo) / 2;
    src_stride[2] = nstride / 2;
    src_size[2] = (src_stride[1] * nslice) / 2;
    dst_width[2] = GST_VIDEO_INFO_WIDTH (vinfo) / 2;
    dst_height[2] = GST_VIDEO_INFO_HEIGHT (vinfo) / 2;

    src = inbuf->data + inbuf->offset;

    /* Plane-by-plane */
    for (p = 0; p < GST_VIDEO_INFO_N_PLANES (vinfo); p++) {
      const guint8 *data;
      guint8 *dst;
      guint h;

      dst = GST_VIDEO_FRAME_PLANE_DATA (&frame, p);
      data = src;

      /* Line-by-line, in each plane */
      for (h = 0; h < dst_height[p]; h++) {
        memcpy (dst, data, dst_width[p]);
        dst += GST_VIDEO_INFO_PLANE_STRIDE (vinfo, p);
        data += src_stride[p];
      }
      src += src_size[p];
    }

    gst_video_frame_unmap (&frame);
    ret = TRUE;
  } else {
    GST_ERROR_OBJECT (self, "Can't map output buffer to frame");
    goto done;
  }

done:
  if (ret) {
    GST_BUFFER_PTS (outbuf) =
        gst_util_uint64_scale (inbuf->pts, GST_SECOND, G_USEC_PER_SEC);
  }

  gst_video_codec_state_unref (state);

  return ret;
}



/**
 * This just feeds any available output buffers to the decoder output port.
 * It's called only from gst_mmal_video_dec_output_task_loop().
 *
 * Will be called *without* decoder stream lock.
 */
static gboolean
gst_mmal_video_dec_output_populate_output_port (GstMMALVideoDec * self)
{
  MMAL_BUFFER_HEADER_T *buffer = NULL;

  /* Send empty buffers to the output port of the decoder to allow the decoder
     to start producing frames as soon as it gets input data.
   */

  if (self->output_buffer_pool != NULL) {

    while ((buffer = mmal_queue_get (self->output_buffer_pool->queue)) != NULL) {

      GST_DEBUG_OBJECT (self, "Sending empty buffer to output port...");

      mmal_buffer_header_reset (buffer);

      if (mmal_port_send_buffer (self->dec->output[0], buffer) != MMAL_SUCCESS) {
        GST_ERROR_OBJECT (self,
            "Failed to send empty output buffer to outport!");
        return FALSE;
      }
    }
  }

  return TRUE;
}


/**
 * Reconfigure the output port settings.
 *
 * This is called from gst_mmal_video_dec_output_task_loop() on the output task
 * thread in response to a caps change on the sink.
 *
 * It will be called *without* the big decoder stream lock held.
 */
static GstFlowReturn
gst_mmal_video_dec_output_reconfigure_output_port (GstMMALVideoDec * self,
    gboolean update_src_caps)
{
  MMAL_PORT_T *output_port = NULL;
  MMAL_ES_FORMAT_T *output_format = NULL;
  GstFlowReturn flow_ret = GST_FLOW_OK;

  GST_DEBUG_OBJECT (self, "Reconfiguring output port...");

  output_port = self->dec->output[0];

  if (output_port->is_enabled && mmal_port_disable (output_port) !=
      MMAL_SUCCESS) {
    GST_ERROR_OBJECT (self, "Failed to disable output port!");
    return GST_FLOW_ERROR;
  }

  mmal_port_flush (output_port);

  /* We need to update src caps here, and go through the allocation query
     with downstream.  Otherwise, we can't know whether we should use opaque
     buffers or not on output port.

     The exception to this rule is when we are being called because negotiation
     has already happened just previously and we have a change in buffer type
     (opaque vs plain).  We are called with update_src_caps == FALSE in that
     case.
   */

  if (update_src_caps
      && (flow_ret =
          gst_mmal_video_dec_output_update_src_caps (self)) != GST_FLOW_OK) {
    return flow_ret;
  }

  output_format = output_port->format;

  GST_DEBUG_OBJECT (self, "Buffers recommended (out): %u",
      output_port->buffer_num_recommended);

  if (mmal_port_parameter_set_boolean (output_port, MMAL_PARAMETER_ZERO_COPY,
          self->opaque) != MMAL_SUCCESS) {

    GST_ERROR_OBJECT (self, "Failed to set/unset zero-copy on output port!");
    return GST_FLOW_ERROR;
  }

  if (self->opaque) {

    GST_DEBUG_OBJECT (self, "Configuring opaque buffers on output port...");

    output_format->encoding = MMAL_ENCODING_OPAQUE;

    if (mmal_port_parameter_set_uint32 (self->dec->output[0],
            MMAL_PARAMETER_EXTRA_BUFFERS,
            GST_MMAL_VIDEO_DEC_EXTRA_OUTPUT_BUFFERS_OPAQUE_MODE) !=
        MMAL_SUCCESS) {

      GST_ERROR_OBJECT (self,
          "Failed to set extra-buffer count on output port!");
      return GST_FLOW_ERROR;
    }

    if (mmal_port_format_commit (output_port) != MMAL_SUCCESS) {
      GST_ERROR_OBJECT (self, "Failed to commit output port format.");
      return GST_FLOW_ERROR;
    }

  } else {

    output_format->encoding = MMAL_ENCODING_I420;

    if (mmal_port_format_commit (output_port) != MMAL_SUCCESS) {
      GST_ERROR_OBJECT (self, "Failed to commit output port format.");
      return GST_FLOW_ERROR;
    }
  }

  output_port->buffer_num = GST_MMAL_NUM_OUTPUT_BUFFERS;

  /*
     NOTE: We can also receive notification of output port format changes via
     in-band events.  It seems we don't get those most of the time though, so
     I'm also dealing with the output port here.
   */

  output_port->buffer_size = GST_MMAL_MAX_I420_BUFFER_SIZE;

  {
    MMAL_BUFFER_HEADER_T *buffer;

    if (self->decoded_frames_queue != NULL) {
      /* Free any decoded frames */
      while ((buffer = mmal_queue_get (self->decoded_frames_queue))) {

        GST_DEBUG_OBJECT (self, "Freeing decoded frame: %p", buffer);
        mmal_buffer_header_release (buffer);
      }

      mmal_queue_destroy (self->decoded_frames_queue);
    }

    GST_DEBUG_OBJECT (self, "Creating decoded frames queue...");
    self->decoded_frames_queue = mmal_queue_create ();
    output_port->userdata = (void *) self;
  }

  GST_DEBUG_OBJECT (self, "Reconfiguring output buffer pool...");

  if (self->opaque) {
    if (!self->output_buffer_pool_opaque) {
      self->output_buffer_pool_opaque = mmal_port_pool_create (output_port,
          GST_MMAL_NUM_OUTPUT_BUFFERS, GST_MMAL_MAX_I420_BUFFER_SIZE);
    }
  } else if (!self->output_buffer_pool_plain) {
    self->output_buffer_pool_plain =
        mmal_pool_create (GST_MMAL_NUM_OUTPUT_BUFFERS,
        GST_MMAL_MAX_I420_BUFFER_SIZE);
  }

  self->output_buffer_pool = self->opaque ?
      self->output_buffer_pool_opaque : self->output_buffer_pool_plain;

  if (mmal_port_enable (output_port,
          &gst_mmal_video_dec_mmal_queue_decoded_frame) != MMAL_SUCCESS) {

    GST_ERROR_OBJECT (self, "Failed to enable output port!");
    return GST_FLOW_ERROR;
  }

  GST_DEBUG_OBJECT (self, "Output port info:");
  gst_mmal_print_port_info (output_port, GST_ELEMENT (self));

  return GST_FLOW_OK;
}


/**
 * This is called repeatedly from the src pad task.
 * The src pad stream lock is held during the call.
 *
 * It's job is to populate the decoder output port with empty buffers and
 * process output frames from the decoder.
 */
static void
gst_mmal_video_dec_output_task_loop (GstMMALVideoDec * self)
{
  GstFlowReturn flow_ret = GST_FLOW_OK;

  MMAL_BUFFER_HEADER_T *buffer = NULL;
  GstVideoCodecFrame *frame = NULL;
  GstClockTimeDiff deadline;
  gboolean was_opaque;
  uint32_t wait_for_buffer_timeout_ms = 250;

  /* We don't need to hold decoder lock for most of output reconfiguration,
     although update_src_caps() will acquire it.

     N.B. access to caps_changed is synchronised on src pad mutex, and that is
     automatically acquired before each entry to this function.

     See: gst_mmal_video_dec_set_format() for where caps_changed is set.
   */
  if (self->output_buffer_pool == NULL || self->caps_changed) {

    /* Output structures not created yet.  To avoid some contention with input
       side we will kick that off here on our task thread.
     */
    if ((flow_ret = gst_mmal_video_dec_output_reconfigure_output_port (self,
                TRUE)) != GST_FLOW_OK) {

      GST_ERROR_OBJECT (self, "Output port reconfiguration failed!");

      GST_VIDEO_DECODER_STREAM_LOCK (self);

      self->output_flow_ret = flow_ret;
      gst_pad_push_event (GST_VIDEO_DECODER_SRC_PAD (self),
          gst_event_new_eos ());
      gst_pad_pause_task (GST_VIDEO_DECODER_SRC_PAD (self));
      self->started = FALSE;

      GST_VIDEO_DECODER_STREAM_UNLOCK (self);

      return;
    }

    /* This is used below to trigger another call to update_src_caps() when we
       receive first output frame from decoder.
     */
    self->output_reconfigured = TRUE;
  }

  /* Don't need to hold lock whilst populating output port. */

  /* Populate output with buffers. */
  if (!gst_mmal_video_dec_output_populate_output_port (self)) {

    GST_ERROR_OBJECT (self, "Populating output port failed!");

    GST_VIDEO_DECODER_STREAM_LOCK (self);

    self->output_flow_ret = GST_FLOW_ERROR;
    gst_pad_push_event (GST_VIDEO_DECODER_SRC_PAD (self), gst_event_new_eos ());
    gst_pad_pause_task (GST_VIDEO_DECODER_SRC_PAD (self));
    self->started = FALSE;

    GST_VIDEO_DECODER_STREAM_UNLOCK (self);

    return;
  }

  /* Handle decoded frames   We wait for at least one frame to be ready before
     returning to avoid spinning.
   */

  if ((buffer = mmal_queue_timedwait (self->decoded_frames_queue,
              wait_for_buffer_timeout_ms)) == NULL) {

    /* Not an error, just nothing ready yet. */

    GST_DEBUG_OBJECT (self, "Timed-out waiting for output decoded frame.");

  } else {

    /* We're fiddling with state of decoder baseclass, so take big lock  */
    GST_VIDEO_DECODER_STREAM_LOCK (self);

    /* N.B. We never seem to see any events, so these TODO's aren't quite as bad
       as they look!
     */
    if (buffer->cmd) {

      GST_DEBUG_OBJECT (self, "Received command.");

      switch (buffer->cmd) {

        case MMAL_EVENT_EOS:
          GST_DEBUG_OBJECT (self, "Got EOS.");
          flow_ret = GST_FLOW_EOS;
          break;
        case MMAL_EVENT_ERROR:
          /* TODO: Pull-out some useful info. */
          GST_DEBUG_OBJECT (self, "TODO: Got error.");
          flow_ret = GST_FLOW_ERROR;
          break;
        case MMAL_EVENT_FORMAT_CHANGED:
          GST_DEBUG_OBJECT (self, "TODO: Output port format changed.");
          break;
        case MMAL_EVENT_PARAMETER_CHANGED:
          GST_DEBUG_OBJECT (self, "TODO: Parameter changed.");
          break;
      }

    } else if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_USER0) {

      /* This is just a wakeup sent by flush() */

      GST_DEBUG_OBJECT (self, "Got wakeup buffer.  Must be flushing...");

    } else {

      GST_DEBUG_OBJECT (self, "Handling decoded frame: %" GST_TIME_FORMAT,
          GST_TIME_ARGS (gst_util_uint64_scale (buffer->pts, GST_SECOND,
                  G_USEC_PER_SEC)));

      if ((buffer->flags & MMAL_BUFFER_HEADER_FLAG_EOS)) {

        /* NOTE: EOS buffer might still have a final payload we need to process.
           At the moment, I'm not expecting this to be the case, since EOS comes
           from us sending (empty) EOS input buffer in drain()
         */
        GST_DEBUG_OBJECT (self, "Buffer signals EOS.");
        flow_ret = GST_FLOW_EOS;
      }

      /* See EOS comment, above */
      if (buffer->length) {

        /* Did we have a caps change via set_format() just previously?
           Now that the decoder has seen a frame with the "new" format, it
           should have some idea about the framerate and whether interlaced.
           It doesn't know these things before seeing frames, and we can't
           really rely on upstream elements to get this right, as we're seeing
           framerate of "200/1" on input caps, and "progressive" when actually
           interlaced.
         */
        if (self->output_reconfigured) {

          self->output_reconfigured = FALSE;

          was_opaque = self->opaque;

          if ((flow_ret = gst_mmal_video_dec_output_update_src_caps (self)) !=
              GST_FLOW_OK) {
            GST_ERROR_OBJECT (self, "Failed to update src caps!");
          }

          if (self->opaque != was_opaque) {

            /* Caps can change slightly after we've seen first frame.
               We might go from progressive to interlace, for example.
               It's possible that we will then fail to negotiate opaque, when
               we had managed that previously, or the other way around (think
               S/W deinterlace in-between decoder and sink switching from
               pass-through to active).

               If this happens, we will have to reconfigure output side, yet
               again.  In any case, we can't send this frame now.
             */

            frame = gst_mmal_video_dec_output_find_nearest_frame (buffer,
                gst_video_decoder_get_frames (GST_VIDEO_DECODER (self)));

            if (frame) {
              gst_video_decoder_drop_frame (GST_VIDEO_DECODER (self), frame);
              frame = NULL;
            }

            /* N.B. if we leave this until after the reconfigure it will be
               too late!
             */
            mmal_buffer_header_release (buffer);
            buffer = NULL;

            GST_WARNING_OBJECT (self, "Change in buffer type from %s to %s! "
                "Trying to reconfigure...", (was_opaque ? "opaque" : "plain"),
                (self->opaque ? "opaque" : "plain"));

            /* We do reconfigure again, but note that we don't go through
               downstream caps negotiation this time (that's what the FALSE is
               for).
             */
            if ((flow_ret =
                    gst_mmal_video_dec_output_reconfigure_output_port (self,
                        FALSE)) != GST_FLOW_OK) {
              GST_ERROR_OBJECT (self, "Failed to reconfigure output port.");
            }

            GST_ERROR_OBJECT (self,
                "TODO: Switching between plain and opaque not supported");
            flow_ret = GST_FLOW_ERROR;
          }
        }

        if (buffer != NULL) {

          frame = gst_mmal_video_dec_output_find_nearest_frame (buffer,
              gst_video_decoder_get_frames (GST_VIDEO_DECODER (self)));

          /* So we have a timestamped MMAL buffer and get, or not, corresponding
             frame.

             Assuming decoder output frames in display order, frames preceding
             this frame could be discarded as they seems useless due to e.g.
             interlaced stream, corrupted input data.

             In any cases, not likely to be seen again. so drop it before they
             pile up and use all the memory.
           */
          gst_mmal_video_dec_output_clean_older_frames (self, buffer,
              gst_video_decoder_get_frames (GST_VIDEO_DECODER (self)));
        }

        if (frame && ((deadline = gst_video_decoder_get_max_decode_time
                    (GST_VIDEO_DECODER (self), frame)) < 0)) {

          GST_WARNING_OBJECT (self,
              "Frame is too late, dropping (deadline %" GST_TIME_FORMAT ")",
              GST_TIME_ARGS (-deadline));

          flow_ret =
              gst_video_decoder_drop_frame (GST_VIDEO_DECODER (self), frame);
          frame = NULL;

        } else if (!frame) {

          if (buffer != NULL) {
            /* TODO: Handle this properly. gst-omx code reckons it can happen. */
            GST_ERROR_OBJECT (self,
                "Failed to find matching frame for PTS: %" G_GUINT64_FORMAT,
                (guint64) buffer->pts);
          }

        } else {

          /* So we've found a matching frame then, and it's not too late ? */

          /* We do dumb thing to start with.  Allocate a new GstBuffer, and copy
             the output frame. Later we will introduce opaque buffers.
           */

          if ((flow_ret =
                  gst_video_decoder_allocate_output_frame (GST_VIDEO_DECODER
                      (self), frame)) != GST_FLOW_OK) {

            GST_ERROR_OBJECT (self, "Failed to allocate output frame!");

          } else {

            /* Set any buffer flags for interlace  See: update_src_caps() */
            GST_BUFFER_FLAG_SET (frame->output_buffer,
                self->output_buffer_flags);

            if (self->opaque) {

              /* Using opaque output buffers.  So the GstMemory in the GstBuffer
                 should have been allocated by our special allocator.
                 We just need to fill-in the pointer to the MMAL header.
               */

              GstMemory *mem = gst_buffer_peek_memory (frame->output_buffer,
                  0);

              if (mem == NULL || !gst_is_mmal_opaque_memory (mem)) {

                GST_ERROR_OBJECT (self, "Expected MMAL Opaque GstMemory!");
                flow_ret = GST_FLOW_ERROR;

              } else {
                gst_mmal_opaque_mem_set_mmal_header (mem, buffer);
              }
            }

            /* Only copy frame if not using opaque. */
            else if (!gst_mmal_video_dec_output_fill_buffer (self, buffer,
                    frame->output_buffer)) {

              /* FAIL */

              GST_ERROR_OBJECT (self,
                  "Failed to copy MMAL output buffer to frame!");

              gst_buffer_replace (&frame->output_buffer, NULL);

              flow_ret =
                  gst_video_decoder_drop_frame (GST_VIDEO_DECODER (self),
                  frame);
            }

            if (flow_ret == GST_FLOW_OK) {

              GST_DEBUG_OBJECT (self, "Finishing frame PTS %" GST_TIME_FORMAT,
                  GST_TIME_ARGS (gst_util_uint64_scale (buffer->pts,
                          GST_SECOND, G_USEC_PER_SEC)));

              flow_ret =
                  gst_video_decoder_finish_frame (GST_VIDEO_DECODER (self),
                  frame);
            }
          }
          /* end-else (managed to allocate output frame) */
        }
        /* end-else (matching frame and not too late) */

        frame = NULL;
      }
      /* end-else (buffer has payload) */
    }
    /* end-else (not an event) */

    if (flow_ret == GST_FLOW_EOS || g_atomic_int_get (&self->flushing)) {

      g_mutex_lock (&self->drain_lock);

      if (self->draining) {

        /* Input side will be waiting on condvar in drain() */

        GST_DEBUG_OBJECT (self, "Drained.");
        self->draining = FALSE;
        g_cond_broadcast (&self->drain_cond);
        flow_ret = GST_FLOW_OK;

        /*
           Pause task so we don't re-enter this loop and try output port
           reconfiguration before input side is finished with format change, or
           whatever it is up to.
         */
        gst_pad_pause_task (GST_VIDEO_DECODER_SRC_PAD (self));
        self->started = FALSE;
      } else {

        GST_DEBUG_OBJECT (self, "EOS seen on output thread.");
      }

      g_mutex_unlock (&self->drain_lock);
    }

    self->output_flow_ret = flow_ret;

    if (flow_ret != GST_FLOW_OK) {
      gst_pad_push_event (GST_VIDEO_DECODER_SRC_PAD (self),
          gst_event_new_eos ());
      gst_pad_pause_task (GST_VIDEO_DECODER_SRC_PAD (self));
      self->started = FALSE;
    }

    GST_VIDEO_DECODER_STREAM_UNLOCK (self);
  }
  /* end-else (a decoded frame or command is ready) */

  /* We will always get here, so buffer should always be freed. */
  if (buffer != NULL) {

    /* Release MMAL buffer back to the output pool.
       NOTE: We do this whether it's a "command" or an actual frame.
     */
    mmal_buffer_header_release (buffer);
  }
}


static GstFlowReturn
gst_mmal_video_dec_send_codec_data (GstMMALVideoDec * self,
    GstBuffer * codec_data, int64_t pts_microsec, int64_t dts_microsec)
{
  GstFlowReturn flow_ret = GST_FLOW_OK;
  MappedBuffer *mb = NULL;
  MMAL_BUFFER_HEADER_T *mmal_buffer = NULL;
  MMAL_PORT_T *input_port = NULL;
  guint data_size;

  g_return_val_if_fail (self != NULL, GST_FLOW_ERROR);
  g_return_val_if_fail (codec_data != NULL, GST_FLOW_ERROR);
  g_return_val_if_fail (self->dec != NULL, GST_FLOW_ERROR);

  data_size = gst_buffer_get_size (codec_data);

  input_port = self->dec->input[0];

  GST_DEBUG_OBJECT (self, "Waiting for input buffer for codec data...");

  if ((mmal_buffer = mmal_queue_timedwait (self->input_buffer_pool->queue,
              GST_MMAL_VIDEO_DEC_INPUT_BUFFER_WAIT_FOR_MS)) == NULL) {

    GST_ERROR_OBJECT (self, "Failed to acquire input buffer for codec data!");
    flow_ret = GST_FLOW_ERROR;
    goto done;
  }

  GST_DEBUG_OBJECT (self, "Got input MMAL buffer (%p) for codec data",
      mmal_buffer);

  /* "Resets all variables to default values" */
  mmal_buffer_header_reset (mmal_buffer);
  mmal_buffer->cmd = 0;

  /* Use MMAL buffer payload for mapped GStreamer buffer info */
  mb = (MappedBuffer *) mmal_buffer->data;

  mb->buffer = gst_buffer_ref (codec_data);

  if (!gst_buffer_map (mb->buffer, &mb->map_info, GST_MAP_READ)) {

    gst_buffer_unref (mb->buffer);
    mmal_buffer_header_release (mmal_buffer);

    GST_ERROR_OBJECT (self, "Failed to map frame input buffer for reading");
    flow_ret = GST_FLOW_ERROR;
    goto done;
  }

  mb->next = NULL;
  mb->previous = NULL;

  mmal_buffer->data = mb->map_info.data;
  mmal_buffer->alloc_size = input_port->buffer_size;
  mmal_buffer->length = data_size;
  mmal_buffer->user_data = mb;

  /* Set PTS */
  mmal_buffer->pts = pts_microsec;
  mmal_buffer->dts = dts_microsec;

  /* Flags */
  mmal_buffer->flags |= MMAL_BUFFER_HEADER_FLAG_CONFIG;
  mmal_buffer->flags |= MMAL_BUFFER_HEADER_FLAG_FRAME_END;

  GST_DEBUG_OBJECT (self, "Sending codec data of size %d to MMAL...",
      data_size);

  /* Now send the buffer to decoder. */
  if (mmal_port_send_buffer (input_port, mmal_buffer) != MMAL_SUCCESS) {

    GST_ERROR_OBJECT (self,
        "Failed to send input buffer (%p) with codec data to decoder!",
        mmal_buffer);

    gst_mmal_video_dec_mmal_return_input_buffer_to_pool (input_port,
        mmal_buffer);

    flow_ret = GST_FLOW_ERROR;
  }

done:
  return flow_ret;
}


/**
 * N.B. This is called by the video decoder baseclass when there is some input
 * data to decode.
 *
 * It is responsible for feeding input frames to the decoder, and also starts
 * the src pad (output) task when the first sync frame is encountered.
 */
static GstFlowReturn
gst_mmal_video_dec_handle_frame (GstVideoDecoder * decoder,
    GstVideoCodecFrame * frame)
{
  MappedBuffer *mapped_gst_buffer = NULL;
  MappedBuffer *previous_mapped_gst_buffer = NULL;
  MMAL_BUFFER_HEADER_T *mmal_buffer = NULL;
  guint input_size = 0;
  guint input_offset = 0;
  int64_t pts_microsec = 0;
  int64_t dts_microsec = 0;
  GstFlowReturn flow_ret = GST_FLOW_OK;
  uint32_t mmal_buffer_flags = 0;

  GstMMALVideoDec *self = GST_MMAL_VIDEO_DEC (decoder);

  /* Handle this input frame. */

  if (g_atomic_int_get (&self->flushing)) {
    GST_DEBUG_OBJECT (self, "Flushing: not decoding input frame.");
    flow_ret = GST_FLOW_FLUSHING;
    goto done_locked;
  }

  /* If there's already some error flagged by output side then give up. */

  if (self->output_flow_ret != GST_FLOW_OK) {

    gst_video_codec_frame_unref (frame);
    return self->output_flow_ret;
  }

  if (!self->started) {

    if (!GST_VIDEO_CODEC_FRAME_IS_SYNC_POINT (frame)) {

      gst_video_decoder_drop_frame (GST_VIDEO_DECODER (self), frame);

      /* Jumping straight out here, rather than goto as we don't need to unref
         frame.
       */
      return GST_FLOW_OK;

    } else {

      /* Output task not running yet.  Start it. */

      GST_DEBUG_OBJECT (self, "Starting output task...");

      mmal_buffer_flags |= MMAL_BUFFER_HEADER_FLAG_DISCONTINUITY;

      gst_pad_start_task (GST_VIDEO_DECODER_SRC_PAD (self),
          (GstTaskFunction) gst_mmal_video_dec_output_task_loop, decoder, NULL);

      self->started = TRUE;
    }
  }

  /* Avoid blocking the output side whilst we are waiting for MMAL input buffer
     headers.  Otherwise, we will not be able to process output frames whilst
     lock is held, which will prevent the decoder from releasing input buffers
     back to us, and we end-up in a deadlock.

     We don't need to be holding the big lock for what follows.
   */
  GST_VIDEO_DECODER_STREAM_UNLOCK (self);

  input_size = gst_buffer_get_size (frame->input_buffer);

  /* GST timestamp is in nanosecs */

  if (GST_CLOCK_TIME_IS_VALID (frame->pts)) {

    GST_DEBUG_OBJECT (self, "PTS: %" GST_TIME_FORMAT,
        GST_TIME_ARGS (frame->pts));

    pts_microsec = GST_TIME_AS_USECONDS (frame->pts);

    /* N.B. last_upstream_ts is used by drain() */
    self->last_upstream_ts = frame->pts;

    if (frame->duration != GST_CLOCK_TIME_NONE) {
      self->last_upstream_ts += frame->duration;
    }

  } else {
    GST_DEBUG_OBJECT (self, "PTS: UNKNOWN!");
    pts_microsec = MMAL_TIME_UNKNOWN;
  }

  if (GST_CLOCK_TIME_IS_VALID (frame->dts)) {

    GST_DEBUG_OBJECT (self, "DTS: %" GST_TIME_FORMAT,
        GST_TIME_ARGS (frame->dts));

    dts_microsec = GST_TIME_AS_USECONDS (frame->dts);

  } else {
    GST_DEBUG_OBJECT (self, "DTS: UNKNOWN!");
    dts_microsec = MMAL_TIME_UNKNOWN;
  }

  /* First, send any extra codec data if present */
  if (self->codec_data) {

    flow_ret = gst_mmal_video_dec_send_codec_data (self, self->codec_data,
        pts_microsec, dts_microsec);

    /* empty codec_data as we have consumed it */
    gst_buffer_replace (&self->codec_data, NULL);

    /* now check error code */
    if (flow_ret != GST_FLOW_OK) {
      GST_ERROR_OBJECT (self, "Failed to send codec data!");
      goto done;
    }
  }


  /* Now, handle the frame */
  while (input_offset < input_size) {

    /* We have to wait for an input buffer.
       We make this a timed-wait as we don't want to get stuck here forever! */

    GST_DEBUG_OBJECT (self, "Waiting for input buffer...");

    mmal_buffer = mmal_queue_wait (self->input_buffer_pool->queue);
    if (mmal_buffer == NULL) {

      GST_ERROR_OBJECT (self, "Failed to acquire input buffer!");
      flow_ret = GST_FLOW_ERROR;
      goto done;
    }

    GST_DEBUG_OBJECT (self, "Got input MMAL buffer (%p)", mmal_buffer);

    if (g_atomic_int_get (&self->flushing)) {
      GST_DEBUG_OBJECT (self, "Flushing: not decoding input frame.");
      mmal_buffer_header_release (mmal_buffer);
      flow_ret = GST_FLOW_FLUSHING;
      goto done;
    }

    /* "Resets all variables to default values" */
    mmal_buffer_header_reset (mmal_buffer);
    mmal_buffer->cmd = 0;

    /* Use MMAL buffer payload for mapped GStreamer buffer info */
    mapped_gst_buffer = (MappedBuffer *) mmal_buffer->data;

    if (previous_mapped_gst_buffer != NULL) {

      mapped_gst_buffer->map_info = previous_mapped_gst_buffer->map_info;
      mapped_gst_buffer->buffer = previous_mapped_gst_buffer->buffer;
      previous_mapped_gst_buffer->next = mapped_gst_buffer;

    } else {

      mapped_gst_buffer->buffer = gst_buffer_ref (frame->input_buffer);

      if (!gst_buffer_map (mapped_gst_buffer->buffer,
              &mapped_gst_buffer->map_info, GST_MAP_READ)) {

        gst_buffer_unref (mapped_gst_buffer->buffer);
        mmal_buffer_header_release (mmal_buffer);

        GST_ERROR_OBJECT (self, "Failed to map frame input buffer for reading");
        flow_ret = GST_FLOW_ERROR;
        goto done;
      }
    }

    mapped_gst_buffer->next = NULL;
    mapped_gst_buffer->previous = previous_mapped_gst_buffer;

    mmal_buffer->data = mapped_gst_buffer->map_info.data + input_offset;
    mmal_buffer->alloc_size = self->dec->input[0]->buffer_size;
    mmal_buffer->length = MIN (input_size - input_offset,
        mmal_buffer->alloc_size);
    mmal_buffer->offset = 0;

    /* Need to save this so we can restore in the MMAL buffer release callback */
    mmal_buffer->user_data = mapped_gst_buffer;

    /* Set PTS */
    mmal_buffer->pts = pts_microsec;
    mmal_buffer->dts = dts_microsec;

    /* Flags */
    mmal_buffer->flags |= mmal_buffer_flags;

    if (input_offset == 0) {
      mmal_buffer->flags |= MMAL_BUFFER_HEADER_FLAG_FRAME_START;

      if (GST_VIDEO_CODEC_FRAME_IS_SYNC_POINT (frame)) {
        mmal_buffer->flags |= MMAL_BUFFER_HEADER_FLAG_KEYFRAME;
      }
    }

    input_offset += mmal_buffer->length;

    if (input_offset == input_size) {
      mmal_buffer->flags |= MMAL_BUFFER_HEADER_FLAG_FRAME_END;
    }

    /* Now send the buffer to decoder. */

    GST_TRACE_OBJECT (self, "Sending MMAL buffer %p, flags 0x%x",
        mmal_buffer, mmal_buffer->flags);

    if (mmal_port_send_buffer (self->dec->input[0], mmal_buffer) !=
        MMAL_SUCCESS) {

      GST_ERROR_OBJECT (self, "Failed to send input buffer(%p) to decoder!",
          mmal_buffer);

      /* N.B. We've taken a ref to the GstBuffer, so we need to handle that.
         Our callback function will do that for us.
       */
      gst_mmal_video_dec_mmal_return_input_buffer_to_pool (self->dec->input[0],
          mmal_buffer);

      flow_ret = GST_FLOW_ERROR;
      goto done;
    }

    previous_mapped_gst_buffer = mapped_gst_buffer;
    mmal_buffer = NULL;
  }
  /* end-while (more to send) */

done:

  /* We relinquished the lock above */
  GST_VIDEO_DECODER_STREAM_LOCK (self);

done_locked:

  /* Should reach this in all cases, except when we drop the frame. */
  gst_video_codec_frame_unref (frame);

  return flow_ret;
}
