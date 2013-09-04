/*
 *  tvheadend, transport and subscription functions
 *  Copyright (C) 2007 Andreas �man
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <assert.h>
#include <pthread.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <errno.h>

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "tvheadend.h"
#include "subscriptions.h"
#include "streaming.h"
#include "channels.h"
#include "service.h"
#include "htsmsg.h"
#include "notify.h"
#include "atomic.h"
#if ENABLE_MPEGTS
#include "input/mpegts.h"
#endif

struct th_subscription_list subscriptions;
static gtimer_t subscription_reschedule_timer;

/**
 *
 */
int
subscriptions_active(void)
{
  return LIST_FIRST(&subscriptions) != NULL;
}



/**
 *
 */
static int
subscription_sort(th_subscription_t *a, th_subscription_t *b)
{
  return b->ths_weight - a->ths_weight;
}


/**
 * The service is producing output.
 */
static void
subscription_link_service(th_subscription_t *s, service_t *t)
{
  streaming_message_t *sm;
  s->ths_state = SUBSCRIPTION_TESTING_SERVICE;
 
  s->ths_service = t;
  LIST_INSERT_HEAD(&t->s_subscriptions, s, ths_service_link);

  pthread_mutex_lock(&t->s_stream_mutex);

  if(TAILQ_FIRST(&t->s_components) != NULL) {

    if(s->ths_start_message != NULL)
      streaming_msg_free(s->ths_start_message);

    s->ths_start_message =
      streaming_msg_create_data(SMT_START, service_build_stream_start(t));
  }

  // Link to service output
  streaming_target_connect(&t->s_streaming_pad, &s->ths_input);


  if(s->ths_start_message != NULL && t->s_streaming_status & TSS_PACKETS) {

    s->ths_state = SUBSCRIPTION_GOT_SERVICE;

    // Send a START message to the subscription client
    streaming_target_deliver(s->ths_output, s->ths_start_message);
    s->ths_start_message = NULL;

    // Send status report
    sm = streaming_msg_create_code(SMT_SERVICE_STATUS, 
				   t->s_streaming_status);
    streaming_target_deliver(s->ths_output, sm);
  }

  pthread_mutex_unlock(&t->s_stream_mutex);
}


/**
 * Called from service code
 */
void
subscription_unlink_service(th_subscription_t *s, int reason)
{
  streaming_message_t *sm;
  service_t *t = s->ths_service;

  pthread_mutex_lock(&t->s_stream_mutex);

  // Unlink from service output
  streaming_target_disconnect(&t->s_streaming_pad, &s->ths_input);

  if(TAILQ_FIRST(&t->s_components) != NULL && 
     s->ths_state == SUBSCRIPTION_GOT_SERVICE) {
    // Send a STOP message to the subscription client
    sm = streaming_msg_create_code(SMT_STOP, reason);
    streaming_target_deliver(s->ths_output, sm);
  }

  pthread_mutex_unlock(&t->s_stream_mutex);

  LIST_REMOVE(s, ths_service_link);
  s->ths_service = NULL;
}

/*
 * Called from mpegts code
 */
void
subscription_unlink_mux(th_subscription_t *s, int reason)
{
  streaming_message_t *sm;
  mpegts_mux_instance_t *mmi = s->ths_mmi;

  pthread_mutex_lock(&mmi->mmi_input->mi_delivery_mutex);

  if (!(s->ths_flags & SUBSCRIPTION_NONE)) {
    streaming_target_disconnect(&mmi->mmi_streaming_pad, &s->ths_input);

    sm = streaming_msg_create_code(SMT_STOP, reason);
    streaming_target_deliver(s->ths_output, sm);
  }

  s->ths_mmi = NULL;
  LIST_REMOVE(s, ths_mmi_link);

  pthread_mutex_unlock(&mmi->mmi_input->mi_delivery_mutex);
}

/**
 *
 */
static void
subscription_reschedule_cb(void *aux)
{
  subscription_reschedule();
}


/**
 *
 */
void
subscription_reschedule(void)
{
  static int reenter = 0;
  th_subscription_t *s;
  service_instance_t *si;
  streaming_message_t *sm;
  int error;
  if (reenter) return;

  lock_assert(&global_lock);

  gtimer_arm(&subscription_reschedule_timer, 
	     subscription_reschedule_cb, NULL, 2);

  LIST_FOREACH(s, &subscriptions, ths_global_link) {
    if (s->ths_mmi) continue;
    if (!s->ths_channel && !s->ths_mmi) continue;
#if 0
    if(s->ths_channel == NULL)
      continue; /* stale entry, channel has been destroyed */
#endif

    if(s->ths_service != NULL && s->ths_current_instance != NULL) {
      /* Already got a service */

      if(s->ths_state != SUBSCRIPTION_BAD_SERVICE)
	      continue; /* And it not bad, so we're happy */

      si = s->ths_current_instance;

      assert(si != NULL);
      si->si_error = s->ths_testing_error;
      time(&si->si_error_time);
    }

    if (s->ths_channel)
      tvhtrace("subscription", "find service for %s weight %d", s->ths_channel->ch_name, s->ths_weight);
    else 
      tvhtrace("subscription", "find instance for %s weight %d", s->ths_service->s_nicename, s->ths_weight);
    si = service_find_instance(s->ths_service, s->ths_channel, &s->ths_instances, &error,
                               s->ths_weight);
    s->ths_current_instance = si;

    if(si == NULL) {
      /* No service available */

      sm = streaming_msg_create_code(SMT_NOSTART, error);
      streaming_target_deliver(s->ths_output, sm);
      continue;
    }

    subscription_link_service(s, si->si_s);
  }
  
  reenter = 0;
}

/**
 *
 */
static void 
subscription_unsubscribe0(th_subscription_t *s, int silent)
{
  service_t *t = s->ths_service;

  lock_assert(&global_lock);

  service_instance_list_clear(&s->ths_instances);

  LIST_REMOVE(s, ths_global_link);

  if (!silent) {
    if(s->ths_channel != NULL) {
      LIST_REMOVE(s, ths_channel_link);
      tvhlog(LOG_INFO, "subscription", "\"%s\" unsubscribing from \"%s\"",
             s->ths_title, s->ths_channel->ch_name);
    } else {
      tvhlog(LOG_INFO, "subscription", "\"%s\" unsubscribing",
             s->ths_title);
    }
  }

  if(t != NULL)
    service_remove_subscriber(t, s, SM_CODE_OK);

#if ENABLE_MPEGTS
  if(s->ths_mmi) {
    mpegts_mux_t *mm = s->ths_mmi->mmi_mux;
    subscription_unlink_mux(s, SM_CODE_SUBSCRIPTION_OVERRIDDEN);
    if (mm)
      mm->mm_stop(mm, 0);
  }
#endif

  if(s->ths_start_message != NULL) 
    streaming_msg_free(s->ths_start_message);
 
  free(s->ths_title);
  free(s->ths_hostname);
  free(s->ths_username);
  free(s->ths_client);
  free(s);

  if (!silent) {
    subscription_reschedule();
    notify_reload("subscriptions");
  }
}

void 
subscription_unsubscribe(th_subscription_t *s)
{
  subscription_unsubscribe0(s, 0);
}

/**
 * This callback is invoked when we receive data and status updates from
 * the currently bound service
 */
static void
subscription_input(void *opauqe, streaming_message_t *sm)
{
  th_subscription_t *s = opauqe;

  if(s->ths_state == SUBSCRIPTION_TESTING_SERVICE) {
    // We are just testing if this service is good

    if(sm->sm_type == SMT_START) {
      if(s->ths_start_message != NULL) 
	streaming_msg_free(s->ths_start_message);
      s->ths_start_message = sm;
      return;
    }

    if(sm->sm_type == SMT_SERVICE_STATUS &&
       sm->sm_code & (TSS_GRACEPERIOD | TSS_ERRORS)) {
      // No, mark our subscription as bad_service
      // the scheduler will take care of things
      s->ths_testing_error = tss2errcode(sm->sm_code);
      s->ths_state = SUBSCRIPTION_BAD_SERVICE;
      streaming_msg_free(sm);
      return;
    }

    if(sm->sm_type == SMT_SERVICE_STATUS &&
       sm->sm_code & TSS_PACKETS) {
      if(s->ths_start_message != NULL) {
	streaming_target_deliver(s->ths_output, s->ths_start_message);
	s->ths_start_message = NULL;
      }
      s->ths_state = SUBSCRIPTION_GOT_SERVICE;
    }
  }

  if(s->ths_state != SUBSCRIPTION_GOT_SERVICE) {
    streaming_msg_free(sm);
    return;
  }

  if(sm->sm_type == SMT_PACKET) {
    th_pkt_t *pkt = sm->sm_data;
    if(pkt->pkt_err)
      s->ths_total_err++;
    s->ths_bytes += pkt->pkt_payload->pb_size;
  } else if(sm->sm_type == SMT_MPEGTS) {
    pktbuf_t *pb = sm->sm_data;
    s->ths_bytes += pb->pb_size;
  }

  streaming_target_deliver(s->ths_output, sm);
}


/**
 *
 */
static void
subscription_input_direct(void *opauqe, streaming_message_t *sm)
{
  th_subscription_t *s = opauqe;

 if(sm->sm_type == SMT_PACKET) {
    th_pkt_t *pkt = sm->sm_data;
    if(pkt->pkt_err)
      s->ths_total_err++;
    s->ths_bytes += pkt->pkt_payload->pb_size;
  } else if(sm->sm_type == SMT_MPEGTS) {
    pktbuf_t *pb = sm->sm_data;
    s->ths_bytes += pb->pb_size;
  }

  streaming_target_deliver(s->ths_output, sm);
}



/**
 *
 */
th_subscription_t *
subscription_create(int weight, const char *name, streaming_target_t *st,
		    int flags, st_callback_t *cb, const char *hostname,
		    const char *username, const char *client)
{
  th_subscription_t *s = calloc(1, sizeof(th_subscription_t));
  int reject = 0;
  static int tally;

  if (flags & SUBSCRIPTION_NONE)
    reject |= -1;
  else if(flags & SUBSCRIPTION_RAW_MPEGTS)
    reject |= SMT_TO_MASK(SMT_PACKET);  // Reject parsed frames
  else
    reject |= SMT_TO_MASK(SMT_MPEGTS);  // Reject raw mpegts

  // TODO: possibly we don't connect anything for SUB_NONE
  streaming_target_init(&s->ths_input, 
			cb ?: subscription_input_direct, s, reject);

  s->ths_weight            = weight;
  s->ths_title             = strdup(name);
  s->ths_hostname          = hostname ? strdup(hostname) : NULL;
  s->ths_username          = username ? strdup(username) : NULL;
  s->ths_client            = client   ? strdup(client)   : NULL;
  s->ths_total_err         = 0;
  s->ths_output            = st;
  s->ths_flags             = flags;

  time(&s->ths_start);

  s->ths_id = ++tally;

  LIST_INSERT_SORTED(&subscriptions, s, ths_global_link, subscription_sort);

  return s;
}


/**
 *
 */
static th_subscription_t *
subscription_create_from_channel_or_service
  (channel_t *ch, service_t *t, unsigned int weight, 
   const char *name, streaming_target_t *st,
   int flags, const char *hostname,
   const char *username, const char *client)
{
  th_subscription_t *s;
  assert(!ch || !t);

  if (ch)
    tvhtrace("subscription", "creating subscription for %s weight %d",
             ch->ch_name, weight);
  s = subscription_create(weight, name, st, flags, subscription_input,
			  hostname, username, client);

  s->ths_channel = ch;
  if (ch)
    LIST_INSERT_HEAD(&ch->ch_subscriptions, s, ths_channel_link);
  s->ths_service = t;

  subscription_reschedule();

  if(s->ths_service == NULL) {
    tvhlog(LOG_NOTICE, "subscription", 
	   "No transponder available for subscription \"%s\" "
	   "to channel \"%s\"",
	   s->ths_title, ch ? ch->ch_name : "none");
  } else {
    source_info_t si;

    s->ths_service->s_setsourceinfo(s->ths_service, &si);

    tvhlog(LOG_INFO, "subscription", 
	   "\"%s\" subscribing on \"%s\", weight: %d, adapter: \"%s\", "
	   "network: \"%s\", mux: \"%s\", provider: \"%s\", "
	   "service: \"%s\"",
	   s->ths_title, ch ? ch->ch_name : "none", weight,
	   si.si_adapter  ?: "<N/A>",
	   si.si_network  ?: "<N/A>",
	   si.si_mux      ?: "<N/A>",
	   si.si_provider ?: "<N/A>",
	   si.si_service  ?: "<N/A>");

    service_source_info_free(&si);
  }
  notify_reload("subscriptions");
  return s;
}

th_subscription_t *
subscription_create_from_channel(channel_t *ch, unsigned int weight, 
				 const char *name, streaming_target_t *st,
				 int flags, const char *hostname,
				 const char *username, const char *client)
{
  return subscription_create_from_channel_or_service
           (ch, NULL, weight, name, st, flags, hostname, username, client);
}

/**
 *
 */
th_subscription_t *
subscription_create_from_service(service_t *t, unsigned int weight,
                                 const char *name,
				 streaming_target_t *st, int flags,
				 const char *hostname, const char *username, 
				 const char *client)
{
  return subscription_create_from_channel_or_service
           (NULL, t, weight, name, st, flags, hostname, username, client);
}

/**
 *
 */
/**
 *
 */
#if ENABLE_MPEGTS
// TODO: move this
static void
mpegts_mux_setsourceinfo ( mpegts_mux_t *mm, source_info_t *si )
{
  char buf[128];

  /* Validate */
  lock_assert(&global_lock);

  /* Update */
  if(mm->mm_network->mn_network_name != NULL)
    si->si_network = strdup(mm->mm_network->mn_network_name);

  mm->mm_display_name(mm, buf, sizeof(buf));
  si->si_mux = strdup(buf);

  if(mm->mm_active && mm->mm_active->mmi_input) {
    mpegts_input_t *mi = mm->mm_active->mmi_input;
    mi->mi_display_name(mi, buf, sizeof(buf));
    si->si_adapter = strdup(buf);
  }
}


th_subscription_t *
subscription_create_from_mux
  (mpegts_mux_t *mm,
   unsigned int weight,
   const char *name,
	 streaming_target_t *st,
   int flags,
	 const char *hostname,
	 const char *username, 
	 const char *client)
{
  th_subscription_t *s;
  streaming_message_t *sm;
  streaming_start_t *ss;
  int r;

  if (!flags)
    flags = SUBSCRIPTION_RAW_MPEGTS;

  s = subscription_create(weight, name, st, flags,
			  NULL, hostname, username, client);

  /* Tune */
  r = mm->mm_start(mm, s->ths_title, weight);

  /* Failed */
  if (r) {
    subscription_unsubscribe0(s, 1);
    return NULL;
  }
  s->ths_mmi = mm->mm_active;

  pthread_mutex_lock(&s->ths_mmi->mmi_input->mi_delivery_mutex);

  /* Store */
  LIST_INSERT_HEAD(&mm->mm_active->mmi_subs, s, ths_mmi_link);
  
  /* Connect (not for NONE streams) */
  if (!(flags & SUBSCRIPTION_NONE)) {
    streaming_target_connect(&s->ths_mmi->mmi_streaming_pad, &s->ths_input);

    /* Deliver a start message */
    ss = calloc(1, sizeof(streaming_start_t));
    ss->ss_num_components = 0;
    ss->ss_refcount       = 1;
    
    mpegts_mux_setsourceinfo(mm, &ss->ss_si);
    ss->ss_si.si_service = strdup("rawmux");
  
    sm = streaming_msg_create_data(SMT_START, ss);
    streaming_target_deliver(s->ths_output, sm);

    tvhinfo("subscription", 
	   "'%s' subscribing to mux, weight: %d, adapter: '%s', "
	   "network: '%s', mux: '%s'",
	   s->ths_title,
     s->ths_weight,
	   ss->ss_si.si_adapter  ?: "<N/A>",
	   ss->ss_si.si_network  ?: "<N/A>",
	   ss->ss_si.si_mux      ?: "<N/A>");
  }

  pthread_mutex_unlock(&s->ths_mmi->mmi_input->mi_delivery_mutex);

  notify_reload("subscriptions");
  return s;
}
#endif

/**
 *
 */
void
subscription_change_weight(th_subscription_t *s, int weight)
{
  if(s->ths_weight == weight)
    return;

  LIST_REMOVE(s, ths_global_link);

  s->ths_weight = weight;
  LIST_INSERT_SORTED(&subscriptions, s, ths_global_link, subscription_sort);

  subscription_reschedule();
}


/**
 *
 */
static void
dummy_callback(void *opauqe, streaming_message_t *sm)
{
  switch(sm->sm_type) {
  case SMT_START:
    fprintf(stderr, "dummysubscription START\n");
    break;
  case SMT_STOP:
    fprintf(stderr, "dummysubscription STOP\n");
    break;

  case SMT_SERVICE_STATUS:
    fprintf(stderr, "dummsubscription: %x\n", sm->sm_code);
    break;
  default:
    break;
  }

  streaming_msg_free(sm);
}

static gtimer_t dummy_sub_timer;

/**
 *
 */
static void
dummy_retry(void *opaque)
{
  subscription_dummy_join(opaque, 0);
  free(opaque);
}

/**
 *
 */
void
subscription_dummy_join(const char *id, int first)
{
  service_t *t = service_find_by_identifier(id);
  streaming_target_t *st;

  if(first) {
    gtimer_arm(&dummy_sub_timer, dummy_retry, strdup(id), 2);
    return;
  }

  if(t == NULL) {
    tvhlog(LOG_ERR, "subscription", 
	   "Unable to dummy join %s, service not found, retrying...", id);

    gtimer_arm(&dummy_sub_timer, dummy_retry, strdup(id), 1);
    return;
  }

  st = calloc(1, sizeof(streaming_target_t));
  streaming_target_init(st, dummy_callback, NULL, 0);
  subscription_create_from_service(t, 1, "dummy", st, 0, NULL, NULL, "dummy");

  tvhlog(LOG_NOTICE, "subscription", 
	 "Dummy join %s ok", id);
}

/**
 *
 */
htsmsg_t *
subscription_create_msg(th_subscription_t *s)
{
  htsmsg_t *m = htsmsg_create_map();

  htsmsg_add_u32(m, "id", s->ths_id);
  htsmsg_add_u32(m, "start", s->ths_start);
  htsmsg_add_u32(m, "errors", s->ths_total_err);

  const char *state;
  switch(s->ths_state) {
  default:
    state = "Idle";
    break;

  case SUBSCRIPTION_TESTING_SERVICE:
    state = "Testing";
    break;
    
  case SUBSCRIPTION_GOT_SERVICE:
    state = "Running";
    break;

  case SUBSCRIPTION_BAD_SERVICE:
    state = "Bad";
    break;
  }


  htsmsg_add_str(m, "state", state);

  if(s->ths_hostname != NULL)
    htsmsg_add_str(m, "hostname", s->ths_hostname);

  if(s->ths_username != NULL)
    htsmsg_add_str(m, "username", s->ths_username);

  if(s->ths_client != NULL)
    htsmsg_add_str(m, "title", s->ths_client);
  else if(s->ths_title != NULL)
    htsmsg_add_str(m, "title", s->ths_title);
  
  if(s->ths_channel != NULL)
    htsmsg_add_str(m, "channel", s->ths_channel->ch_name ?: "");
  
  if(s->ths_service != NULL)
    htsmsg_add_str(m, "service", s->ths_service->s_nicename ?: "");

  return m;
}


static gtimer_t every_sec;

/**
 *
 */
static void
every_sec_cb(void *aux)
{
  th_subscription_t *s;
  gtimer_arm(&every_sec, every_sec_cb, NULL, 1);

  LIST_FOREACH(s, &subscriptions, ths_global_link) {
    int errors = s->ths_total_err;
    int bw = atomic_exchange(&s->ths_bytes, 0);
    
    htsmsg_t *m = subscription_create_msg(s);
    htsmsg_delete_field(m, "errors");
    htsmsg_add_u32(m, "errors", errors);
    htsmsg_add_u32(m, "bw", bw);
    htsmsg_add_u32(m, "updateEntry", 1);
    notify_by_msg("subscriptions", m);
  }
}


/**
 *
 */
void
subscription_init(void)
{
  gtimer_arm(&every_sec, every_sec_cb, NULL, 1);
}

/**
 * Set speed
 */
void
subscription_set_speed ( th_subscription_t *s, int speed )
{
  streaming_message_t *sm;
  service_t *t = s->ths_service;

  if (!t) return;

  pthread_mutex_lock(&t->s_stream_mutex);

  sm = streaming_msg_create_code(SMT_SPEED, speed);

  streaming_target_deliver(s->ths_output, sm);

  pthread_mutex_unlock(&t->s_stream_mutex);
}

/**
 * Set skip
 */
void
subscription_set_skip ( th_subscription_t *s, const streaming_skip_t *skip )
{
  streaming_message_t *sm;
  service_t *t = s->ths_service;

  if (!t) return;

  pthread_mutex_lock(&t->s_stream_mutex);

  sm = streaming_msg_create(SMT_SKIP);
  sm->sm_data = malloc(sizeof(streaming_skip_t));
  memcpy(sm->sm_data, skip, sizeof(streaming_skip_t));

  streaming_target_deliver(s->ths_output, sm);

  pthread_mutex_unlock(&t->s_stream_mutex);
}
