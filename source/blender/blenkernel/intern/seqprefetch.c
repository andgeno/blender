/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 */

/** \file
 * \ingroup bke
 */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "MEM_guardedalloc.h"

#include "DNA_sequence_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_windowmanager_types.h"

#include "BLI_listbase.h"
#include "BLI_threads.h"

#include "IMB_imbuf.h"
#include "IMB_imbuf_types.h"

#include "BKE_animsys.h"
#include "BKE_library.h"
#include "BKE_scene.h"
#include "BKE_main.h"
#include "BKE_context.h"
#include "BKE_sequencer.h"
#include "BKE_layer.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_build.h"
#include "DEG_depsgraph_debug.h"
#include "DEG_depsgraph_query.h"

typedef struct PrefetchJob {
  struct PrefetchJob *next, *prev;

  struct Main *bmain;
  struct Scene *scene;
  struct Scene *scene_eval;
  struct Depsgraph *depsgraph;

  ThreadMutex prefetch_suspend_mutex;
  ThreadCondition prefetch_suspend_cond;

  ListBase threads;

  /* context */
  struct SeqRenderData context;
  struct SeqRenderData context_cpy;
  struct ListBase *seqbasep;
  struct ListBase *seqbasep_cpy;

  /* prefetch area */
  float cfra;
  int num_frames_prefetched;

  /* control */
  bool running;
  bool waiting;
  bool stop;
} PrefetchJob;

static bool seq_prefetch_is_playing(Main *bmain)
{
  for (bScreen *screen = bmain->screens.first; screen; screen = screen->id.next) {
    if (screen->animtimer) {
      return true;
    }
  }
  return false;
}

static bool seq_prefetch_is_scrubbing(Main *bmain)
{

  for (bScreen *screen = bmain->screens.first; screen; screen = screen->id.next) {
    if (screen->scrubbing) {
      return true;
    }
  }
  return false;
}

static PrefetchJob *seq_prefetch_job_get(Scene *scene)
{
  if (scene && scene->ed) {
    return scene->ed->prefetch_job;
  }
  return NULL;
}

static bool seq_prefetch_job_is_running(Scene *scene)
{
  PrefetchJob *pfjob = seq_prefetch_job_get(scene);

  if (!pfjob) {
    return false;
  }

  return pfjob->running;
}

static bool seq_prefetch_job_is_waiting(Scene *scene)
{
  PrefetchJob *pfjob = seq_prefetch_job_get(scene);

  if (!pfjob) {
    return false;
  }

  return pfjob->waiting;
}

/* for cache context swapping */
Sequence *BKE_sequencer_prefetch_get_original_sequence(Sequence *seq, Scene *scene)
{
  Editing *ed = scene->ed;
  ListBase *seqbase = &ed->seqbase;
  Sequence *seq_orig = NULL;

  for (seq_orig = (Sequence *)seqbase->first; seq_orig; seq_orig = seq_orig->next) {
    if (strcmp(seq->name, seq_orig->name) == 0) {
      break;
    }
  }
  return seq_orig;
}

/* for cache context swapping */
SeqRenderData *BKE_sequencer_prefetch_get_original_context(const SeqRenderData *context)
{
  PrefetchJob *pfjob = seq_prefetch_job_get(context->scene);

  return &pfjob->context;
}

static bool seq_prefetch_is_cache_full(Scene *scene)
{
  PrefetchJob *pfjob = seq_prefetch_job_get(scene);

  if (!BKE_sequencer_cache_is_full(pfjob->scene)) {
    return false;
  }

  return BKE_sequencer_cache_recycle_item(pfjob->scene) == false;
}

void BKE_sequencer_prefetch_get_time_range(Scene *scene, int *start, int *end)
{
  PrefetchJob *pfjob = seq_prefetch_job_get(scene);

  *start = pfjob->cfra;
  *end = pfjob->cfra + pfjob->num_frames_prefetched;
}

static void seq_prefetch_free_depsgraph(PrefetchJob *pfjob)
{
  if (pfjob->depsgraph != NULL) {
    DEG_graph_free(pfjob->depsgraph);
  }
  pfjob->depsgraph = NULL;
  pfjob->scene_eval = NULL;
}

static void seq_prefetch_update_depsgraph(PrefetchJob *pfjob)
{
  DEG_evaluate_on_framechange(
      pfjob->bmain, pfjob->depsgraph, pfjob->cfra + pfjob->num_frames_prefetched);
}

static void seq_prefetch_init_depsgraph(PrefetchJob *pfjob)
{
  Main *bmain = pfjob->bmain;
  Scene *scene = pfjob->scene;
  ViewLayer *view_layer = BKE_view_layer_default_render(scene);

  pfjob->depsgraph = DEG_graph_new(bmain, scene, view_layer, DAG_EVAL_RENDER);
  DEG_debug_name_set(pfjob->depsgraph, "SEQUENCER PREFETCH");

  /* Make sure there is a correct evaluated scene pointer. */
  DEG_graph_build_for_render_pipeline(pfjob->depsgraph, pfjob->bmain, scene, view_layer);

  /* Update immediately so we have proper evaluated scene. */
  seq_prefetch_update_depsgraph(pfjob);

  pfjob->scene_eval = DEG_get_evaluated_scene(pfjob->depsgraph);
  pfjob->scene_eval->ed->cache_flag = 0;
}

static void seq_prefetch_update_area(PrefetchJob *pfjob)
{
  int cfra = pfjob->scene->r.cfra;

  /* rebase */
  if (cfra > pfjob->cfra) {
    int delta = cfra - pfjob->cfra;
    pfjob->cfra = cfra;
    pfjob->num_frames_prefetched -= delta;

    if (pfjob->num_frames_prefetched <= 1) {
      pfjob->num_frames_prefetched = 1;
    }
  }

  /* reset */
  if (cfra < pfjob->cfra) {
    pfjob->cfra = cfra;
    pfjob->num_frames_prefetched = 1;
  }
}

/* Use also to update scene and context changes */
void BKE_sequencer_prefetch_stop(Scene *scene)
{
  PrefetchJob *pfjob;
  pfjob = seq_prefetch_job_get(scene);

  if (!pfjob) {
    return;
  }

  pfjob->stop = true;

  while (pfjob->running) {
    BLI_condition_notify_one(&pfjob->prefetch_suspend_cond);
  }
}

static void seq_prefetch_update_context(const SeqRenderData *context)
{
  PrefetchJob *pfjob;
  pfjob = seq_prefetch_job_get(context->scene);

  BKE_sequencer_new_render_data(pfjob->bmain,
                                pfjob->depsgraph,
                                pfjob->scene_eval,
                                context->rectx,
                                context->recty,
                                context->preview_render_size,
                                false,
                                &pfjob->context_cpy);
  pfjob->context_cpy.is_prefetch_render = true;
  pfjob->context_cpy.task_id = SEQ_TASK_PREFETCH_RENDER;

  BKE_sequencer_new_render_data(pfjob->bmain,
                                pfjob->depsgraph,
                                pfjob->scene,
                                context->rectx,
                                context->recty,
                                context->preview_render_size,
                                false,
                                &pfjob->context);
  pfjob->context.is_prefetch_render = false;

  /* Same ID as prefetch context, because context will be swapped, but we still
   * want to assign this ID to cache entries created in this thread.
   * This is to allow "temp cache" work correctly for both threads.
   */
  pfjob->context.task_id = SEQ_TASK_PREFETCH_RENDER;
}

static void seq_prefetch_update_scene(Scene *scene)
{
  PrefetchJob *pfjob = seq_prefetch_job_get(scene);

  if (!pfjob) {
    return;
  }

  seq_prefetch_free_depsgraph(pfjob);
  seq_prefetch_init_depsgraph(pfjob);
}

static void seq_prefetch_resume(Scene *scene)
{
  PrefetchJob *pfjob = seq_prefetch_job_get(scene);

  if (pfjob && pfjob->waiting) {
    BLI_condition_notify_one(&pfjob->prefetch_suspend_cond);
  }
}

void BKE_sequencer_prefetch_free(Scene *scene)
{
  PrefetchJob *pfjob = seq_prefetch_job_get(scene);
  if (!pfjob) {
    return;
  }

  BKE_sequencer_prefetch_stop(scene);

  BLI_threadpool_remove(&pfjob->threads, pfjob);
  BLI_threadpool_end(&pfjob->threads);
  BLI_mutex_end(&pfjob->prefetch_suspend_mutex);
  BLI_condition_end(&pfjob->prefetch_suspend_cond);
  seq_prefetch_free_depsgraph(pfjob);
  MEM_freeN(pfjob);
  scene->ed->prefetch_job = NULL;
}

static void *seq_prefetch_frames(void *job)
{
  PrefetchJob *pfjob = (PrefetchJob *)job;

  /* set to NULL before return! */
  pfjob->scene_eval->ed->prefetch_job = pfjob;

  while (pfjob->cfra + pfjob->num_frames_prefetched < pfjob->scene->r.efra) {
    BKE_animsys_evaluate_all_animation(pfjob->context_cpy.bmain,
                                       pfjob->context_cpy.depsgraph,
                                       pfjob->context_cpy.scene,
                                       pfjob->cfra + pfjob->num_frames_prefetched);
    seq_prefetch_update_depsgraph(pfjob);

    ImBuf *ibuf = BKE_sequencer_give_ibuf(
        &pfjob->context_cpy, pfjob->cfra + pfjob->num_frames_prefetched, 0);
    BKE_sequencer_cache_free_temp_cache(
        pfjob->scene, pfjob->context.task_id, pfjob->cfra + pfjob->num_frames_prefetched);
    IMB_freeImBuf(ibuf);

    /* suspend thread */
    BLI_mutex_lock(&pfjob->prefetch_suspend_mutex);
    while ((seq_prefetch_is_cache_full(pfjob->scene) || seq_prefetch_is_scrubbing(pfjob->bmain)) &&
           pfjob->scene->ed->cache_flag & SEQ_CACHE_PREFETCH_ENABLE && !pfjob->stop) {
      pfjob->waiting = true;
      BLI_condition_wait(&pfjob->prefetch_suspend_cond, &pfjob->prefetch_suspend_mutex);
      seq_prefetch_update_area(pfjob);
    }
    pfjob->waiting = false;
    BLI_mutex_unlock(&pfjob->prefetch_suspend_mutex);

    /* Avoid "collision" with main thread, but make sure to fetch at least few frames */
    if (pfjob->num_frames_prefetched > 5 &&
        (pfjob->cfra + pfjob->num_frames_prefetched - pfjob->scene->r.cfra) < 2) {
      break;
    }

    if (!(pfjob->scene->ed->cache_flag & SEQ_CACHE_PREFETCH_ENABLE) || pfjob->stop) {
      break;
    }

    seq_prefetch_update_area(pfjob);
    pfjob->num_frames_prefetched++;
  }

  BKE_sequencer_cache_free_temp_cache(
      pfjob->scene, pfjob->context.task_id, pfjob->cfra + pfjob->num_frames_prefetched);
  pfjob->running = false;
  pfjob->scene_eval->ed->prefetch_job = NULL;

  return 0;
}

PrefetchJob *seq_prefetch_start(const SeqRenderData *context, float cfra)
{
  PrefetchJob *pfjob;
  pfjob = seq_prefetch_job_get(context->scene);

  if (!pfjob) {
    if (context->scene->ed) {
      pfjob = (PrefetchJob *)MEM_callocN(sizeof(PrefetchJob), "PrefetchJob");
      context->scene->ed->prefetch_job = pfjob;

      BLI_threadpool_init(&pfjob->threads, seq_prefetch_frames, 1);
      BLI_mutex_init(&pfjob->prefetch_suspend_mutex);
      BLI_condition_init(&pfjob->prefetch_suspend_cond);

      pfjob->bmain = context->bmain;

      pfjob->scene = context->scene;
      seq_prefetch_init_depsgraph(pfjob);
    }
  }
  seq_prefetch_update_scene(context->scene);
  seq_prefetch_update_context(context);

  pfjob->cfra = cfra;
  pfjob->num_frames_prefetched = 1;

  pfjob->waiting = false;
  pfjob->stop = false;
  pfjob->running = true;

  if (&pfjob->threads) {
    BLI_threadpool_remove(&pfjob->threads, pfjob);
  }
  BLI_threadpool_insert(&pfjob->threads, pfjob);

  return pfjob;
}

/* Start or resume prefetching*/
void BKE_sequencer_prefetch_start(const SeqRenderData *context, float cfra, float cost)
{
  Scene *scene = context->scene;
  Editing *ed = scene->ed;
  bool has_strips = (bool)ed->seqbasep->first;

  if (!context->is_prefetch_render && !context->is_proxy_render) {
    bool playing = seq_prefetch_is_playing(context->bmain);
    bool scrubbing = seq_prefetch_is_scrubbing(context->bmain);
    bool running = seq_prefetch_job_is_running(scene);
    seq_prefetch_resume(scene);
    /* conditions to start:
     * prefetch enabled, prefetch not running, not scrubbing,
     * not playing and rendering-expensive footage, cache storage enabled, has strips to render
     */
    if ((ed->cache_flag & SEQ_CACHE_PREFETCH_ENABLE) && !running && !scrubbing &&
        !(playing && cost > 0.9) && ed->cache_flag & SEQ_CACHE_ALL_TYPES && has_strips) {

      seq_prefetch_start(context, cfra);
    }
  }
}

bool BKE_sequencer_prefetch_need_redraw(Main *bmain, Scene *scene)
{
  bool playing = seq_prefetch_is_playing(bmain);
  bool scrubbing = seq_prefetch_is_scrubbing(bmain);
  bool running = seq_prefetch_job_is_running(scene);
  bool suspended = seq_prefetch_job_is_waiting(scene);

  /* force redraw, when prefetching and using cache view. */
  if (running && !playing && !suspended && scene->ed->cache_flag & SEQ_CACHE_VIEW_ENABLE) {
    return true;
  }
  /* Sometimes scrubbing flag is set when not scrubbing. In that case I want to catch "event" of
   * stopping scrubbing */
  if (scrubbing) {
    return true;
  }
  return false;
}
