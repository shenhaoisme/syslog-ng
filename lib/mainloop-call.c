#include "mainloop-call.h"
#include "tls-support.h"
#include <iv.h>
#include <iv_event.h>

typedef struct _MainLoopTaskCallSite MainLoopTaskCallSite;
struct _MainLoopTaskCallSite
{
  struct iv_list_head list;
  MainLoopTaskFunc func;
  gpointer user_data;
  gpointer result;
  gboolean pending;
  gboolean wait;
  GCond *cond;
  GStaticMutex lock;
};

TLS_BLOCK_START
{
  MainLoopTaskCallSite call_info;
}
TLS_BLOCK_END;

#define call_info __tls_deref(call_info)

static GStaticMutex main_task_lock = G_STATIC_MUTEX_INIT;
static struct iv_list_head main_task_queue = IV_LIST_HEAD_INIT(main_task_queue);

static struct iv_event main_task_posted;

gpointer
main_loop_call(MainLoopTaskFunc func, gpointer user_data, gboolean wait)
{
  if (main_loop_is_main_thread())
    return func(user_data);

  g_static_mutex_lock(&main_task_lock);

  /* check if a previous call is being executed */
  g_static_mutex_lock(&call_info.lock);
  if (call_info.pending)
    {
      /* yes, it is still running, indicate that we need to be woken up */
      call_info.wait = TRUE;
      g_static_mutex_unlock(&call_info.lock);

      while (call_info.pending)
        {
          g_cond_wait(call_info.cond, g_static_mutex_get_mutex(&main_task_lock));
        }
    }
  else
    {
      g_static_mutex_unlock(&call_info.lock);
    }

  /* call_info.lock is no longer needed, since we're the only ones using call_info now */
  INIT_IV_LIST_HEAD(&call_info.list);
  call_info.pending = TRUE;
  call_info.func = func;
  call_info.user_data = user_data;
  call_info.wait = wait;
  main_loop_call_thread_init();
  iv_list_add(&call_info.list, &main_task_queue);
  iv_event_post(&main_task_posted);
  if (wait)
    {
      while (call_info.pending)
        g_cond_wait(call_info.cond, g_static_mutex_get_mutex(&main_task_lock));
    }
  g_static_mutex_unlock(&main_task_lock);
  return call_info.result;
}

static void
main_loop_call_handler(gpointer user_data)
{
  g_static_mutex_lock(&main_task_lock);
  while (!iv_list_empty(&main_task_queue))
    {
      MainLoopTaskCallSite *site;
      gpointer result;

      site = iv_list_entry(main_task_queue.next, MainLoopTaskCallSite, list);
      iv_list_del_init(&site->list);
      g_static_mutex_unlock(&main_task_lock);

      result = site->func(site->user_data);

      g_static_mutex_lock(&site->lock);
      site->result = result;
      site->pending = FALSE;
      g_static_mutex_unlock(&site->lock);

      g_static_mutex_lock(&main_task_lock);
      if (site->wait)
        g_cond_signal(site->cond);
    }
  g_static_mutex_unlock(&main_task_lock);
}

void
main_loop_call_thread_init(void)
{
  call_info.cond = g_cond_new();
}

void
main_loop_call_thread_deinit(void)
{
  if (call_info.cond)
    g_cond_free(call_info.cond);
}

void
main_loop_call_init(void)
{
  IV_EVENT_INIT(&main_task_posted);
  main_task_posted.cookie = NULL;
  main_task_posted.handler = main_loop_call_handler;
  iv_event_register(&main_task_posted);
}

void
main_loop_call_deinit(void)
{
  iv_event_unregister(&main_task_posted);
}