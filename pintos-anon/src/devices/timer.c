#include "threads/malloc.h"
//needed for memory management of list items.
#include "devices/timer.h"
#include <debug.h>
#include <list.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include "devices/pit.h"
#include "threads/interrupt.h"
#include "threads/synch.h"
#include "threads/thread.h"
  
/* See [8254] for hardware details of the 8254 timer chip. */

#if TIMER_FREQ < 19
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

/* Number of timer ticks since OS booted. */
static int64_t ticks;
//Cecils additions
static struct list suspension_list;

struct susp_elem{
     struct list_elem elem;
     struct thread* thr;
     enum thread_status init_status;
     int64_t wake_ctr;//the count till the thread wakes up.
};


/* Number of loops per timer tick.
   Initialized by timer_calibrate(). */
static unsigned loops_per_tick;

static intr_handler_func timer_interrupt;
static bool too_many_loops (unsigned loops);
static void busy_wait (int64_t loops);
static void real_time_sleep (int64_t num, int32_t denom);
static void real_time_delay (int64_t num, int32_t denom);

//cecils additions
static void suspension_tick (void);
static void suspension_enque(struct thread* thr, int64_t count_till_wake);
static void suspension_deque(void);
static void 
suspension_remove(struct list_elem* item);
//Cecils additions

static void 
suspension_remove(struct list_elem* item)
{
     list_remove(item);
     thread_unblock(((struct susp_elem*)item)->thr);
     ((struct susp_elem*)item)->thr = NULL;//prevent the thread from being deleted
     /*
      Call any other code to make the thread usable again, like pushing the thread back onto the running queue if it is removed.
      */    
     item->next = NULL;//prevent the next list element from being deleted.
     item->prev = NULL;//prevent the previous list element from being deleted.
     free( ( void *)item );//free the list
     item = NULL;
}

static void 
suspension_tick(void)
{
     
     if (list_empty(&suspension_list)){
          return;//list has no length, return.
     }

     struct list_elem * iter ;
     //for each element in list
     for(iter= list_front(&suspension_list); iter != list_end(&suspension_list);
         iter = list_next( iter) )
     {
          ((struct susp_elem*)iter)->wake_ctr-=1;//reduce the wake counter by one
          //if it is zero, then remove it from the suspension list and re awaken it.
          if (((struct susp_elem*)iter)->wake_ctr==0){
               suspension_remove(iter);
               
          }else{
               //if the thread is the current thread,
               //and the thread status is not blocked.
               //then block the thread.
               if( ((struct susp_elem*)iter)->thr == thread_current() && 
                    ( ((struct susp_elem*)iter)->thr->status != THREAD_BLOCKED){
                         thread_block();
               }
          }
     }
}

static void 
suspension_deque(void)
{
     struct list_elem* item = list_pop_front(&suspension_list);
     thread_unblock(((struct susp_elem*)item)->thr);
     ((struct susp_elem*)item)->thr = NULL;//prevent the thread from being deleted
     /*
      Call any other code to make the thread usable again, like pushing the thread back onto the running queue if it is removed.
      */
     
     item->next = NULL;//prevent the next list element from being deleted.
     item->prev = NULL;//prevent the previous list element from being deleted.
     free( ( void *)item );//free the list
     item = NULL;
}

int64_t get_wake_time( list_elem* susp_node){
     return ((struct susp_elem*) susp_node)->wake_ctr;
}

static void suspension_enque (int64_t wake_ctr){
     struct susp_elem* new_elem = (struct susp_elem*) malloc( sizeof(struct susp_elem));
     ASSERT( new_elem != NULL);
     enum intr_level old_level = intr_disable ();//Disable interrupts so you can block the thread.
     new_elem->wake_ctr = wake_ctr;
     new_elem->thr = thread_current();
     thread_block();//make it so that the thread does not run on the cpu.
     list_push_front(&suspension_list, (struct list_elem*)&new_elem);
     new_elem = NULL;
     intr_set_level (old_level);//Re enable interrupts so you can block the thread.
}

/*
static void
suspension_enque(struct thread* th_insrt, int64_t wake_ctr){
     struct susp_elem* new_elem = (struct susp_elem*) malloc( sizeof(struct susp_elem));
     ASSERT( new_elem != NULL);
     new_elem->wake_ctr = wake_ctr;
     new_elem->thr = th_insrt;
     if (list_empty(&suspension_list)){
          list_push_front(&suspension_list, (struct list_elem*)&new_elem);
          new_elem = NULL;
          return;
     }
     list_push_front(&suspension_list, (struct list_elem*)&new_elem);     
     new_elem = NULL;
     sort_susp_list();
}

static void sort_susp_list(){
     static list_elem* iter = list_begin(&suspension_list);
     if( get_wake_time(iter) >= get_wake_time(iter->next){
          swap(iter, iter->next);
     }
     iter = list_next(&suspension_list);
     while (iter != list
}
*/

/* Sets up the timer to interrupt TIMER_FREQ times per second,
   and registers the corresponding interrupt. */
void
timer_init (void) 
{
  list_init( &suspension_list);//initialize suspension list.
  pit_configure_channel (0, 2, TIMER_FREQ);
  intr_register_ext (0x20, timer_interrupt, "8254 Timer");
}

/* Calibrates loops_per_tick, used to implement brief delays. */
void
timer_calibrate (void) 
{
  unsigned high_bit, test_bit;

  ASSERT (intr_get_level () == INTR_ON);
  printf ("Calibrating timer...  ");

  /* Approximate loops_per_tick as the largest power-of-two
     still less than one timer tick. */
  loops_per_tick = 1u << 10;
  while (!too_many_loops (loops_per_tick << 1)) 
    {
      loops_per_tick <<= 1;
      ASSERT (loops_per_tick != 0);
    }

  /* Refine the next 8 bits of loops_per_tick. */
  high_bit = loops_per_tick;
  for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
    if (!too_many_loops (high_bit | test_bit))
      loops_per_tick |= test_bit;

  printf ("%'"PRIu64" loops/s.\n", (uint64_t) loops_per_tick * TIMER_FREQ);
}

/* Returns the number of timer ticks since the OS booted. */
int64_t
timer_ticks (void) 
{
  enum intr_level old_level = intr_disable ();
  int64_t t = ticks;
  intr_set_level (old_level);
  return t;
}

/* Returns the number of timer ticks elapsed since THEN, which
   should be a value once returned by timer_ticks(). */
int64_t
timer_elapsed (int64_t then) 
{
  return timer_ticks () - then;
}

/* Sleeps for approximately TICKS timer ticks.  Interrupts must
   be turned on. */
void
timer_sleep (int64_t ticks) 
{
  int64_t start = timer_ticks ();

  ASSERT (intr_get_level () == INTR_ON);
  thread_enque( , ticks);
  
  while (timer_elapsed (start) < ticks) 
    thread_yield ();
}

/* Sleeps for approximately MS milliseconds.  Interrupts must be
   turned on. */
void
timer_msleep (int64_t ms) 
{
  real_time_sleep (ms, 1000);
}

/* Sleeps for approximately US microseconds.  Interrupts must be
   turned on. */
void
timer_usleep (int64_t us) 
{
  real_time_sleep (us, 1000 * 1000);
}

/* Sleeps for approximately NS nanoseconds.  Interrupts must be
   turned on. */
void
timer_nsleep (int64_t ns) 
{
  real_time_sleep (ns, 1000 * 1000 * 1000);
}

/* Busy-waits for approximately MS milliseconds.  Interrupts need
   not be turned on.

   Busy waiting wastes CPU cycles, and busy waiting with
   interrupts off for the interval between timer ticks or longer
   will cause timer ticks to be lost.  Thus, use timer_msleep()
   instead if interrupts are enabled. */
void
timer_mdelay (int64_t ms) 
{
  real_time_delay (ms, 1000);
}

/* Sleeps for approximately US microseconds.  Interrupts need not
   be turned on.

   Busy waiting wastes CPU cycles, and busy waiting with
   interrupts off for the interval between timer ticks or longer
   will cause timer ticks to be lost.  Thus, use timer_usleep()
   instead if interrupts are enabled. */
void
timer_udelay (int64_t us) 
{
  real_time_delay (us, 1000 * 1000);
}

/* Sleeps execution for approximately NS nanoseconds.  Interrupts
   need not be turned on.

   Busy waiting wastes CPU cycles, and busy waiting with
   interrupts off for the interval between timer ticks or longer
   will cause timer ticks to be lost.  Thus, use timer_nsleep()
   instead if interrupts are enabled.*/
void
timer_ndelay (int64_t ns) 
{
  real_time_delay (ns, 1000 * 1000 * 1000);
}

/* Prints timer statistics. */
void
timer_print_stats (void) 
{
  printf ("Timer: %"PRId64" ticks\n", timer_ticks ());
}

/* Timer interrupt handler. */
static void
timer_interrupt (struct intr_frame *args UNUSED)
{
  ticks++;
  thread_tick ();
  suspension_tick();
}

/* Returns true if LOOPS iterations waits for more than one timer
   tick, otherwise false. */
static bool
too_many_loops (unsigned loops) 
{
  /* Wait for a timer tick. */
  int64_t start = ticks;
  while (ticks == start)
    barrier ();

  /* Run LOOPS loops. */
  start = ticks;
  busy_wait (loops);

  /* If the tick count changed, we iterated too long. */
  barrier ();
  return start != ticks;
}

/* Iterates through a simple loop LOOPS times, for implementing
   brief delays.

   Marked NO_INLINE because code alignment can significantly
   affect timings, so that if this function was inlined
   differently in different places the results would be difficult
   to predict. */
static void NO_INLINE
busy_wait (int64_t loops) 
{
  while (loops-- > 0)
    barrier ();
}

/* Sleep for approximately NUM/DENOM seconds. */
static void
real_time_sleep (int64_t num, int32_t denom) 
{
  /* Convert NUM/DENOM seconds into timer ticks, rounding down.
          
        (NUM / DENOM) s          
     ---------------------- = NUM * TIMER_FREQ / DENOM ticks. 
     1 s / TIMER_FREQ ticks
  */
  int64_t ticks = num * TIMER_FREQ / denom;

  ASSERT (intr_get_level () == INTR_ON);
  if (ticks > 0)
    {
      /* We're waiting for at least one full timer tick.  Use
         timer_sleep() because it will yield the CPU to other
         processes. */                
      timer_sleep (ticks); 
    }
  else 
    {
      /* Otherwise, use a busy-wait loop for more accurate
         sub-tick timing. */
      real_time_delay (num, denom); 
    }
}

/* Busy-wait for approximately NUM/DENOM seconds. */
static void
real_time_delay (int64_t num, int32_t denom)
{
  /* Scale the numerator and denominator down by 1000 to avoid
     the possibility of overflow. */
  ASSERT (denom % 1000 == 0);
  busy_wait (loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000)); 
}
