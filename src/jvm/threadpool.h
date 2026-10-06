/*
 * J2ME Emulator — v36.73 [VM-THREADPOOL]
 *
 * Worker-thread POOL for java/lang/Thread.start() (POSIX path).
 * ==========================================================================
 * WHY (architecture decision, replaces the v36.72 phonestacks reaper):
 *
 * The pre-pool lifecycle paid, on EVERY Thread.start():
 *   pthread_create (2 MB stack carved out of the malloc arena) +
 *   pthread_detach (a NO-OP on libnx: the 32 MB v36.63 stacks leaked the
 *   arena dry — the v36.72 phonestacks reaper fixed that with pthread_join)
 *   + usleep(1000) ON THE CALLING GAME THREAD + reaper join latency at
 *   teardown. The reaper also introduced NEW stall surfaces: joins taken
 *   under the malloc lock while a game thread allocates, and GC-census
 *   interplay (a joiner parked in a native never polls safepoints).
 *
 * The pool removes the create/destroy lifecycle from gameplay ENTIRELY:
 *   Thread.start  = claim a parked worker (condvar signal, ~µs)
 *   run() return  = park the worker back (no exit, no stack free)
 * Stacks are allocated once per worker and freed only at VM teardown, so
 * the leak is impossible BY CONSTRUCTION — no detach, no reaper, no join
 * churn. join()/isAlive() semantics are unchanged (they watch the
 * JavaThread shell + its g_pthread_cond slot, exactly as before).
 *
 * On the Switch each worker stack is allocated by us (posix_memalign) with
 * a precise PROT_NONE guard page at the growing edge (v36.72 parity);
 * pthread_attr_setstack hands it to pthread_create. If any step is not
 * supported we degrade to pthread_attr_setstacksize (libnx then manages
 * the stack — still pool-owned, so still leak-free).
 *
 * GC census correctness: a worker parked IDLE is NOT a mutator — the
 * claimer re-points the worker's registry slot and marks it running BEFORE
 * the pool wakes the worker (mirrors the old "set running before
 * pthread_create" v9 rule), and the worker's first action after wake is
 * the v34.95 CENSUS-WINDOW safepoint park. A worker therefore can never
 * run its first bytecode while a mark/sweep is in flight.
 * ==========================================================================
 */

#ifndef NOJME_THREADPOOL_H
#define NOJME_THREADPOOL_H

/* Submit a job (the pthread runner body). Returns 0 on success — the job
 * will start asynchronously exactly like pthread_create; -1 when the pool
 * is at its hard cap or is shutting down (caller must run the failure
 * cleanup, same as the old pthread_create rc!=0 path).
 *
 * on_claim (optional, ctx + slot) runs INSIDE the claim critical section:
 * the worker is assigned but still parked — it CANNOT run until on_claim
 * returns. This is where the caller re-points the worker's permanent
 * registry slot (g_live_threads[slot] = jt; g_live_running[slot] = true)
 * so the GC census sees the newcomer from the very first instant it can
 * execute — closing the v34.95 census-window race with ZERO gap (the old
 * pthread path had a create->register window that the newcomer's first
 * safepoint park could only mitigate, not eliminate). */
int  vm_pool_submit(void (*fn)(void*), void* arg,
                    void (*on_claim)(void* ctx, int slot), void* claim_ctx);

/* The registry slot of the worker THIS OS thread is running on right now
 * (-1 on the frontend/main thread). The job body uses it for its idempotent
 * re-point at job start and for the slot clear at job end. */
int  vm_pool_current_slot(void);

/* Workers created so far (== permanent registry slots in use). */
int  vm_pool_worker_count(void);

/* Parked-and-free workers right now (diagnostics for the trace line). */
int  vm_pool_idle_count(void);

/* Teardown: poison every worker and join it. Call ONLY after
 * native_threads_wait_idle() reported 0 busy — a worker mid-job does not
 * poll the poison flag (its job exits via jvm->running=false). Joins all
 * workers, frees their stacks, resets the pool for the next session. */
void vm_pool_shutdown_join(void);

#endif /* NOJME_THREADPOOL_H */
