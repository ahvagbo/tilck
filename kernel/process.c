/* SPDX-License-Identifier: BSD-2-Clause */

#include <tilck_gen_headers/config_debug.h>
#include <tilck_gen_headers/mod_debugpanel.h>

#include <tilck/common/basic_defs.h>
#include <tilck/common/printk.h>
#include <tilck/common/string_util.h>
#include <tilck/common/unaligned.h>

#include <tilck/kernel/process.h>
#include <tilck/kernel/process_mm.h>
#include <tilck/kernel/sched.h>
#include <tilck/kernel/list.h>
#include <tilck/kernel/kmalloc.h>
#include <tilck/kernel/errno.h>
#include <tilck/kernel/user.h>
#include <tilck/kernel/debug_utils.h>
#include <tilck/kernel/fs/vfs.h>
#include <tilck/kernel/paging_hw.h>
#include <tilck/kernel/process_int.h>

#include <sys/prctl.h>        // system header

STATIC_ASSERT(IS_PAGE_ALIGNED(KERNEL_STACK_SIZE));
STATIC_ASSERT(IS_PAGE_ALIGNED(IO_COPYBUF_SIZE));
STATIC_ASSERT(IS_PAGE_ALIGNED(ARGS_COPYBUF_SIZE));

#define ISOLATED_STACK_HI_VMEM_SPACE   (KERNEL_STACK_SIZE + (2 * PAGE_SIZE))

static void *alloc_kernel_isolated_stack(struct process *pi)
{
   void *vaddr_in_block;
   void *block_vaddr;
   void *direct_va;
   ulong direct_pa;
   size_t count;

   ASSERT(pi->pdir != NULL);

   direct_va = kzmalloc(KERNEL_STACK_SIZE);

   if (!direct_va)
      return NULL;

   direct_pa = LIN_VA_TO_PA(direct_va);
   block_vaddr = hi_vmem_reserve(ISOLATED_STACK_HI_VMEM_SPACE);

   if (!block_vaddr) {
      kfree2(direct_va, KERNEL_STACK_SIZE);
      return NULL;
   }

   vaddr_in_block = (void *)((ulong)block_vaddr + PAGE_SIZE);

   count = map_pages(get_kernel_pdir(),
                     vaddr_in_block,
                     direct_pa,
                     KERNEL_STACK_PAGES,
                     PAGING_FL_RW);

   if (count != KERNEL_STACK_PAGES) {
      unmap_pages(get_kernel_pdir(), vaddr_in_block, count, false);
      hi_vmem_release(block_vaddr, ISOLATED_STACK_HI_VMEM_SPACE);
      kfree2(direct_va, KERNEL_STACK_SIZE);
      return NULL;
   }

   return vaddr_in_block;
}

static void
free_kernel_isolated_stack(struct process *pi, void *vaddr_in_block)
{
   void *block_vaddr = (void *)((ulong)vaddr_in_block - PAGE_SIZE);
   ulong direct_pa = get_mapping(get_kernel_pdir(), vaddr_in_block);
   void *direct_va = PA_TO_LIN_VA(direct_pa);

   unmap_pages(get_kernel_pdir(), vaddr_in_block, KERNEL_STACK_PAGES, false);
   hi_vmem_release(block_vaddr, ISOLATED_STACK_HI_VMEM_SPACE);
   kfree2(direct_va, KERNEL_STACK_SIZE);
}


#define TOT_IOBUF_AND_ARGS_BUF_PG (USER_ARGS_PAGE_COUNT + USER_ARGS_PAGE_COUNT)

STATIC_ASSERT(
   TOT_IOBUF_AND_ARGS_BUF_PG ==  2     ||
   TOT_IOBUF_AND_ARGS_BUF_PG ==  4     ||
   TOT_IOBUF_AND_ARGS_BUF_PG ==  8     ||
   TOT_IOBUF_AND_ARGS_BUF_PG == 16     ||
   TOT_IOBUF_AND_ARGS_BUF_PG == 32     ||
   TOT_IOBUF_AND_ARGS_BUF_PG == 64
);

#undef TOT_IOBUF_AND_ARGS_BUF_PG

static void alloc_kernel_stack(struct task *ti)
{
   if (KERNEL_STACK_ISOLATION) {
      ti->kernel_stack = alloc_kernel_isolated_stack(ti->pi);
   } else {
      ti->kernel_stack = kzmalloc(KERNEL_STACK_SIZE);
   }
}

static void free_kernel_stack(struct task *ti)
{
   if (KERNEL_STACK_ISOLATION) {
      free_kernel_isolated_stack(ti->pi, ti->kernel_stack);
   } else {
      kfree2(ti->kernel_stack, KERNEL_STACK_SIZE);
   }
}

static bool do_common_task_allocs(struct task *ti, bool alloc_bufs)
{
   alloc_kernel_stack(ti);

   if (!ti->kernel_stack)
      return false;

   if (alloc_bufs) {

      ti->io_copybuf = kmalloc(IO_COPYBUF_SIZE + ARGS_COPYBUF_SIZE);

      if (!ti->io_copybuf) {
         free_kernel_stack(ti);
         return false;
      }

      ti->args_copybuf = (void *)((ulong)ti->io_copybuf + IO_COPYBUF_SIZE);
   }
   return true;
}

void process_free_mappings_info(struct process *pi)
{
   struct mappings_info *mi = pi->mi;

   if (mi) {
      ASSERT(mi->mmap_heap);
      kmalloc_destroy_heap(mi->mmap_heap);
      kfree2(mi->mmap_heap, kmalloc_get_heap_struct_size());
      kfree_obj(mi, struct mappings_info);
      pi->mi = NULL;
   }
}

void free_common_task_allocs(struct task *ti)
{
   struct process *pi = ti->pi;
   process_free_mappings_info(pi);

   free_kernel_stack(ti);
   kfree2(ti->io_copybuf, IO_COPYBUF_SIZE + ARGS_COPYBUF_SIZE);

   ti->io_copybuf = NULL;
   ti->args_copybuf = NULL;
   ti->kernel_stack = NULL;
}

void free_mem_for_zombie_task(struct task *ti)
{
   ASSERT_TASK_STATE(ti->state, TASK_STATE_ZOMBIE);

#if DEBUG_CHECKS
   if (ti == get_curr_task()) {
      volatile ulong stack_var = 123;
      if (!IN_RANGE((ulong)&stack_var & PAGE_MASK, init_st_begin, init_st_end))
         panic("free_mem_for_zombie_task() called w/o switch to initial stack");
   }
#endif

   free_common_task_allocs(ti);

   if (ti->pi->automatic_reaping || is_kernel_thread(ti)) {
      /*
       * The SIGCHLD signal has been EXPLICITLY ignored by the parent or this
       * is a kernel thread and we don't do any reaping.
       */
      remove_task(ti);
   }
}

void init_task_lists(struct task *ti)
{
   bintree_node_init(&ti->tree_by_tid_node);
   list_node_init(&ti->runnable_node);
   list_node_init(&ti->wakeup_timer_node);
   list_node_init(&ti->siblings_node);

   list_init(&ti->tasks_waiting_list);
   list_init(&ti->on_exit);
   bzero(&ti->wobj, sizeof(struct wait_obj));
}

void init_process_lists(struct process *pi)
{
   list_init(&pi->children);
   kmutex_init(&pi->fslock, KMUTEX_FL_RECURSIVE);
}

struct task *
allocate_new_process(struct task *parent, int pid, pdir_t *new_pdir)
{
   struct process *pi, *parent_pi = parent->pi;
   struct task_and_process *tp;
   struct task *ti = NULL;
   bool common_allocs = false;
   bool arch_fields = false;

   if (UNLIKELY(!(tp = kalloc_obj(struct task_and_process))))
      goto oom_case;

   ti = &tp->main_task_obj;
   pi = &tp->process_obj;

   /* The first process (init) has as parent == kernel_process */
   ASSERT(parent != NULL);

   memcpy(ti, parent, sizeof(struct task));
   memcpy(pi, parent_pi, sizeof(struct process));

   if (MOD_debugpanel) {

      if (UNLIKELY(!(pi->debug_cmdline = kzmalloc(PROCESS_CMDLINE_BUF_SIZE))))
         goto oom_case;

      if (parent_pi->debug_cmdline) {
         memcpy(pi->debug_cmdline,
                parent_pi->debug_cmdline,
                PROCESS_CMDLINE_BUF_SIZE);
      }
   }

   pi->parent_pid = parent_pi->pid;
   pi->pdir = new_pdir;
   pi->ref_count = 1;
   pi->pid = pid;
   pi->did_call_execve = false;
   pi->automatic_reaping = false;
   pi->cwd.fs = NULL;
   pi->vforked = false;
   pi->inherited_mmap_heap = false;

   if (new_pdir != parent_pi->pdir) {

      if (parent_pi->mi) {
         if (UNLIKELY(!(pi->mi = duplicate_mappings_info(pi, parent_pi->mi))))
            goto oom_case;
      }

      if (pi->elf)
         retain_subsys_flock(pi->elf);

   } else {
      pi->vforked = true;
      pi->inherited_mmap_heap = !!pi->mi;
   }

   ti->pi = pi;
   ti->tid = pid;
   ti->is_main_thread = true;
   ti->timer_ready = false;

   /*
    * From fork(2):
    *    The child's set of pending signals is initially empty.
    *
    * From sigpending(2):
    *    A child created via fork(2) initially has an empty pending signal
    *    set; the pending signal set is preserved across an execve(2).
    */
   drop_all_pending_signals(ti);

   /* Reset sched ticks in the new process */
   bzero(&ti->ticks, sizeof(ti->ticks));

   /* Copy parent's `cwd` while retaining the `fs` and the inode obj */
   process_set_cwd2_nolock_raw(pi, &parent_pi->cwd);

   if (UNLIKELY(!(common_allocs = do_common_task_allocs(ti, true))))
      goto oom_case;

   if (UNLIKELY(!(arch_fields = arch_specific_new_task_setup(ti, parent))))
      goto oom_case;

   arch_specific_new_proc_setup(pi, parent_pi); // NOTE: cannot fail
   init_task_lists(ti);
   init_process_lists(pi);
   list_add_tail(&parent_pi->children, &ti->siblings_node);

   pi->proc_tty = parent_pi->proc_tty;
   return ti;

oom_case:

   if (ti) {

      if (arch_fields)
         arch_specific_free_task(ti);

      if (common_allocs)
         free_common_task_allocs(ti);

      if (pi->cwd.fs) {
         vfs_release_inode_at(&pi->cwd);
         release_obj(pi->cwd.fs);
      }

      process_free_mappings_info(ti->pi);

      if (MOD_debugpanel && pi->debug_cmdline)
         kfree2(pi->debug_cmdline, PROCESS_CMDLINE_BUF_SIZE);

      kfree_obj((void *)ti, struct task_and_process);
   }

   return NULL;
}

struct task *allocate_new_thread(struct process *pi, int tid, bool alloc_bufs)
{
   ASSERT(pi != NULL);
   struct task *process_task = get_process_task(pi);
   struct task *ti = kzalloc_obj(struct task);

   if (!ti || !(ti->pi = pi) || !do_common_task_allocs(ti, alloc_bufs)) {

      if (ti) /* do_common_task_allocs() failed */
         free_common_task_allocs(ti);

      kfree_obj(ti, struct task);
      return NULL;
   }

   ti->tid = tid;
   ti->is_main_thread = false;

   init_task_lists(ti);
   arch_specific_new_task_setup(ti, process_task);
   return ti;
}

static void free_process_int(struct process *pi)
{
   ASSERT(get_ref_count(pi) > 0);

   if (LIKELY(pi->cwd.fs != NULL)) {

      /*
       * When we change the current directory or when we fork a process, we
       * set a new value for the struct vfs_path pi->cwd which has its inode
       * retained as well as its owning fs. Here we have to release those
       * ref-counts.
       */

      vfs_release_inode_at(&pi->cwd);
      release_obj(pi->cwd.fs);
   }

   if (release_obj(pi) == 0) {

      if (MOD_debugpanel)
         kfree2(pi->debug_cmdline, PROCESS_CMDLINE_BUF_SIZE);

      arch_specific_free_proc(pi);
      kfree_obj((void *)get_process_task(pi), struct task_and_process);
   }
}

void free_task(struct task *ti)
{
   ASSERT_TASK_STATE(ti->state, TASK_STATE_ZOMBIE);
   arch_specific_free_task(ti);

   ASSERT(!ti->kernel_stack);
   ASSERT(!ti->io_copybuf);
   ASSERT(!ti->args_copybuf);

   list_remove(&ti->siblings_node);

   if (is_main_thread(ti))
      free_process_int(ti->pi);
   else
      kfree_obj(ti, struct task);
}

void *task_temp_kernel_alloc(size_t size)
{
   struct task *curr = get_curr_task();
   void *ptr = NULL;

   disable_preemption();
   {
      ptr = kmalloc(size);

      if (ptr) {

         struct kernel_alloc *alloc = kzalloc_obj(struct kernel_alloc);

         if (alloc) {

            bintree_node_init(&alloc->node);
            alloc->vaddr = ptr;
            alloc->size = size;

            bintree_insert_ptr(&curr->kallocs_tree_root,
                               alloc,
                               struct kernel_alloc,
                               node,
                               vaddr);

         } else {

            kfree2(ptr, size);
            ptr = NULL;
         }
      }
   }
   enable_preemption();
   return ptr;
}

void task_temp_kernel_free(void *ptr)
{
   struct task *curr = get_curr_task();
   struct kernel_alloc *alloc;

   if (!ptr)
      return;

   disable_preemption();
   {
      alloc = bintree_find_ptr(curr->kallocs_tree_root,
                               ptr,
                               struct kernel_alloc,
                               node,
                               vaddr);

      ASSERT(alloc != NULL);

      kfree2(alloc->vaddr, alloc->size);

      bintree_remove_ptr(&curr->kallocs_tree_root,
                         alloc,
                         struct kernel_alloc,
                         node,
                         vaddr);

      kfree_obj(alloc, struct kernel_alloc);
   }
   enable_preemption();
}

void set_kernel_process_pdir(pdir_t *pdir)
{
   kernel_process_pi->pdir = pdir;
}


/*
 * ***************************************************************
 *
 * SYSCALLS
 *
 * ***************************************************************
 */

mode_t sys_umask(mode_t mask)
{
   struct process *pi = get_curr_proc();
   mode_t old = pi->umask;
   pi->umask = mask & 0777;
   return old;
}

int sys_getpid(void)
{
   return get_curr_proc()->pid;
}

int sys_gettid(void)
{
   return get_curr_task()->tid;
}

int kthread_join(int tid, bool ignore_signals)
{
   struct task *curr = get_curr_task();
   struct task *ti;
   int rc = 0;

   ASSERT(is_preemption_enabled());
   disable_preemption();

   while ((ti = get_task(tid))) {

      if (pending_signals()) {

         wait_obj_reset(&curr->wobj);

         if (!ignore_signals) {
            rc = -EINTR;
            break;
         }
      }

      prepare_to_wait_on(WOBJ_TASK,
                         TO_PTR(ti->tid),
                         NO_EXTRA,
                         &ti->tasks_waiting_list);

      enter_sleep_wait_state();
      /* after enter_sleep_wait_state() the preemption is be enabled */

      disable_preemption();
   }

   enable_preemption();
   return rc;
}

int kthread_join_all(const int *tids, size_t n, bool ignore_signals)
{
   int rc = 0;

   for (size_t i = 0; i < n; i++) {

      rc = kthread_join(tids[i], ignore_signals);

      if (rc)
         break;
   }

   return rc;
}

void
unblock_parent_of_vforked_child(struct process *pi)
{
   struct task *parent;

   ASSERT(!is_preemption_enabled());
   ASSERT(pi->vforked);
   parent = get_task(pi->parent_pid);

   ASSERT(parent != NULL);
   ASSERT(parent->stopped);
   ASSERT(parent->vfork_stopped);

   parent->stopped = false;
   parent->vfork_stopped = false;
}

void
vforked_child_transfer_dispose_mi(struct process *pi)
{
   struct task *parent;
   ASSERT(pi->vforked);

   parent = get_task(pi->parent_pid);

   if (!pi->inherited_mmap_heap) {

      /* We're in a vfork-ed child: the parent cannot die */
      ASSERT(parent != NULL);

      /*
      * If we didn't inherit mappings info from the parent and the parent
      * didn't run the whole time: its `mi` must continue to be NULL.
      */
      ASSERT(!parent->pi->mi);

      /*
      * Transfer the ownership of our mappings info, created after vfork(),
      * back to our parent. We do this trick because in Tilck processes
      * are so lightweight that they don't have even a mappings object.
      * Since a vforked-child is allowed to use mmap() and that must affect
      * parent's address space, because that's the *same* address space,
      * in the corner case the child called mmap(), we must transfer that
      * object back to the parent.
      */
      parent->pi->mi = pi->mi;

   } else {

      /*
       * The pi->mi object was inherited and, therefore, the one in the parent
       * MUST BE the same.
       */
      ASSERT(parent->pi->mi != NULL);
      ASSERT(parent->pi->mi == pi->mi);
   }

  /*
   * Reset the mappings info object, no matter if we inherited it from the
   * parent or not. In case we did inherited, that was never ours. In the
   * other case, we didn't inherit it, but we just transferred its ownership
   * to the parent process. So, we want process_free_mappings_info() to
   * never found it non-null the case of vforked-processes.
   */
   pi->mi = NULL;
}

int sys_getppid(void)
{
   return get_curr_proc()->parent_pid;
}

/* create new session */
int sys_setsid(void)
{
   struct process *pi = get_curr_proc();
   int rc = -EPERM;

   disable_preemption();

   if (!sched_count_proc_in_group(pi->pid)) {
      pi->pgid = pi->pid;
      pi->sid = pi->pid;
      pi->proc_tty = NULL;
      rc = pi->sid;
   }

   enable_preemption();
   return rc;
}

/* get current session id */
int sys_getsid(int pid)
{
   return get_curr_proc()->sid;
}

int sys_setpgid(int pid, int pgid)
{
   struct process *pi;
   int sid;
   int rc = 0;

   if (pgid < 0)
      return -EINVAL;

   disable_preemption();

   if (!pid) {

      pi = get_curr_proc();

   } else {

      pi = get_process(pid);

      if (!pi) {
         rc = -ESRCH;
         goto out;
      }

      if (pi->did_call_execve) {
         rc = -EACCES;
         goto out;
      }

      /* Cannot move processes in other sessions */
      if (pi->sid != get_curr_proc()->sid) {
         rc = -EPERM;
         goto out;
      }
   }

   if (pgid) {

      sid = sched_get_session_of_group(pgid);

      /*
      * If the process group exists (= there's at least one process with
      * pi->pgid == pgid), it must be in the same session as `pid` and the
      * calling process.
      */
      if (sid >= 0 && sid != pi->sid) {
         rc = -EPERM;
         goto out;
      }

      /* Set process' pgid to `pgid` */
      pi->pgid = pgid;

   } else {

      /* pgid is 0: make the process a group leader */
      pi->pgid = pi->pid;
   }

out:
   enable_preemption();
   return rc;
}

int sys_getpgid(int pid)
{
   struct process *pi;
   int ret = -ESRCH;

   if (!pid)
      return get_curr_proc()->pgid;

   disable_preemption();
   {
      if ((pi = get_process(pid)))
         ret = pi->pgid;
   }
   enable_preemption();
   return ret;
}

int sys_getpgrp(void)
{
   return get_curr_proc()->pgid;
}

int sys_prctl(int option, ulong a2, ulong a3, ulong a4, ulong a5)
{
   // TODO: actually implement sys_prctl()

   switch (option) {

      case PR_SET_NAME:
         // printk("[TID: %d] PR_SET_NAME '%s'\n", get_curr_tid(), (void *)a2);
         // TODO: support PR_SET_NAME in prctl().
         return -EINVAL;

      case PR_GET_NAME:
         // TODO: support PR_GET_NAME in prctl().
         return -EINVAL;

      default:
         printk(
            "[TID: %d] Unknown prctl() option: %d\n",
            get_curr_tid(), option
         );
         return -EINVAL;
   }
}


int
setup_first_process(pdir_t *pdir, struct task **ti_ref)
{
   struct task *ti;
   struct process *pi;

   VERIFY(create_new_pid() == 1);

   if (!(ti = allocate_new_process(kernel_process, 1, pdir)))
      return -ENOMEM;

   pi = ti->pi;
   pi->pgid = 1;
   pi->sid = 1;
   pi->umask = 0022;
   ti->state = TASK_STATE_RUNNING;
   add_task(ti);
   memcpy(pi->str_cwd, "/", 2);
   *ti_ref = ti;
   return 0;
}

void
finalize_usermode_task_setup(struct task *ti, regs_t *user_regs)
{
   ASSERT(!is_preemption_enabled());

   ASSERT_TASK_STATE(ti->state, TASK_STATE_RUNNING);
   task_change_state(ti, TASK_STATE_RUNNABLE);

   ti->running_in_kernel = 0;
   ASSERT(ti->kernel_stack != NULL);

   task_info_reset_kernel_stack(ti);
   ti->state_regs--;             // make room for a regs_t struct in the stack
   *ti->state_regs = *user_regs; // copy the regs_t struct we prepared before
}

int setup_process(struct elf_program_info *pinfo,
                  struct task *ti,
                  const char *const *argv,
                  const char *const *env,
                  struct task **ti_ref,
                  regs_t *r)
{
   int rc = 0;
   u32 argv_elems = 0;
   u32 env_elems = 0;
   pdir_t *old_pdir;
   struct process *pi = NULL;

   ASSERT(!is_preemption_enabled());

   *ti_ref = NULL;
   setup_usermode_task_regs(r, pinfo->entry, pinfo->stack);

   /* Switch to the new page directory (we're going to write on user's stack) */
   old_pdir = get_curr_pdir();
   set_curr_pdir(pinfo->pdir);

   while (READ_PTR(&argv[argv_elems])) argv_elems++;
   while (READ_PTR(&env[env_elems])) env_elems++;

   if ((rc = push_args_on_user_stack(r, argv, argv_elems, env, env_elems)))
      goto err;

   if (UNLIKELY(!ti)) {

      /* Special case: applies only for `init`, the first process */

      if ((rc = setup_first_process(pinfo->pdir, &ti)))
         goto err;

      ASSERT(ti != NULL);
      pi = ti->pi;

   } else {

      /*
       * Common case: we're creating a new process using the data structures
       * and the PID from a forked child (the `ti` task).
       */

      pi = ti->pi;

      if (pi->vforked) {

        /*
         * In case of vforked processes, we cannot remove any mappings and we
         * need some special management for the mappings info object (pi->mi).
         */
         vforked_child_transfer_dispose_mi(pi);

      } else {

         remove_all_user_zero_mem_mappings(pi);
         remove_all_file_mappings(pi);
         process_free_mappings_info(pi);

         ASSERT(old_pdir == pi->pdir);
         pdir_destroy(pi->pdir);

         if (pi->elf)
            release_subsys_flock(pi->elf);
      }

      pi->pdir = pinfo->pdir;
      old_pdir = NULL;

      /* NOTE: not calling arch_specific_free_task() */
      VERIFY(arch_specific_new_task_setup(ti, NULL));

      arch_specific_free_proc(pi);
      arch_specific_new_proc_setup(pi, NULL);
   }

   pi->elf = pinfo->lf;
   *ti_ref = ti;
   return 0;

err:
   ASSERT(rc != 0);

   if (old_pdir) {
      set_curr_pdir(old_pdir);
      pdir_destroy(pinfo->pdir);
   }

   return rc;
}

void task_info_reset_kernel_stack(struct task *ti)
{
   ulong bottom = (ulong)ti->kernel_stack + KERNEL_STACK_SIZE - 1;
   ti->state_regs = (regs_t *)(bottom & POINTER_ALIGN_MASK);
}

void kthread_exit(void)
{
   /*
    * WARNING: DO NOT USE ANY STACK VARIABLES HERE.
    *
    * The call to switch_to_initial_kernel_stack() will mess-up your whole stack
    * (but that's what it is supposed to do). In this function, only global
    * variables can be accessed.
    *
    * This function gets called automatically when a kernel thread function
    * returns, but it can be called manually as well at any point.
    */
   disable_preemption();

   wake_up_tasks_waiting_on(get_curr_task(), task_died);
   task_change_state(get_curr_task(), TASK_STATE_ZOMBIE);

   /* WARNING: the following call discards the whole stack! */
   switch_to_initial_kernel_stack();

   /* Run the scheduler */
   do_schedule();

   /* The scheduler will never return to a ZOMBIE task */
   NOT_REACHED();
}

NODISCARD int
kthread_create2(kthread_func_ptr func, const char *name, int fl, void *arg)
{
   struct task *ti;
   int tid, ret = -ENOMEM;
   regs_t r;

   ASSERT(name != NULL);
   kthread_create_init_regs_arch(&r, func);
   disable_preemption();

   tid = create_new_kernel_tid();

   if (tid < 0) {
      ret = -EAGAIN;
      goto end;
   }

   ti = allocate_new_thread(kernel_process->pi, tid, !!(fl & KTH_ALLOC_BUFS));

   if (!ti)
      goto end;

   ASSERT(is_kernel_thread(ti));

   if (*name == '&')
      name++;         /* see the macro kthread_create() */

   ti->kthread_name = name;
   ti->state = TASK_STATE_RUNNABLE;
   ti->running_in_kernel = IN_SYSCALL_FLAG;
   task_info_reset_kernel_stack(ti);
   kthread_create_setup_initial_stack(ti, &r, arg);

   ret = ti->tid;

   if (fl & KTH_WORKER_THREAD)
      ti->worker_thread = arg;

   /*
    * After the following call to add_task(), given that preemption is enabled,
    * there is NO GUARANTEE that the `tid` returned by this function will still
    * belong to a valid kernel thread. For example, the kernel thread might run
    * and terminate before the caller has the chance to run. Therefore, it is up
    * to the caller to be prepared for that.
    */

   add_task(ti);
   enable_preemption();

end:
   return ret; /* tid or error */
}
