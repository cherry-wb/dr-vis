/* **********************************************************
 * Copyright (c) 2011-2013 Google, Inc.  All rights reserved.
 * Copyright (c) 2000-2010 VMware, Inc.  All rights reserved.
 * **********************************************************/

/*
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 * * Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * 
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * 
 * * Neither the name of VMware, Inc. nor the names of its contributors may be
 *   used to endorse or promote products derived from this software without
 *   specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL VMWARE, INC. OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

/* Copyright (c) 2003-2007 Determina Corp. */
/* Copyright (c) 2001-2003 Massachusetts Institute of Technology */
/* Copyright (c) 2000-2001 Hewlett-Packard Company */

/*
 * signal.c - dynamorio signal handler
 */

#include <unistd.h>
#include <errno.h>
#undef errno

/* We want to build on older toolchains so we have our own copy of signal
 * data structures
 */
#include "include/sigcontext.h"
#include "include/signalfd.h"

#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <linux/sched.h>
#include <string.h> /* for memcpy and memset */
#include "../globals.h"
#include "os_private.h"
#include "../fragment.h"
#include "../fcache.h"
#include "../perfctr.h"
#include "arch.h"
#include "../monitor.h" /* for trace_abort */
#include "../link.h" /* for linking interrupted fragment_t */
#include "instr.h" /* to find target of SIGSEGV */
#include "decode.h" /* to find target of SIGSEGV */
#include "decode_fast.h" /* to handle self-mod code */
#include "../synch.h"
#include "../nudge.h"
#include "disassemble.h"

#include "include/syscall.h"

#ifdef CLIENT_INTERFACE
# include "instrument.h"
#endif

#ifdef VMX86_SERVER
# include <errno.h>
#endif

/* important reference files:
 *   /usr/include/asm/signal.h
 *   /usr/include/asm/siginfo.h
 *   /usr/include/asm/ucontext.h
 *   /usr/include/asm/sigcontext.h
 *   /usr/include/sys/ucontext.h (see notes below...asm one is more useful)
 *   /usr/include/bits/sigaction.h
 *   /usr/src/linux/kernel/signal.c
 *   /usr/src/linux/arch/i386/kernel/signal.c
 *   /usr/src/linux/include/asm-i386/signal.h
 *   /usr/src/linux/include/asm-i386/sigcontext.h
 *   /usr/src/linux/include/asm-i386/ucontext.h
 *   /usr/src/linux/include/linux/signal.h
 *   /usr/src/linux/include/linux/sched.h (sas_ss_flags, on_sig_stack)
 * ditto with x86_64, plus:
 *   /usr/src/linux/arch/x86_64/ia32/ia32_signal.c
 */


/**** data structures ***************************************************/

/* handler with SA_SIGINFO flag set gets three arguments: */
typedef void (*handler_t)(int, siginfo_t *, void *);

/* default actions */
enum {
    DEFAULT_TERMINATE,
    DEFAULT_TERMINATE_CORE,
    DEFAULT_IGNORE,
    DEFAULT_STOP,
    DEFAULT_CONTINUE,
};

int default_action[] = {
    /* nothing    0 */   DEFAULT_IGNORE,
    /* SIGHUP     1 */   DEFAULT_TERMINATE,
    /* SIGINT     2 */   DEFAULT_TERMINATE,
    /* SIGQUIT    3 */   DEFAULT_TERMINATE_CORE,
    /* SIGILL     4 */   DEFAULT_TERMINATE_CORE,
    /* SIGTRAP    5 */   DEFAULT_TERMINATE_CORE,
    /* SIGABRT/SIGIOT 6 */   DEFAULT_TERMINATE_CORE,
    /* SIGBUS     7 */   DEFAULT_TERMINATE, /* should be CORE */
    /* SIGFPE     8 */   DEFAULT_TERMINATE_CORE,
    /* SIGKILL    9 */   DEFAULT_TERMINATE,
    /* SIGUSR1   10 */   DEFAULT_TERMINATE,
    /* SIGSEGV   11 */   DEFAULT_TERMINATE_CORE,
    /* SIGUSR2   12 */   DEFAULT_TERMINATE,
    /* SIGPIPE   13 */   DEFAULT_TERMINATE,
    /* SIGALRM   14 */   DEFAULT_TERMINATE,
    /* SIGTERM   15 */   DEFAULT_TERMINATE,
    /* SIGSTKFLT 16 */   DEFAULT_TERMINATE,
    /* SIGCHLD   17 */   DEFAULT_IGNORE,
    /* SIGCONT   18 */   DEFAULT_CONTINUE,
    /* SIGSTOP   19 */   DEFAULT_STOP,
    /* SIGTSTP   20 */   DEFAULT_STOP,
    /* SIGTTIN   21 */   DEFAULT_STOP,
    /* SIGTTOU   22 */   DEFAULT_STOP,
    /* SIGURG    23 */   DEFAULT_IGNORE,
    /* SIGXCPU   24 */   DEFAULT_TERMINATE,
    /* SIGXFSZ   25 */   DEFAULT_TERMINATE,
    /* SIGVTALRM 26 */   DEFAULT_TERMINATE,
    /* SIGPROF   27 */   DEFAULT_TERMINATE,
    /* SIGWINCH  28 */   DEFAULT_IGNORE,
    /* SIGIO/SIGPOLL/SIGLOST 29 */ DEFAULT_TERMINATE,
    /* SIGPWR    30 */   DEFAULT_TERMINATE,
    /* SIGSYS/SIGUNUSED 31 */ DEFAULT_TERMINATE,

    /* ASSUMPTION: all real-time have default of terminate...XXX: ok? */
    /* 32 */ DEFAULT_TERMINATE,
    /* 33 */ DEFAULT_TERMINATE,
    /* 34 */ DEFAULT_TERMINATE,
    /* 35 */ DEFAULT_TERMINATE,
    /* 36 */ DEFAULT_TERMINATE,
    /* 37 */ DEFAULT_TERMINATE,
    /* 38 */ DEFAULT_TERMINATE,
    /* 39 */ DEFAULT_TERMINATE,
    /* 40 */ DEFAULT_TERMINATE,
    /* 41 */ DEFAULT_TERMINATE,
    /* 42 */ DEFAULT_TERMINATE,
    /* 43 */ DEFAULT_TERMINATE,
    /* 44 */ DEFAULT_TERMINATE,
    /* 45 */ DEFAULT_TERMINATE,
    /* 46 */ DEFAULT_TERMINATE,
    /* 47 */ DEFAULT_TERMINATE,
    /* 48 */ DEFAULT_TERMINATE,
    /* 49 */ DEFAULT_TERMINATE,
    /* 50 */ DEFAULT_TERMINATE,
    /* 51 */ DEFAULT_TERMINATE,
    /* 52 */ DEFAULT_TERMINATE,
    /* 53 */ DEFAULT_TERMINATE,
    /* 54 */ DEFAULT_TERMINATE,
    /* 55 */ DEFAULT_TERMINATE,
    /* 56 */ DEFAULT_TERMINATE,
    /* 57 */ DEFAULT_TERMINATE,
    /* 58 */ DEFAULT_TERMINATE,
    /* 59 */ DEFAULT_TERMINATE,
    /* 60 */ DEFAULT_TERMINATE,
    /* 61 */ DEFAULT_TERMINATE,
    /* 62 */ DEFAULT_TERMINATE,
    /* 63 */ DEFAULT_TERMINATE,
    /* 64 */ DEFAULT_TERMINATE,
};

/* We know that many signals are always asynchronous.
 * Others, however, may be synchronous or may not -- e.g., another process
 * could send us a SIGSEGV, and there is no way we can tell whether it
 * was generated by a real memory fault or not.  Thus we have to assume
 * that we must not delay any SIGSEGV deliveries.
 */
bool can_always_delay[] = {
    /* nothing    0 */             true,
    /* SIGHUP     1 */             true,
    /* SIGINT     2 */             true,
    /* SIGQUIT    3 */             true,
    /* SIGILL     4 */            false,
    /* SIGTRAP    5 */            false,
    /* SIGABRT/SIGIOT 6 */        false,
    /* SIGBUS     7 */            false, 
    /* SIGFPE     8 */            false,
    /* SIGKILL    9 */             true,
    /* SIGUSR1   10 */             true,
    /* SIGSEGV   11 */            false,
    /* SIGUSR2   12 */             true,
    /* SIGPIPE   13 */            false,
    /* SIGALRM   14 */             true,
    /* SIGTERM   15 */             true,
    /* SIGSTKFLT 16 */            false,
    /* SIGCHLD   17 */             true,
    /* SIGCONT   18 */             true,
    /* SIGSTOP   19 */             true,
    /* SIGTSTP   20 */             true,
    /* SIGTTIN   21 */             true,
    /* SIGTTOU   22 */             true,
    /* SIGURG    23 */             true,
    /* SIGXCPU   24 */            false,
    /* SIGXFSZ   25 */             true,
    /* SIGVTALRM 26 */             true,
    /* SIGPROF   27 */             true,
    /* SIGWINCH  28 */             true,
    /* SIGIO/SIGPOLL/SIGLOST 29 */ true,
    /* SIGPWR    30 */             true,
    /* SIGSYS/SIGUNUSED 31 */     false,

    /* ASSUMPTION: all real-time can be delayed */
    /* 32 */                       true,
    /* 33 */                       true,
    /* 34 */                       true,
    /* 35 */                       true,
    /* 36 */                       true,
    /* 37 */                       true,
    /* 38 */                       true,
    /* 39 */                       true,
    /* 40 */                       true,
    /* 41 */                       true,
    /* 42 */                       true,
    /* 43 */                       true,
    /* 44 */                       true,
    /* 45 */                       true,
    /* 46 */                       true,
    /* 47 */                       true,
    /* 48 */                       true,
    /* 49 */                       true,
    /* 50 */                       true,
    /* 51 */                       true,
    /* 52 */                       true,
    /* 53 */                       true,
    /* 54 */                       true,
    /* 55 */                       true,
    /* 56 */                       true,
    /* 57 */                       true,
    /* 58 */                       true,
    /* 59 */                       true,
    /* 60 */                       true,
    /* 61 */                       true,
    /* 62 */                       true,
    /* 63 */                       true,
    /* 64 */                       true,
};

static inline bool
sig_is_alarm_signal(int sig)
{
    return (sig == SIGALRM || sig == SIGVTALRM || sig == SIGPROF);
}

/* we do not use SIGSTKSZ b/c for things like code modification
 * we end up calling many core routines and so want more space
 * (though currently non-debug stack size == SIGSTKSZ (8KB))
 */
/* this size is assumed in heap.c's threadunits_exit leak relaxation */
#define SIGSTACK_SIZE DYNAMORIO_STACK_SIZE

/* this flag not defined in our headers */
#define SA_RESTORER 0x04000000

/**** kernel_sigset_t ***************************************************/

/* defines and typedefs are exported in os_exports.h for siglongjmp */

/* most of these are from /usr/src/linux/include/linux/signal.h */
static inline 
void kernel_sigemptyset(kernel_sigset_t *set)
{
    memset(set, 0, sizeof(kernel_sigset_t));
}

static inline 
void kernel_sigfillset(kernel_sigset_t *set)
{
    memset(set, -1, sizeof(kernel_sigset_t));
}

static inline 
void kernel_sigaddset(kernel_sigset_t *set, int _sig)
{
    uint sig = _sig - 1;
    if (_NSIG_WORDS == 1)
        set->sig[0] |= 1UL << sig;
    else
        set->sig[sig / _NSIG_BPW] |= 1UL << (sig % _NSIG_BPW);
}

static inline 
void kernel_sigdelset(kernel_sigset_t *set, int _sig)
{
    uint sig = _sig - 1;
    if (_NSIG_WORDS == 1)
        set->sig[0] &= ~(1UL << sig);
    else
        set->sig[sig / _NSIG_BPW] &= ~(1UL << (sig % _NSIG_BPW));
}

static inline 
bool kernel_sigismember(kernel_sigset_t *set, int _sig)
{
    int sig = _sig - 1; /* go to 0-based */
    if (_NSIG_WORDS == 1)
        return CAST_TO_bool(1 & (set->sig[0] >> sig));
    else
        return CAST_TO_bool(1 & (set->sig[sig / _NSIG_BPW] >> (sig % _NSIG_BPW)));
}

/* FIXME: how does libc do this? */
static inline
void copy_kernel_sigset_to_sigset(kernel_sigset_t *kset, sigset_t *uset)
{
    int sig;
#ifdef DEBUG
    int rc =
#endif 
        sigemptyset(uset);
    ASSERT(rc == 0);
    /* do this the slow way...I don't want to make assumptions about
     * structure of user sigset_t
     */
    for (sig=1; sig<=MAX_SIGNUM; sig++) {
        if (kernel_sigismember(kset, sig))
            sigaddset(uset, sig);
    }
}

/* FIXME: how does libc do this? */
static inline
void copy_sigset_to_kernel_sigset(sigset_t *uset, kernel_sigset_t *kset)
{
    int sig;
    kernel_sigemptyset(kset);
    /* do this the slow way...I don't want to make assumptions about
     * structure of user sigset_t
     */
    for (sig=1; sig<=MAX_SIGNUM; sig++) {
        if (sigismember(uset, sig))
            kernel_sigaddset(kset, sig);
    }
}

/**** frames *********************************************************/

/* kernel's notion of sigaction has fields in different order from that
 * used in glibc (I looked at glibc-2.2.4/sysdeps/unix/sysv/linux/i386/sigaction.c)
 * also, /usr/include/asm/signal.h lists both versions
 * I deliberately give kernel_sigaction_t's fields different names to help
 * avoid confusion.
 * (2.1.20 kernel has mask as 2nd field instead, and is expected to be passed
 * in to the non-rt sigaction() call, which we do not yet support)
 */
struct _kernel_sigaction_t {
    handler_t handler;
    unsigned long flags;
    void (*restorer)(void);
    kernel_sigset_t mask;
}; /* typedef in os_private.h */

/* kernel's notion of ucontext is different from glibc's!
 * this is adapted from asm/ucontext.h:
 */
typedef struct {
    unsigned long     uc_flags;
    struct ucontext  *uc_link;
    stack_t           uc_stack;
    struct sigcontext uc_mcontext;
    kernel_sigset_t   uc_sigmask; /* mask last for extensibility */
} kernel_ucontext_t;

/* we assume frames look like this, with rt_sigframe used if SA_SIGINFO is set
 * (these are from /usr/src/linux/arch/i386/kernel/signal.c for kernel 2.4.17)
 */

#define RETCODE_SIZE 8

typedef struct sigframe {
    char *pretcode;
    int sig;
    struct sigcontext sc;
    /* Since 2.6.28, this fpstate has been unused and the real fpstate
     * is at the end of the struct so it can include xstate
     */
    struct _fpstate fpstate;
    unsigned long extramask[_NSIG_WORDS-1];
    char retcode[RETCODE_SIZE];
    /* FIXME: this is a field I added, so our frame looks different from
     * the kernel's...but where else can I store sig where the app won't
     * clobber it?
     * WARNING: our handler receives only rt frames, and we construct
     * plain frames but never pass them to the kernel (on sigreturn() we
     * just go to new context and interpret from there), so the only
     * transparency problem here is if app tries to build its own plain
     * frame and call sigreturn() unrelated to signal delivery.
     * UPDATE: actually we do invoke SYS_*sigreturn
     */
    int sig_noclobber;
    /* In 2.6.28+, fpstate/xstate goes here */
} sigframe_plain_t;

/* the rt frame is used for SA_SIGINFO signals */
typedef struct rt_sigframe {
    char *pretcode;
#ifdef X64
# ifdef VMX86_SERVER
    siginfo_t info;
    kernel_ucontext_t uc;
# else
    kernel_ucontext_t uc;
    siginfo_t info;
# endif
#else
    int sig;
    siginfo_t *pinfo;
    void *puc;
    siginfo_t info;
    kernel_ucontext_t uc;
    /* Prior to 2.6.28, "struct _fpstate fpstate" was here.  Rather than
     * try to reproduce that exact layout and detect the underlying kernel
     * (the safest way would be to send ourselves a signal and examine the
     * frame, rather than relying on uname, to handle backports), we use
     * the new layout even on old kernels.  The app should use the fpstate
     * pointer in the sigcontext anyway.
     */
    char retcode[RETCODE_SIZE];
#endif
    /* In 2.6.28+, fpstate/xstate goes here */
} sigframe_rt_t;


/* if no app sigaction, it's RT, since that's our handler */
#define IS_RT_FOR_APP(info, sig) \
  IF_X64_ELSE(true, ((info)->app_sigaction[(sig)] == NULL ? true : \
                     (TEST(SA_SIGINFO, (info)->app_sigaction[(sig)]->flags))))

/* kernel sets size and sp to 0 for SS_DISABLE
 * when asked, will hand back SS_ONSTACK only if current xsp is inside the
 * alt stack; otherwise, if an alt stack is registered, it will give flags of 0
 * We do not support the "legacy stack switching" that uses the restorer field
 * as seen in kernel sources.
 */
#define APP_HAS_SIGSTACK(info) \
  ((info)->app_sigstack.ss_sp != NULL && (info)->app_sigstack.ss_flags != SS_DISABLE)

/* even though we don't execute xsave ourselves, kernel will do xrestore on sigreturn
 * so we have to obey alignment for avx
 */
#define AVX_ALIGNMENT 64
#define FPSTATE_ALIGNMENT 16
#define XSTATE_ALIGNMENT (YMM_ENABLED() ? AVX_ALIGNMENT : FPSTATE_ALIGNMENT)

/* we have to queue up both rt and non-rt signals because we delay
 * their delivery.
 * PR 304708: we now leave in rt form right up until we copy to the
 * app stack, so that we can deliver to a client at a safe spot
 * in rt form.
 */
typedef struct _sigpending_t {
    sigframe_rt_t rt_frame;
    /* fpstate is no longer kept inside the frame, and is not always present.
     * if we delay we need to ensure we have room for it.
     * we statically keep room for full xstate in case we need it.
     */
    struct _xstate __attribute__ ((aligned (AVX_ALIGNMENT))) xstate;
#ifdef CLIENT_INTERFACE
    /* i#182/PR 449996: we provide the faulting access address for SIGSEGV, etc. */
    byte *access_address;
#endif
    /* use the sigcontext, not the mcontext (used to restart syscalls for i#1145) */
    bool use_sigcontext;
    struct _sigpending_t *next;
} sigpending_t;

/* Extra space needed to put the signal frame on the app stack.  We include the
 * size of the extra padding potentially needed to align these structs.  We
 * assume the stack pointer is 4-aligned already, so we over estimate padding
 * size by the alignment minus 4.
 */
/* An extra 4 for trailing FP_XSTATE_MAGIC2 */
#define AVX_FRAME_EXTRA (sizeof(struct _xstate) + AVX_ALIGNMENT - 4 + 4)
#define FPSTATE_FRAME_EXTRA (sizeof(struct _fpstate) + FPSTATE_ALIGNMENT - 4)
#define XSTATE_FRAME_EXTRA (YMM_ENABLED() ? AVX_FRAME_EXTRA : FPSTATE_FRAME_EXTRA)

#define AVX_DATA_SIZE (sizeof(struct _xstate) + 4)
#define FPSTATE_DATA_SIZE (sizeof(struct _fpstate))
#define XSTATE_DATA_SIZE (YMM_ENABLED() ? AVX_DATA_SIZE : FPSTATE_DATA_SIZE)

/* PR 204556: DR/clients use itimers so we need to emulate the app's usage */
typedef struct _itimer_info_t {
    /* easier to manipulate a single value than the two-field struct timeval */
    uint64 interval;
    uint64 value;
} itimer_info_t;

typedef struct _thread_itimer_info_t {
    itimer_info_t app;
    itimer_info_t app_saved;
    itimer_info_t dr;
    itimer_info_t actual;
    void (*cb)(dcontext_t *, priv_mcontext_t *);
    /* version for clients */
    void (*cb_api)(dcontext_t *, dr_mcontext_t *);
} thread_itimer_info_t;

/* We use all 3: ITIMER_REAL for clients (i#283/PR 368737), ITIMER_VIRTUAL
 * for -prof_pcs, and ITIMER_PROF for PAPI
 */
#define NUM_ITIMERS 3

/* Don't try to translate every alarm if they're piling up (PR 213040) */
#define SKIP_ALARM_XL8_MAX 3

struct _sigfd_pipe_t;
typedef struct _sigfd_pipe_t sigfd_pipe_t;

/* If we only intercept a few signals, we leave whether un-intercepted signals
 * are blocked unchanged and stored in the kernel.  If we intercept all (not
 * quite yet: PR 297033, hence the need for this macro) we emulate the mask for
 * all.
 */
#define EMULATE_SIGMASK(info, sig) \
    (DYNAMO_OPTION(intercept_all_signals) || (info)->we_intercept[(sig)])

typedef struct _thread_sig_info_t {
    /* we use kernel_sigaction_t so we don't have to translate back and forth
     * between it and libc version.
     * have to dynamically allocate app_sigaction array so we can share it.
     */
    kernel_sigaction_t **app_sigaction;

    /* True after signal_thread_inherit or signal_fork_init are called.  We
     * squash alarm or profiling signals up until this point.
     */
    bool fully_initialized;

    /* with CLONE_SIGHAND we may have to share app_sigaction */
    bool shared_app_sigaction;
    mutex_t *shared_lock;
    int *shared_refcount;
    /* signals we intercept must also be sharable */
    bool *we_intercept;

    /* DR and clients use itimers, so we need to emulate the app's itimer
     * usage.  This info is shared across CLONE_THREAD threads only for
     * NPTL in kernel 2.6.12+ so these fields are separately shareable from
     * the CLONE_SIGHAND set of fields above.
     */
    bool shared_itimer;
    /* We only need owner info.  xref i#219: we should add a known-owner
     * lock for cases where a full-fledged recursive lock is not needed.
     */
    recursive_lock_t *shared_itimer_lock;
    /* b/c a non-CLONE_THREAD thread can be created we can't just use dynamo_exited
     * and need a refcount here
     */
    int *shared_itimer_refcount;
    int *shared_itimer_underDR; /* indicates # of threads under DR control */
    thread_itimer_info_t (*itimer)[NUM_ITIMERS];

    /* cache restorer validity.  not shared: inheriter will re-populate. */
    int restorer_valid[SIGARRAY_SIZE];

    /* rest of app state */
    stack_t app_sigstack;
    sigpending_t *sigpending[SIGARRAY_SIZE];
    /* "lock" to prevent interrupting signal from messing up sigpending array */
    bool accessing_sigpending;
    kernel_sigset_t app_sigblocked;
    /* for returning the old mask (xref PR 523394) */
    kernel_sigset_t pre_syscall_app_sigblocked;
    /* for preserving the app memory (xref i#1187) */
    kernel_sigset_t pre_syscall_app_sigprocmask;
    /* for alarm signals arriving in coarse units we only attempt to xl8
     * every nth signal since coarse translation is expensive (PR 213040)
     */
    uint skip_alarm_xl8;
    /* signalfd array (lazily initialized) */
    sigfd_pipe_t *signalfd[SIGARRAY_SIZE];

    /* to handle sigsuspend we have to save blocked set */
    bool in_sigsuspend;
    kernel_sigset_t app_sigblocked_save;

    /* to inherit in children must not modify until they're scheduled */
    volatile int num_unstarted_children;
    mutex_t child_lock;

    /* our own structures */
    stack_t sigstack;
    void *sigheap; /* special heap */
    fragment_t *interrupted; /* frag we unlinked for delaying signal */
    cache_pc interrupted_pc; /* pc within frag we unlinked for delaying signal */

#ifdef RETURN_AFTER_CALL
    app_pc signal_restorer_retaddr;     /* last signal restorer, known ret exception */
#endif
} thread_sig_info_t;

/* i#27: custom data to pass to the child of a clone */
/* PR i#149/403015: clone record now passed via a new dstack */
typedef struct _clone_record_t {
    byte *dstack;          /* dstack for new thread - allocated by parent thread */
    reg_t app_thread_xsp;  /* app xsp preserved for new thread to use */
    app_pc continuation_pc;
    thread_id_t caller_id;
    int clone_sysnum;
    uint clone_flags;
    thread_sig_info_t info;
    thread_sig_info_t *parent_info;
    void *pcprofile_info;
    /* we leave some padding at base of stack for dynamorio_clone
     * to store values
     */
    reg_t for_dynamorio_clone[4];
} clone_record_t;

/* i#350: set up signal handler for safe_read/faults during init */
static thread_sig_info_t init_info;
static kernel_sigset_t init_sigmask;

#ifdef DEBUG
static bool removed_sig_handler;
#endif

/**** function prototypes ***********************************************/

/* in x86.asm */
void
master_signal_handler(int sig, siginfo_t *siginfo, kernel_ucontext_t *ucxt);

static void
intercept_signal(dcontext_t *dcontext, thread_sig_info_t *info, int sig);

static void
signal_info_init_sigaction(dcontext_t *dcontext, thread_sig_info_t *info);

static void
signal_info_exit_sigaction(dcontext_t *dcontext, thread_sig_info_t *info,
                           bool other_thread);

static bool
execute_handler_from_cache(dcontext_t *dcontext, int sig, sigframe_rt_t *our_frame,
                           struct sigcontext *sc_orig, fragment_t *f
                           _IF_CLIENT(byte *access_address));

static bool
execute_handler_from_dispatch(dcontext_t *dcontext, int sig);

/* Execute default action from code cache and may terminate the process.
 * If returns, the return value decides if caller should restore
 * the untranslated context.
 */
static bool
execute_default_from_cache(dcontext_t *dcontext, int sig, sigframe_rt_t *frame,
                           struct sigcontext *sc_orig);

static void
execute_default_from_dispatch(dcontext_t *dcontext, int sig, sigframe_rt_t *frame);

static bool
handle_alarm(dcontext_t *dcontext, int sig, kernel_ucontext_t *ucxt);

static bool
handle_suspend_signal(dcontext_t *dcontext, kernel_ucontext_t *ucxt);

static bool
handle_nudge_signal(dcontext_t *dcontext, siginfo_t *siginfo, kernel_ucontext_t *ucxt);

static struct sigcontext *
get_sigcontext_from_rt_frame(sigframe_rt_t *frame);

static void
init_itimer(dcontext_t *dcontext, bool first);

static bool
set_actual_itimer(dcontext_t *dcontext, int which, thread_sig_info_t *info,
                  bool enable);

#ifdef DEBUG
static void
dump_sigset(dcontext_t *dcontext, kernel_sigset_t *set);
#endif

static bool
is_sys_kill(dcontext_t *dcontext, byte *pc, byte *xsp, siginfo_t *info);

static void
signalfd_init(void);

static void
signalfd_exit(void);

static void
signalfd_thread_exit(dcontext_t *dcontext, thread_sig_info_t *info);

static inline int
sigaction_syscall(int sig, kernel_sigaction_t *act, kernel_sigaction_t *oact)
{
#if defined(X64) && !defined(VMX86_SERVER)
    /* PR 305020: must have SA_RESTORER for x64 */
    if (act != NULL && !TEST(SA_RESTORER, act->flags)) {
        act->flags |= SA_RESTORER;
        act->restorer = (void (*)(void)) dynamorio_sigreturn;
    }
#endif
    return dynamorio_syscall(SYS_rt_sigaction, 4, sig, act, oact,
                             sizeof(kernel_sigset_t));
}

static inline int
sigaltstack_syscall(const stack_t *newstack, stack_t *oldstack)
{
    return dynamorio_syscall(SYS_sigaltstack, 2, newstack, oldstack);
}

static inline int
getitimer_syscall(int which, struct itimerval *val)
{
    return dynamorio_syscall(SYS_getitimer, 2, which, val);
}

static inline int
setitimer_syscall(int which, struct itimerval *val, struct itimerval *old)
{
    return dynamorio_syscall(SYS_setitimer, 3, which, val, old);
}

static inline int
sigprocmask_syscall(int how, kernel_sigset_t *set, kernel_sigset_t *oset,
                    size_t sigsetsize)
{
    return dynamorio_syscall(SYS_rt_sigprocmask, 4, how, set, oset, sigsetsize);
}

static void
unblock_all_signals(kernel_sigset_t *oset)
{
    kernel_sigset_t set;
    kernel_sigemptyset(&set);
    sigprocmask_syscall(SIG_SETMASK, &set, oset, sizeof(set));
}

/* exported for stackdump.c */
bool
set_default_signal_action(int sig)
{
    kernel_sigaction_t act;
    int rc;
    memset(&act, 0, sizeof(act));
    act.handler = (handler_t) SIG_DFL;
    /* arm the signal */
    rc = sigaction_syscall(sig, &act, NULL);
    DODEBUG({ removed_sig_handler = true; });
    return (rc == 0);
}

/* We assume that signal handlers will be shared most of the time
 * (pthreads shares them)
 * Rather than start out with the handler table in local memory and then
 * having to transfer to global, we just always use global
 */
static void
handler_free(dcontext_t *dcontext, void *p, size_t size)
{
    global_heap_free(p, size HEAPACCT(ACCT_OTHER));
}

static void *
handler_alloc(dcontext_t *dcontext, size_t size)
{
    return global_heap_alloc(size HEAPACCT(ACCT_OTHER));
}

/**** floating point support ********************************************/

/* The following code is based on routines in
 *   /usr/src/linux/arch/i386/kernel/i387.c
 * and definitions in
 *   /usr/src/linux/include/asm-i386/processor.h
 *   /usr/src/linux/include/asm-i386/i387.h
 */

struct i387_fsave_struct {
    long        cwd;
    long        swd;
    long        twd;
    long        fip;
    long        fcs;
    long        foo;
    long        fos;
    long        st_space[20];   /* 8*10 bytes for each FP-reg = 80 bytes */
    long        status;         /* software status information */
};

/* note that fxsave requires that i387_fxsave_struct be aligned on
 * a 16-byte boundary
 */
struct i387_fxsave_struct {
    unsigned short      cwd;
    unsigned short      swd;
    unsigned short      twd;
    unsigned short      fop;
#ifdef X64
    long        rip;
    long        rdp;
    int         mxcsr;
    int         mxcsr_mask;
    int         st_space[32];   /* 8*16 bytes for each FP-reg = 128 bytes */
    int         xmm_space[64];  /* 16*16 bytes for each XMM-reg = 256 bytes */
    int         padding[24];
#else
    long        fip;
    long        fcs;
    long        foo;
    long        fos;
    long        mxcsr;
    long        reserved;
    long        st_space[32];   /* 8*16 bytes for each FP-reg = 128 bytes */
    long        xmm_space[32];  /* 8*16 bytes for each XMM-reg = 128 bytes */
    long        padding[56];
#endif
} __attribute__ ((aligned (16)));

union i387_union {
    struct i387_fsave_struct    fsave;
    struct i387_fxsave_struct   fxsave;
};

#ifndef X64
/* For 32-bit if we use fxsave we need to convert it to the kernel's struct.
 * For 64-bit the kernel's struct is identical to the fxsave format.
 */
static uint
twd_fxsr_to_i387(struct i387_fxsave_struct *fxsave)
{
    struct _fpxreg *st = NULL;
    uint twd = (uint) fxsave->twd;
    uint tag;
    uint ret = 0xffff0000;
    int i;
    for (i = 0 ; i < 8 ; i++) {
        if (TEST(0x1, twd)) {
            st = (struct _fpxreg *) &fxsave->st_space[i*4];

            switch (st->exponent & 0x7fff) {
            case 0x7fff:
                tag = 2;        /* Special */
                break;
            case 0x0000:
                if (st->significand[0] == 0 &&
                    st->significand[1] == 0 &&
                    st->significand[2] == 0 &&
                    st->significand[3] == 0) {
                    tag = 1;    /* Zero */
                } else {
                    tag = 2;    /* Special */
                }
                break;
            default:
                if (TEST(0x8000, st->significand[3])) {
                    tag = 0;    /* Valid */
                } else {
                    tag = 2;    /* Special */
                }
                break;
            }
        } else {
            tag = 3;            /* Empty */
        }
        ret |= (tag << (2 * i));
        twd = twd >> 1;
    }
    return ret;
}

static void
convert_fxsave_to_fpstate(struct _fpstate *fpstate,
                          struct i387_fxsave_struct *fxsave)
{
    int i;

    fpstate->cw = (uint)fxsave->cwd | 0xffff0000;
    fpstate->sw = (uint)fxsave->swd | 0xffff0000;
    fpstate->tag = twd_fxsr_to_i387(fxsave);
    fpstate->ipoff = fxsave->fip;
    fpstate->cssel = fxsave->fcs | ((uint)fxsave->fop << 16);
    fpstate->dataoff = fxsave->foo;
    fpstate->datasel = fxsave->fos;

    for (i = 0; i < 8; i++) {
        memcpy(&fpstate->_st[i], &fxsave->st_space[i*4], sizeof(fpstate->_st[i]));
    }

    fpstate->status = fxsave->swd;
    fpstate->magic = X86_FXSR_MAGIC;

    memcpy(&fpstate->_fxsr_env[0], fxsave,
           sizeof(struct i387_fxsave_struct));
}
#endif /* !X64 */

static void
save_xmm(dcontext_t *dcontext, sigframe_rt_t *frame)
{
    /* see comments at call site: can't just do xsave */
    int i;
    struct sigcontext *sc = get_sigcontext_from_rt_frame(frame);
    struct _xstate *xstate = (struct _xstate *) sc->fpstate;
    if (!preserve_xmm_caller_saved())
        return;
    if (YMM_ENABLED()) {
        /* all ymm regs are in our mcontext.  the only other thing
         * in xstate is the xgetbv.
         */
        uint bv_high, bv_low;
        dr_xgetbv(&bv_high, &bv_low);
        xstate->xstate_hdr.xstate_bv = (((uint64)bv_high)<<32) | bv_low;
    }
    for (i=0; i<NUM_XMM_SAVED; i++) {
        /* we assume no padding */
#ifdef X64
        /* __u32 xmm_space[64] */
        memcpy(&sc->fpstate->xmm_space[i*4], &get_mcontext(dcontext)->ymm[i],
               XMM_REG_SIZE);
        if (YMM_ENABLED()) {
            /* i#637: ymm top halves are inside struct _xstate */
            memcpy(&xstate->ymmh.ymmh_space[i * 4], 
                   ((void *)&get_mcontext(dcontext)->ymm[i]) + XMM_REG_SIZE,
                   YMMH_REG_SIZE);
        }
#else
        memcpy(&sc->fpstate->_xmm[i], &get_mcontext(dcontext)->ymm[i], XMM_REG_SIZE);
        if (YMM_ENABLED()) {
            /* i#637: ymm top halves are inside struct _xstate */
            memcpy(&xstate->ymmh.ymmh_space[i * 4], 
                   ((void *)&get_mcontext(dcontext)->ymm[i]) + XMM_REG_SIZE,
                   YMMH_REG_SIZE);
        }
#endif
    }
}

/* We can't tell whether the app has used fpstate yet so we preserve every time
 * (i#641 covers optimizing that)
 */
static void
save_fpstate(dcontext_t *dcontext, sigframe_rt_t *frame)
{
    /* FIXME: is there a better way to align this thing?
     * the __attribute__ on the struct above doesn't do it
     */
    char align[sizeof(union i387_union) + 16];
    union i387_union *temp = (union i387_union *)
        ( (((ptr_uint_t)align) + 16) & ((ptr_uint_t)-16) );
    struct sigcontext *sc = get_sigcontext_from_rt_frame(frame);
    LOG(THREAD, LOG_ASYNCH, 3, "save_fpstate\n");
    if (sc->fpstate == NULL) {
        /* Nothing to do: there was no fpstate to save at the time the kernel
         * gave us this frame.
         * It's possible that by the time we deliver the signal
         * there is some state: but it's up to the caller to set up room
         * for fpstate and point at it in that case.
         */
        return;
    } else {
        LOG(THREAD, LOG_ASYNCH, 3, "ptr="PFX"\n", sc->fpstate);
    }
    if (proc_has_feature(FEATURE_FXSR)) {
        LOG(THREAD, LOG_ASYNCH, 3, "\ttemp="PFX"\n", temp);
#ifdef X64
        /* this is "unlazy_fpu" */
        /* fxsaveq is only supported with gas >= 2.16 but we have that */
        asm volatile( "fxsaveq %0 ; fnclex"
                      : "=m" (temp->fxsave) );
        /* now convert into struct _fpstate form */
        ASSERT(sizeof(struct _fpstate) == sizeof(struct i387_fxsave_struct));
        memcpy(sc->fpstate, &temp->fxsave, sizeof(struct i387_fxsave_struct));
#else
        /* this is "unlazy_fpu" */
        asm volatile( "fxsave %0 ; fnclex"
                      : "=m" (temp->fxsave) );
        /* now convert into struct _fpstate form */
        convert_fxsave_to_fpstate(sc->fpstate, &temp->fxsave);
#endif
    } else {
        /* FIXME NYI: need to convert to fxsave format for sc->fpstate */
        IF_X64(ASSERT_NOT_IMPLEMENTED(false));
        /* this is "unlazy_fpu" */
        asm volatile( "fnsave %0 ; fwait"
                      : "=m" (temp->fsave) );
        /* now convert into struct _fpstate form */
        temp->fsave.status = temp->fsave.swd;
        memcpy(sc->fpstate, &temp->fsave, sizeof(struct i387_fsave_struct));
    }

    /* the app's xmm registers may be saved away in priv_mcontext_t, in which
     * case we need to copy those values instead of using what was in
     * the physical xmm registers.
     * because of this, we can't just execute "xsave".  we still need to
     * execute xgetbv though.  xsave is very expensive so not worth doing
     * when xgetbv is all we need; if in the future they add status words,
     * etc. we can't get any other way then we'll have to do it, but best
     * to avoid for now.
     */
    save_xmm(dcontext, frame);
}

/**** top-level routines ***********************************************/

static bool
os_itimers_thread_shared(void)
{
    static bool itimers_shared;
    static bool cached = false;
    if (!cached) {
        file_t f = os_open("/proc/version", OS_OPEN_READ);
        if (f != INVALID_FILE) {
            char buf[128];
            int major, minor, rel;
            os_read(f, buf, BUFFER_SIZE_ELEMENTS(buf));
            NULL_TERMINATE_BUFFER(buf);
            if (sscanf(buf, "%*s %*s %d.%d.%d", &major, &minor, &rel) == 3) {
                /* Linux NPTL in kernel 2.6.12+ has POSIX-style itimers shared
                 * among threads.
                 */
                LOG(GLOBAL, LOG_ASYNCH, 1, "kernel version = %d.%d.%d\n",
                    major, minor, rel);
                itimers_shared = ((major == 2 && minor >= 6 && rel >= 12) ||
                                  (major >= 3 /* linux-3.0 or above */));
                cached = true;
            }
            os_close(f);
        }
        if (!cached) {
            /* assume not shared */
            itimers_shared = false;
            cached = true;
        }
        LOG(GLOBAL, LOG_ASYNCH, 1, "itimers are %s\n",
            itimers_shared ? "thread-shared" : "thread-private");
    }
    return itimers_shared;
}

void
signal_init()
{
    IF_X64(ASSERT(ALIGNED(offsetof(sigpending_t, xstate), AVX_ALIGNMENT)));
    os_itimers_thread_shared();

    /* Set up a handler for safe_read (or other fault detection) during
     * DR init before thread is initialized.
     *
     * XXX: could set up a clone_record_t and pass to the initial
     * signal_thread_inherit() but that would require further code changes.
     * Could also call signal_thread_inherit to init this, but we don't want
     * to intercept timer signals, etc. before we're ready to handle them,
     * so we do a partial init.
     */
    signal_info_init_sigaction(GLOBAL_DCONTEXT, &init_info);
    intercept_signal(GLOBAL_DCONTEXT, &init_info, SIGSEGV);
    intercept_signal(GLOBAL_DCONTEXT, &init_info, SIGBUS);
    unblock_all_signals(&init_sigmask);

    signalfd_init();
}

void
signal_exit()
{
    signalfd_exit();
#ifdef DEBUG
    if (stats->loglevel > 0 && (stats->logmask & (LOG_ASYNCH|LOG_STATS)) != 0) {
        LOG(GLOBAL, LOG_ASYNCH|LOG_STATS, 1,
            "Total signals delivered: %d\n", GLOBAL_STAT(num_signals));
    }
#endif
}

void
signal_thread_init(dcontext_t *dcontext)
{
#ifdef HAVE_SIGALTSTACK
    int rc;
#endif
    thread_sig_info_t *info = HEAP_TYPE_ALLOC(dcontext, thread_sig_info_t,
                                              ACCT_OTHER, PROTECTED);

    /* all fields want to be initialized to 0 */
    memset(info, 0, sizeof(thread_sig_info_t));
    dcontext->signal_field = (void *) info;

    /* our special heap to avoid reentrancy problems
     * composed entirely of sigpending_t units
     * Note that it's fine to have the special heap do page-at-a-time
     * committing, which does not use locks (unless triggers reset!),
     * but if we need a new unit that will grab a lock: we try to
     * avoid that by limiting the # of pending alarm signals (PR 596768).
     */
    info->sigheap = special_heap_init(sizeof(sigpending_t),
                                      false /* cannot have any locking */,
                                      false /* -x */,
                                      true /* persistent */);

#ifdef HAVE_SIGALTSTACK
    /* set up alternate stack 
     * i#552 we may terminate the process without freeing the stack, so we
     * stack_alloc it to exempt from the memory leak check.
     */
    info->sigstack.ss_sp = (char *) stack_alloc(SIGSTACK_SIZE) - SIGSTACK_SIZE;
    info->sigstack.ss_size = SIGSTACK_SIZE;
    /* kernel will set xsp to sp+size to grow down from there, we don't have to */
    info->sigstack.ss_flags = SS_ONSTACK;
    rc = sigaltstack_syscall(&info->sigstack, &info->app_sigstack);
    ASSERT(rc == 0);
    LOG(THREAD, LOG_ASYNCH, 1, "signal stack is "PFX" - "PFX"\n",
        info->sigstack.ss_sp, info->sigstack.ss_sp + info->sigstack.ss_size);
    /* app_sigstack dealt with below, based on parentage */
#endif

    kernel_sigemptyset(&info->app_sigblocked);

    ASSIGN_INIT_LOCK_FREE(info->child_lock, child_lock);
    
    /* someone must call signal_thread_inherit() to finish initialization:
     * for first thread, called from initial setup; else, from new_thread_setup
     * or share_siginfo_after_take_over.
     */
}

/* i#27: create custom data to pass to the child of a clone
 * since we can't rely on being able to find the caller, or that
 * its syscall data is still valid, once in the child.
 *
 * i#149/ PR 403015: The clone record is passed to the new thread via the dstack
 * created for it.  Unlike before, where the child thread would create its own
 * dstack, now the parent thread creates the dstack.  Also, switches app stack
 * to dstack.
 */
void *
create_clone_record(dcontext_t *dcontext, reg_t *app_thread_xsp)
{
    byte *dstack = stack_alloc(DYNAMORIO_STACK_SIZE);
    LOG(THREAD, LOG_ASYNCH, 1,
        "create_clone_record: dstack for new thread is "PFX"\n", dstack);

    /* Note, the stack grows to low memory addr, so dstack points to the high
     * end of the allocated stack region.  So, we must subtract to get space for
     * the clone record.
     */
    clone_record_t *record = (clone_record_t *) (dstack - sizeof(clone_record_t));
    LOG(THREAD, LOG_ASYNCH, 1, "allocated clone record: "PFX"\n", record);

    record->dstack = dstack;
    record->app_thread_xsp = *app_thread_xsp;
    /* asynch_target is set in dispatch() prior to calling pre_system_call(). */
    record->continuation_pc = dcontext->asynch_target;
    record->caller_id = dcontext->owning_thread;
    record->clone_sysnum = dcontext->sys_num;
    record->clone_flags = dcontext->sys_param0;
    record->info = *((thread_sig_info_t *)dcontext->signal_field);
    record->parent_info = (thread_sig_info_t *) dcontext->signal_field;
    record->pcprofile_info = dcontext->pcprofile_field;
    LOG(THREAD, LOG_ASYNCH, 1,
        "create_clone_record: thread %d, pc "PFX"\n",
        record->caller_id, record->continuation_pc);

    /* Set the thread stack to point to the dstack, below the clone record.
     * Note: it's glibc who sets up the arg to the thread start function;
     * the kernel just does a fork + stack swap, so we can get away w/ our
     * own stack swap if we restore before the glibc asm code takes over.
     */
    /* i#754: set stack to be XSTATE aligned for saving YMM registers */
    ASSERT(ALIGNED(XSTATE_ALIGNMENT, REGPARM_END_ALIGN));
    *app_thread_xsp = ALIGN_BACKWARD(record, XSTATE_ALIGNMENT);
 
    return (void *) record;
}

/* This is to support dr_create_client_thread() */
void
set_clone_record_fields(void *record, reg_t app_thread_xsp, app_pc continuation_pc,
                        uint clone_sysnum, uint clone_flags)
{
    clone_record_t *rec = (clone_record_t *) record;
    ASSERT(rec != NULL);
    rec->app_thread_xsp = app_thread_xsp;
    rec->continuation_pc = continuation_pc;
    rec->clone_sysnum = clone_sysnum;
    rec->clone_flags = clone_flags;
}

/* i#149/PR 403015: The clone record is passed to the new thread by placing it
 * at the bottom of the dstack, i.e., the high memory.  So the new thread gets
 * it from the base of the dstack.  The dstack is then set as the app stack.
 *
 * CAUTION: don't use a lot of stack in this routine as it gets invoked on the
 *          dstack from new_thread_setup - this is because this routine assumes 
 *          no more than a page of dstack has been used so far since the clone 
 *          system call was done.
 */
void *
get_clone_record(reg_t xsp)
{
    clone_record_t *record; 
    byte *dstack_base;

    /* xsp should be in a dstack, i.e., dynamorio heap.  */
    ASSERT(is_dynamo_address((app_pc) xsp));

    /* The (size of the clone record +
     *      stack used by new_thread_start (only for setting up priv_mcontext_t) +
     *      stack used by new_thread_setup before calling get_clone_record())
     * is less than a page.  This is verified by the assert below.  If it does
     * exceed a page, it won't happen at random during runtime, but in a
     * predictable way during development, which will be caught by the assert.
     * The current usage is about 800 bytes for clone_record +
     * sizeof(priv_mcontext_t) + few words in new_thread_setup before
     * get_clone_record() is called.
     */
    dstack_base = (byte *) ALIGN_FORWARD(xsp, PAGE_SIZE);
    record = (clone_record_t *) (dstack_base - sizeof(clone_record_t));

    /* dstack_base and the dstack in the clone record should be the same. */
    ASSERT(dstack_base == record->dstack);
    return (void *) record;
}

/* i#149/PR 403015: App xsp is passed to the new thread via the clone record. */
reg_t
get_clone_record_app_xsp(void *record)
{
    ASSERT(record != NULL);
    return ((clone_record_t *) record)->app_thread_xsp;
}

byte *
get_clone_record_dstack(void *record)
{
    ASSERT(record != NULL);
    return ((clone_record_t *) record)->dstack;
}

/* Initializes info's app_sigaction, restorer_valid, and we_intercept fields */
static void
signal_info_init_sigaction(dcontext_t *dcontext, thread_sig_info_t *info)
{
    info->app_sigaction = (kernel_sigaction_t **)
        handler_alloc(dcontext, SIGARRAY_SIZE * sizeof(kernel_sigaction_t *));
    memset(info->app_sigaction, 0, SIGARRAY_SIZE * sizeof(kernel_sigaction_t *));
    memset(&info->restorer_valid, -1, SIGARRAY_SIZE * sizeof(info->restorer_valid[0]));
    info->we_intercept = (bool *) handler_alloc(dcontext, SIGARRAY_SIZE * sizeof(bool));
    memset(info->we_intercept, 0, SIGARRAY_SIZE * sizeof(bool));
}

/* Cleans up info's app_sigaction, restorer_valid, and we_intercept fields */
static void
signal_info_exit_sigaction(dcontext_t *dcontext, thread_sig_info_t *info,
                           bool other_thread)
{
    int i;
    kernel_sigaction_t act;
    memset(&act, 0, sizeof(act));
    act.handler = (handler_t) SIG_DFL;
    kernel_sigemptyset(&act.mask); /* does mask matter for SIG_DFL? */
    for (i = 1; i <= MAX_SIGNUM; i++) {
        if (!other_thread) {
            if (info->app_sigaction[i] != NULL) {
                /* restore to old handler, but not if exiting whole
                 * process: else may get itimer during cleanup, so we
                 * set to SIG_IGN (we'll have to fix once we impl detach)
                 */
                if (dynamo_exited) {
                    info->app_sigaction[i]->handler = (handler_t) SIG_IGN;
                    sigaction_syscall(i, info->app_sigaction[i], NULL);
                }
                LOG(THREAD, LOG_ASYNCH, 2, "\trestoring "PFX" as handler for %d\n",
                    info->app_sigaction[i]->handler, i);
                sigaction_syscall(i, info->app_sigaction[i], NULL);
            } else if (info->we_intercept[i]) {
                /* restore to default */
                LOG(THREAD, LOG_ASYNCH, 2, "\trestoring SIG_DFL as handler for %d\n", i);
                sigaction_syscall(i, &act, NULL);
            }
        }
        if (info->app_sigaction[i] != NULL) {
            handler_free(dcontext, info->app_sigaction[i],
                         sizeof(kernel_sigaction_t));
        }
    }
    handler_free(dcontext, info->app_sigaction,
                 SIGARRAY_SIZE * sizeof(kernel_sigaction_t *));
    handler_free(dcontext, info->we_intercept, SIGARRAY_SIZE * sizeof(bool));
}

/* Called once a new thread's dcontext is created.
 * Inherited and shared fields are set up here.
 * The clone_record contains the continuation pc, which is returned.
 */
app_pc
signal_thread_inherit(dcontext_t *dcontext, void *clone_record)
{
    app_pc res = NULL;
    clone_record_t *record = (clone_record_t *) clone_record;
    thread_sig_info_t *info = (thread_sig_info_t *) dcontext->signal_field;
    kernel_sigaction_t oldact;
    int i, rc;
    if (record != NULL) {
        app_pc continuation_pc = record->continuation_pc;
        LOG(THREAD, LOG_ASYNCH, 1,
            "continuation pc is "PFX"\n", continuation_pc);
        LOG(THREAD, LOG_ASYNCH, 1,
            "parent tid is %d, parent sysnum is %d(%s), clone flags="PIFX"\n", 
            record->caller_id, record->clone_sysnum,
            record->clone_sysnum == SYS_vfork ? "vfork" : 
            record->clone_sysnum == SYS_clone ? "clone" : "unexpected",
            record->clone_flags);
        if (record->clone_sysnum == SYS_vfork) {
            /* The above clone_flags argument is bogus.
               SYS_vfork doesn't have a free register to keep the hardcoded value
               see /usr/src/linux/arch/i386/kernel/process.c */
            /* CHECK: is this the only place real clone flags are needed? */
            record->clone_flags = CLONE_VFORK | CLONE_VM | SIGCHLD;
        }

        /* handlers are either inherited or shared */
        if (TEST(CLONE_SIGHAND, record->clone_flags)) {
            /* need to share table of handlers! */
            LOG(THREAD, LOG_ASYNCH, 2, "sharing signal handlers with parent\n");
            info->shared_app_sigaction = true;
            info->shared_refcount = record->info.shared_refcount;
            info->shared_lock = record->info.shared_lock;
            info->app_sigaction = record->info.app_sigaction;
            info->we_intercept = record->info.we_intercept;
            mutex_lock(info->shared_lock);
            (*info->shared_refcount)++;
#ifdef DEBUG
            for (i = 1; i <= MAX_SIGNUM; i++) {
                if (info->app_sigaction[i] != NULL) {
                    LOG(THREAD, LOG_ASYNCH, 2, "\thandler for signal %d is "PFX"\n",
                        i, info->app_sigaction[i]->handler);
                }
            }
#endif
            mutex_unlock(info->shared_lock);
        } else {
            /* copy handlers */
            LOG(THREAD, LOG_ASYNCH, 2, "inheriting signal handlers from parent\n");
            info->app_sigaction = (kernel_sigaction_t **)
                handler_alloc(dcontext, SIGARRAY_SIZE * sizeof(kernel_sigaction_t *));
            memset(info->app_sigaction, 0, SIGARRAY_SIZE * sizeof(kernel_sigaction_t *));
            for (i = 1; i <= MAX_SIGNUM; i++) {
                info->restorer_valid[i] = -1;  /* clear cache */
                if (record->info.app_sigaction[i] != NULL) {
                    info->app_sigaction[i] = (kernel_sigaction_t *)
                        handler_alloc(dcontext, sizeof(kernel_sigaction_t));
                    memcpy(info->app_sigaction[i], record->info.app_sigaction[i],
                           sizeof(kernel_sigaction_t));
                    LOG(THREAD, LOG_ASYNCH, 2, "\thandler for signal %d is "PFX"\n",
                        i, info->app_sigaction[i]->handler);
                }
            }
            info->we_intercept = (bool *)
                handler_alloc(dcontext, SIGARRAY_SIZE * sizeof(bool));
            memcpy(info->we_intercept, record->info.we_intercept,
                   SIGARRAY_SIZE * sizeof(bool));
            mutex_lock(&record->info.child_lock);
            record->info.num_unstarted_children--;
            mutex_unlock(&record->info.child_lock);
            /* this should be safe since parent should wait for us */
            mutex_lock(&record->parent_info->child_lock);
            record->parent_info->num_unstarted_children--;
            mutex_unlock(&record->parent_info->child_lock);
        }

        /* itimers are either private or shared */
        if (TEST(CLONE_THREAD, record->clone_flags) && os_itimers_thread_shared()) {
            ASSERT(record->info.shared_itimer);
            LOG(THREAD, LOG_ASYNCH, 2, "sharing itimers with parent\n");
            info->shared_itimer = true;
            info->shared_itimer_refcount = record->info.shared_itimer_refcount;
            info->shared_itimer_underDR = record->info.shared_itimer_underDR;
            info->shared_itimer_lock = record->info.shared_itimer_lock;
            info->itimer = record->info.itimer;
            acquire_recursive_lock(info->shared_itimer_lock);
            (*info->shared_itimer_refcount)++;
            release_recursive_lock(info->shared_itimer_lock);
            /* shared_itimer_underDR will be incremented in start_itimer() */
        } else {
            info->shared_itimer = false;
            init_itimer(dcontext, false/*!first thread*/);
        }

        if (APP_HAS_SIGSTACK(info)) {
            /* parent was under our control, so the real sigstack we see is just
             * the parent's being inherited -- clear it now
             */
            memset(&info->app_sigstack, 0, sizeof(stack_t));
            info->app_sigstack.ss_flags |= SS_DISABLE;
        }

        /* rest of state is never shared.
         * app_sigstack should already be in place, when we set up our sigstack
         * we asked for old sigstack.
         * FIXME: are current pending or blocked inherited?
         */
        res = continuation_pc;
    } else {
        /* initialize in isolation */
        if (!dynamo_initialized) {
            /* Undo the early-init handler */
            signal_info_exit_sigaction(GLOBAL_DCONTEXT, &init_info,
                                       false/*!other_thread*/);
            /* Undo the unblock-all */
            sigprocmask_syscall(SIG_SETMASK, &init_sigmask, NULL, sizeof(init_sigmask));
            DOLOG(2, LOG_ASYNCH, {
                LOG(THREAD, LOG_ASYNCH, 2, "initial app signal mask:\n");
                dump_sigset(dcontext, &init_sigmask);
            });
        }

        if (APP_HAS_SIGSTACK(info)) {
            /* parent was NOT under our control, so the real sigstack we see is
             * a real sigstack that was present before we took control
             */
            LOG(THREAD, LOG_ASYNCH, 1, "app already has signal stack "PFX" - "PFX"\n",
                info->app_sigstack.ss_sp,
                info->app_sigstack.ss_sp + info->app_sigstack.ss_size);
        }

        signal_info_init_sigaction(dcontext, info);
                
        info->shared_itimer = false; /* we'll set to true if a child is created */
        init_itimer(dcontext, true/*first*/);

        if (DYNAMO_OPTION(intercept_all_signals)) {
            /* PR 304708: to support client signal handlers without
             * the complexity of per-thread and per-signal callbacks
             * we always intercept all signals.  We also check here
             * for handlers the app registered before our init.
             */
            for (i=1; i<=MAX_SIGNUM; i++) {
                /* cannot intercept KILL or STOP */
                if (i != SIGKILL && i != SIGSTOP &&
                    /* FIXME PR 297033: we don't support intercepting DEFAULT_STOP /
                     * DEFAULT_CONTINUE signals.  Once add support, update
                     * dr_register_signal_event() comments.
                     */
                    default_action[i] != DEFAULT_STOP &&
                    default_action[i] != DEFAULT_CONTINUE)
                    intercept_signal(dcontext, info, i);
            }
        } else {
            /* we intercept the following signals ourselves: */
            intercept_signal(dcontext, info, SIGSEGV);
            /* PR 313665: look for DR crashes on unaligned memory or mmap bounds */
            intercept_signal(dcontext, info, SIGBUS);
            /* PR 212090: the signal we use to suspend threads */
            intercept_signal(dcontext, info, SUSPEND_SIGNAL);
#ifdef PAPI
            /* use SIGPROF for updating gui so it can be distinguished from SIGVTALRM */
            intercept_signal(dcontext, info, SIGPROF);
#endif
            /* vtalarm only used with pc profiling.  it interferes w/ PAPI
             * so arm this signal only if necessary
             */
            if (INTERNAL_OPTION(profile_pcs)) {
                intercept_signal(dcontext, info, SIGVTALRM);
            }
#ifdef CLIENT_INTERFACE
            intercept_signal(dcontext, info, SIGALRM);
#endif
#ifdef SIDELINE
            intercept_signal(dcontext, info, SIGCHLD);
#endif
            /* i#61/PR 211530: the signal we use for nudges */
            intercept_signal(dcontext, info, NUDGESIG_SIGNUM);
        
            /* process any handlers app registered before our init */
            for (i=1; i<=MAX_SIGNUM; i++) {
                if (info->we_intercept[i]) {
                    /* intercept_signal already stored pre-existing handler */
                    continue;
                }
                rc = sigaction_syscall(i, NULL, &oldact);
                ASSERT(rc == 0
                       /* Workaround for PR 223720, which was fixed in ESX4.0 but
                        * is present in ESX3.5 and earlier: vmkernel treats
                        * 63 and 64 as invalid signal numbers.
                        */
                       IF_VMX86(|| (i >= 63 && rc == -EINVAL))
                       );
                if (rc == 0 &&
                    oldact.handler != (handler_t) SIG_DFL &&
                    oldact.handler != (handler_t) master_signal_handler) {
                    /* could be master_ if inherited */
                    /* FIXME: if app removes handler, we'll never remove ours */
                    intercept_signal(dcontext, info, i);
                    info->we_intercept[i] = false;
                }
            }
        }

        /* should be 1st thread */
        if (get_num_threads() > 1)
            ASSERT_NOT_REACHED();
        /* FIXME: any way to recover if not 1st thread? */
        res = NULL;
    }

    unblock_all_signals(&info->app_sigblocked);
    DOLOG(2, LOG_ASYNCH, {
        LOG(THREAD, LOG_ASYNCH, 2, "thread's initial app signal mask:\n");
        dump_sigset(dcontext, &info->app_sigblocked);
    });

    /* only when SIGVTALRM handler is in place should we start itimer (PR 537743) */
    if (INTERNAL_OPTION(profile_pcs)) {
        /* even if the parent thread exits, we can use a pointer to its
         * pcprofile_info b/c when shared it's process-shared and is not freed
         * until the entire process exits
         */
        pcprofile_thread_init(dcontext, info->shared_itimer,
                              (record == NULL) ? NULL : record->pcprofile_info);
    }

    /* Assumed to be async safe. */
    info->fully_initialized = true;

    return res;
}

/* When taking over existing app threads, we assume they're using pthreads and
 * expect to share signal handlers, memory, thread group id, etc.
 */
void
share_siginfo_after_take_over(dcontext_t *dcontext, dcontext_t *takeover_dc)
{
    clone_record_t crec;
    thread_sig_info_t *parent_siginfo =
        (thread_sig_info_t*)takeover_dc->signal_field;
    /* Create a fake clone record with the given siginfo.  All threads in the
     * same thread group must share signal handlers since Linux 2.5.35, but we
     * have to guess at the other flags.
     * FIXME i#764: If we take over non-pthreads threads, we'll need some way to
     * tell if they're sharing signal handlers or not.
     */
    crec.caller_id = takeover_dc->owning_thread;
    crec.clone_sysnum = SYS_clone;
    crec.clone_flags = PTHREAD_CLONE_FLAGS;
    crec.parent_info = parent_siginfo;
    crec.info = *parent_siginfo;
    signal_thread_inherit(dcontext, &crec);
}

/* This is split from os_fork_init() so the new logfiles are available
 * (xref i#189/PR 452168).  It had to be after dynamo_other_thread_exit()
 * called in dynamorio_fork_init() after os_fork_init() else we clean
 * up data structs used in signal_thread_exit().
 */
void
signal_fork_init(dcontext_t *dcontext)
{
    thread_sig_info_t *info = (thread_sig_info_t *) dcontext->signal_field;
    int i;
    /* Child of fork is a single thread in a new process so should
     * start over w/ no sharing (xref i#190/PR 452178) 
     */
    if (info->shared_app_sigaction) {
        info->shared_app_sigaction = false;
        if (info->shared_lock != NULL) {
            DELETE_LOCK(*info->shared_lock);
            global_heap_free(info->shared_lock, sizeof(mutex_t) HEAPACCT(ACCT_OTHER));
        }
        if (info->shared_refcount != NULL)
            global_heap_free(info->shared_refcount, sizeof(int) HEAPACCT(ACCT_OTHER));
        info->shared_lock = NULL;
        info->shared_refcount = NULL;
    }
    if (info->shared_itimer) {
        /* itimers are not inherited across fork */
        info->shared_itimer = false;
        if (os_itimers_thread_shared())
            global_heap_free(info->itimer, sizeof(*info->itimer) HEAPACCT(ACCT_OTHER));
        else
            heap_free(dcontext, info->itimer, sizeof(*info->itimer) HEAPACCT(ACCT_OTHER));
        info->itimer = NULL;  /* reset by init_itimer */
        ASSERT(info->shared_itimer_lock != NULL);
        DELETE_RECURSIVE_LOCK(*info->shared_itimer_lock);
        global_heap_free(info->shared_itimer_lock, sizeof(*info->shared_itimer_lock)
                         HEAPACCT(ACCT_OTHER));
        info->shared_itimer_lock = NULL;
        ASSERT(info->shared_itimer_refcount != NULL);
        global_heap_free(info->shared_itimer_refcount, sizeof(int) HEAPACCT(ACCT_OTHER));
        info->shared_itimer_refcount = NULL;
        ASSERT(info->shared_itimer_underDR != NULL);
        global_heap_free(info->shared_itimer_underDR, sizeof(int) HEAPACCT(ACCT_OTHER));
        info->shared_itimer_underDR = NULL;
        init_itimer(dcontext, true/*first*/);
    }
    info->num_unstarted_children = 0;
    for (i = 1; i <= MAX_SIGNUM; i++) {
        /* "A child created via fork(2) initially has an empty pending signal set" */
        dcontext->signals_pending = false;
        while (info->sigpending[i] != NULL) {
            sigpending_t *temp = info->sigpending[i];
            info->sigpending[i] = temp->next;
            special_heap_free(info->sigheap, temp);
        }
    }
    if (INTERNAL_OPTION(profile_pcs)) {
        pcprofile_fork_init(dcontext);
    }

    /* Assumed to be async safe. */
    info->fully_initialized = true;
}

#ifdef DEBUG
static bool
sigsegv_handler_is_ours(void)
{
    int rc;
    kernel_sigaction_t oldact;
    rc = sigaction_syscall(SIGSEGV, NULL, &oldact);
    return (rc == 0 && oldact.handler == (handler_t)master_signal_handler);
}
#endif /* DEBUG */

void
signal_thread_exit(dcontext_t *dcontext, bool other_thread)
{
    thread_sig_info_t *info = (thread_sig_info_t *) dcontext->signal_field;
    int i;

    /* i#1012: DR's signal handler should always be installed before this point.
     */
    ASSERT(sigsegv_handler_is_ours() || removed_sig_handler);

    while (info->num_unstarted_children > 0) {
        /* must wait for children to start and copy our state
         * before we destroy it!
         */
        thread_yield();
    }

    if (dynamo_exited) {
        /* stop itimers before removing signal handlers */
        for (i = 0; i < NUM_ITIMERS; i++)
            set_actual_itimer(dcontext, i, info, false/*disable*/);
    }

    /* FIXME: w/ shared handlers, if parent (the owner here) dies,
     * can children keep living w/ a copy of the handlers?
     */
    if (info->shared_app_sigaction) {
        mutex_lock(info->shared_lock);
        (*info->shared_refcount)--;
        mutex_unlock(info->shared_lock);
    }
    if (!info->shared_app_sigaction || *info->shared_refcount == 0) {
        LOG(THREAD, LOG_ASYNCH, 2, "signal handler cleanup:\n");
        signal_info_exit_sigaction(dcontext, info, other_thread);
        if (info->shared_lock != NULL) {
            DELETE_LOCK(*info->shared_lock);
            global_heap_free(info->shared_lock, sizeof(mutex_t) HEAPACCT(ACCT_OTHER));
        }
        if (info->shared_refcount != NULL)
            global_heap_free(info->shared_refcount, sizeof(int) HEAPACCT(ACCT_OTHER));
    }

    if (info->shared_itimer) {
        acquire_recursive_lock(info->shared_itimer_lock);
        (*info->shared_itimer_refcount)--;
        release_recursive_lock(info->shared_itimer_lock);
    }
    if (!info->shared_itimer || *info->shared_itimer_refcount == 0) {
        if (INTERNAL_OPTION(profile_pcs)) {
            /* no cleanup needed for non-final thread in group */
            pcprofile_thread_exit(dcontext);
        }
        if (os_itimers_thread_shared())
            global_heap_free(info->itimer, sizeof(*info->itimer) HEAPACCT(ACCT_OTHER));
        else
            heap_free(dcontext, info->itimer, sizeof(*info->itimer) HEAPACCT(ACCT_OTHER));
        if (info->shared_itimer_lock != NULL) {
            DELETE_RECURSIVE_LOCK(*info->shared_itimer_lock);
            global_heap_free(info->shared_itimer_lock, sizeof(recursive_lock_t)
                             HEAPACCT(ACCT_OTHER));
            ASSERT(info->shared_itimer_refcount != NULL);
            global_heap_free(info->shared_itimer_refcount, sizeof(int)
                             HEAPACCT(ACCT_OTHER));
            ASSERT(info->shared_itimer_underDR != NULL);
            global_heap_free(info->shared_itimer_underDR, sizeof(int)
                             HEAPACCT(ACCT_OTHER));
        }
    }
    for (i = 1; i <= MAX_SIGNUM; i++) {
        /* pending queue is per-thread and not shared */
        while (info->sigpending[i] != NULL) {
            sigpending_t *temp = info->sigpending[i];
            info->sigpending[i] = temp->next;
            special_heap_free(info->sigheap, temp);
        }
    }
#ifdef HAVE_SIGALTSTACK
    /* Remove our sigstack and restore the app sigstack if it had one.  */
    LOG(THREAD, LOG_ASYNCH, 2, "removing our signal stack "PFX" - "PFX"\n",
        info->sigstack.ss_sp, info->sigstack.ss_sp + info->sigstack.ss_size);
    if (APP_HAS_SIGSTACK(info)) {
        LOG(THREAD, LOG_ASYNCH, 2, "restoring app signal stack "PFX" - "PFX"\n",
            info->app_sigstack.ss_sp,
            info->app_sigstack.ss_sp + info->app_sigstack.ss_size);
    } else {
        ASSERT(TEST(SS_DISABLE, info->app_sigstack.ss_flags));
    }
    if (info->sigstack.ss_sp != NULL) {
        /* i#552: to raise client exit event, we may call dynamo_process_exit
         * on sigstack in signal handler.
         * In that case we set sigstack (ss_sp) NULL to avoid stack swap.
         */
        i = sigaltstack_syscall(&info->app_sigstack, NULL);
        ASSERT(i == 0);
    }
#endif
    signalfd_thread_exit(dcontext, info);
    special_heap_exit(info->sigheap);
    DELETE_LOCK(info->child_lock);
#ifdef DEBUG
    /* for non-debug we do fast exit path and don't free local heap */
# ifdef HAVE_SIGALTSTACK
    if (info->sigstack.ss_sp != NULL) {
        /* i#552: to raise client exit event, we may call dynamo_process_exit
         * on sigstack in signal handler.
         * In that case we set sigstack (ss_sp) NULL to avoid stack free.
         */
        stack_free(info->sigstack.ss_sp + info->sigstack.ss_size,
                   info->sigstack.ss_size);
    }
# endif
    HEAP_TYPE_FREE(dcontext, info, thread_sig_info_t, ACCT_OTHER, PROTECTED);
#endif
#ifdef PAPI
    /* use SIGPROF for updating gui so it can be distinguished from SIGVTALRM */
    set_itimer_callback(dcontext, ITIMER_PROF, 500,
                        (void (*func)(dcontext_t *, priv_mcontext_t *))
                        perfctr_update_gui());
#endif
}

static void
set_our_handler_sigact(kernel_sigaction_t *act, int sig)
{
    act->handler = (handler_t) master_signal_handler;

    act->flags = SA_SIGINFO; /* send 3 args to handler */
#ifdef HAVE_SIGALTSTACK
    act->flags |= SA_ONSTACK; /* use our sigstack */
#endif

#if defined(X64) && !defined(VMX86_SERVER)
    /* PR 305020: must have SA_RESTORER for x64 */
    act->flags |= SA_RESTORER;
    act->restorer = (void (*)(void)) dynamorio_sigreturn;
#endif

    /* We block most signals within our handler */
    kernel_sigfillset(&act->mask);
    /* i#184/PR 450670: we let our suspend signal interrupt our own handler
     * We never send more than one before resuming, so no danger to stack usage
     * from our own: but app could pile them up.
     */
    kernel_sigdelset(&act->mask, SUSPEND_SIGNAL);
    /* i#193/PR 287309: we need to NOT suppress further SIGSEGV, for decode faults,
     * for try/except, and for !HAVE_PROC_MAPS probes.
     * Just like SUSPEND_SIGNAL, if app sends repeated SEGV, could run out of 
     * alt stack: seems too corner-case to be worth increasing stack size.
     */
    kernel_sigdelset(&act->mask, SIGSEGV);
    if (sig == SUSPEND_SIGNAL || sig == SIGSEGV)
        act->flags |= SA_NODEFER;
    /* Sigset is a 1 or 2 elt array of longs on X64/X86.  Treat as 2 elt of
     * uint32. */
    IF_DEBUG(uint32 *mask_sig = (uint32*)&act->mask.sig[0]);
    LOG(THREAD_GET, LOG_ASYNCH, 3,
        "mask for our handler is "PFX" "PFX"\n", mask_sig[0], mask_sig[1]);
}

/* Set up master_signal_handler as the handler for signal "sig",
 * for the current thread.  Since we deal with kernel data structures
 * in our interception of system calls, we use them here as well,
 * to avoid having to translate to/from libc data structures.
 */
static void
intercept_signal(dcontext_t *dcontext, thread_sig_info_t *info, int sig)
{
    int rc;
    kernel_sigaction_t act;
    kernel_sigaction_t oldact;
    ASSERT(sig <= MAX_SIGNUM);

    set_our_handler_sigact(&act, sig);
    /* arm the signal */
    rc = sigaction_syscall(sig, &act, &oldact);
    ASSERT(rc == 0
           /* Workaround for PR 223720, which was fixed in ESX4.0 but
            * is present in ESX3.5 and earlier: vmkernel treats
            * 63 and 64 as invalid signal numbers.
            */
           IF_VMX86(|| (sig >= 63 && rc == -EINVAL))
           );
    if (rc != 0) /* be defensive: app will probably still work */
        return;

    if (oldact.handler != (handler_t) SIG_DFL &&
        oldact.handler != (handler_t) master_signal_handler) {
        /* save the app's action for sig */
        if (info->shared_app_sigaction) {
            /* app_sigaction structure is shared */
            mutex_lock(info->shared_lock);
        }
        if (info->app_sigaction[sig] != NULL) {
            /* go ahead and toss the old one, it's up to the app to store
             * and then restore later if it wants to
             */
            handler_free(dcontext, info->app_sigaction[sig], sizeof(kernel_sigaction_t));
        }
        info->app_sigaction[sig] = (kernel_sigaction_t *)
            handler_alloc(dcontext, sizeof(kernel_sigaction_t));
        memcpy(info->app_sigaction[sig], &oldact, sizeof(kernel_sigaction_t));
        /* clear cache */
        info->restorer_valid[sig] = -1;
        if (info->shared_app_sigaction)
            mutex_unlock(info->shared_lock);
#ifdef DEBUG
        if (oldact.handler == (handler_t) SIG_IGN) {
            LOG(THREAD, LOG_ASYNCH, 2,
                "app already installed SIG_IGN as sigaction for signal %d\n", sig);
        } else {
            LOG(THREAD, LOG_ASYNCH, 2,
                "app already installed "PFX" as sigaction for signal %d\n",
                oldact.handler, sig);
        }
#endif
    }
    
    LOG(THREAD, LOG_ASYNCH, 3, "\twe intercept signal %d\n", sig);
    info->we_intercept[sig] = true;
}

/**** system call handlers ***********************************************/

/* FIXME: invalid pointer passed to kernel will currently show up
 * probably as a segfault in our handlers below...need to make them
 * look like kernel, and pass error code back to os.c
 */

void
handle_clone(dcontext_t *dcontext, uint flags)
{
    thread_sig_info_t *info = (thread_sig_info_t *) dcontext->signal_field;
    if ((flags & CLONE_VM) == 0) {
        /* separate process not sharing memory */
        if ((flags & CLONE_SIGHAND) != 0) {
            /* FIXME: how deal with this?
             * "man clone" says: "Since Linux 2.6.0-test6, flags must also 
             * include CLONE_VM if CLONE_SIGHAND is specified"
             */
            LOG(THREAD, LOG_ASYNCH, 1, "WARNING: !CLONE_VM but CLONE_SIGHAND!\n");
            ASSERT_NOT_IMPLEMENTED(false);
        }
        return;
    }

    pre_second_thread();

    if ((flags & CLONE_SIGHAND) != 0) {
        /* need to share table of handlers! */
        LOG(THREAD, LOG_ASYNCH, 2, "handle_clone: CLONE_SIGHAND set!\n");
        if (!info->shared_app_sigaction) {
            /* this is the start of a chain of sharing
             * no synch needed here, child not created yet
             */
            info->shared_app_sigaction = true;
            info->shared_refcount = (int *) global_heap_alloc(sizeof(int)
                                                              HEAPACCT(ACCT_OTHER));
            *info->shared_refcount = 1;
            info->shared_lock = (mutex_t *) global_heap_alloc(sizeof(mutex_t)
                                                            HEAPACCT(ACCT_OTHER));
            ASSIGN_INIT_LOCK_FREE(*info->shared_lock, shared_lock);
        } /* else, some ancestor is already owner */
   } else {
        /* child will inherit copy of current table -> cannot modify it
         * until child is scheduled!  FIXME: any other way?
         */
        mutex_lock(&info->child_lock);
        info->num_unstarted_children++;
        mutex_unlock(&info->child_lock);
    }

    if (TEST(CLONE_THREAD, flags) && os_itimers_thread_shared()) {
        if (!info->shared_itimer) {
            /* this is the start of a chain of sharing
             * no synch needed here, child not created yet
             */
            info->shared_itimer = true;
            info->shared_itimer_refcount = (int *)
                global_heap_alloc(sizeof(int) HEAPACCT(ACCT_OTHER));
            *info->shared_itimer_refcount = 1;
            info->shared_itimer_underDR = (int *)
                global_heap_alloc(sizeof(int) HEAPACCT(ACCT_OTHER));
            *info->shared_itimer_underDR = 1;
            info->shared_itimer_lock = (recursive_lock_t *)
                global_heap_alloc(sizeof(*info->shared_itimer_lock) HEAPACCT(ACCT_OTHER));
            ASSIGN_INIT_RECURSIVE_LOCK_FREE(*info->shared_itimer_lock, shared_itimer_lock);
        } /* else, some ancestor already created */
    }
}

/* Returns false if should NOT issue syscall.
 */
bool
handle_sigaction(dcontext_t *dcontext, int sig, const kernel_sigaction_t *act,
                 kernel_sigaction_t *oact, size_t sigsetsize)
{
    thread_sig_info_t *info = (thread_sig_info_t *) dcontext->signal_field;
    kernel_sigaction_t *save;
    kernel_sigaction_t *non_const_act = (kernel_sigaction_t *) act;
    /* i#1035: app may pass invalid signum to find MAX_SIGNUM */
    if (sig <= MAX_SIGNUM && act != NULL) {
        /* app is installing a new action */
        
        while (info->num_unstarted_children > 0) {
            /* must wait for children to start and copy our state
             * before we modify it!
             */
            thread_yield();
        }

        if (info->shared_app_sigaction) {
            /* app_sigaction structure is shared */
            mutex_lock(info->shared_lock);
        }

        if (act->handler == (handler_t) SIG_IGN ||
            act->handler == (handler_t) SIG_DFL) {
            LOG(THREAD, LOG_ASYNCH, 2,
                "app installed %s as sigaction for signal %d\n",
                (act->handler == (handler_t) SIG_IGN) ? "SIG_IGN" : "SIG_DFL", sig);
            if (!info->we_intercept[sig]) {
                /* let the SIG_IGN/SIG_DFL go through, we want to remove our
                 * handler.  we delete the stored app_sigaction in post_
                 */
                if (info->shared_app_sigaction)
                    mutex_unlock(info->shared_lock);
                return true;
            }
        } else {
            LOG(THREAD, LOG_ASYNCH, 2,
                "app installed "PFX" as sigaction for signal %d\n",
                act->handler, sig);
            DOLOG(2, LOG_ASYNCH, {
                LOG(THREAD, LOG_ASYNCH, 2, "signal mask for handler:\n");
                dump_sigset(dcontext, (kernel_sigset_t *) &act->mask);
            });
        }

        /* save app's entire sigaction struct */
        save = (kernel_sigaction_t *) handler_alloc(dcontext, sizeof(kernel_sigaction_t));
        memcpy(save, act, sizeof(kernel_sigaction_t));
        if (info->app_sigaction[sig] != NULL) {
            /* go ahead and toss the old one, it's up to the app to store
             * and then restore later if it wants to
             */
            handler_free(dcontext, info->app_sigaction[sig], sizeof(kernel_sigaction_t));
        }
        info->app_sigaction[sig] = save;
        LOG(THREAD, LOG_ASYNCH, 3, "\tflags = "PFX", restorer = "PFX"\n",
            act->flags, act->restorer);
        /* clear cache */
        info->restorer_valid[sig] = -1;
        if (info->shared_app_sigaction)
            mutex_unlock(info->shared_lock);

        if (info->we_intercept[sig]) {
            /* cancel the syscall */
            return false;
        }
        /* now hand kernel our master handler instead of app's 
         * FIXME: double-check we're dealing w/ all possible mask, flag
         * differences between app & our handler
         */
        set_our_handler_sigact(non_const_act, sig);

        /* FIXME PR 297033: we don't support intercepting DEFAULT_STOP /
         * DEFAULT_CONTINUE signals b/c we can't generate the default
         * action: if the app registers a handler, though, we should work
         * properly if we never see SIG_DFL.
         */
    }

    /* oact is handled post-syscall */

    return true;
}

/* os.c thinks it's passing us struct_sigaction, really it's kernel_sigaction_t,
 * which has fields in different order.
 */
void
handle_post_sigaction(dcontext_t *dcontext, int sig, const kernel_sigaction_t *act,
                      kernel_sigaction_t *oact, size_t sigsetsize)
{
    thread_sig_info_t *info = (thread_sig_info_t *) dcontext->signal_field;
    /* this is only called on success, so sig must be in the valid range */
    ASSERT(sig <= MAX_SIGNUM && sig > 0);
    if (oact != NULL) {
        /* FIXME: hold lock across the syscall?!?
         * else could be modified and get wrong old action?
         */
        /* FIXME: make sure oact is readable & writable before accessing! */
        if (info->shared_app_sigaction)
            mutex_lock(info->shared_lock);
        if (info->app_sigaction[sig] == NULL) {
            if (info->we_intercept[sig]) {
                /* need to pretend there is no handler */
                memset(oact, 0, sizeof(*oact));
                oact->handler = (handler_t) SIG_DFL;
            } else {
                ASSERT(oact->handler == (handler_t) SIG_IGN ||
                       oact->handler == (handler_t) SIG_DFL);
            }
        } else {
            memcpy(oact, info->app_sigaction[sig], sizeof(kernel_sigaction_t));

            /* if installing IGN or DFL, delete ours */
            if (act && ((act->handler == (handler_t) SIG_IGN ||
                         act->handler == (handler_t) SIG_DFL) &&
                        !info->we_intercept[sig])) {
                /* remove old stored app action */
                handler_free(dcontext, info->app_sigaction[sig],
                             sizeof(kernel_sigaction_t));
                info->app_sigaction[sig] = NULL;
            }
        }
        if (info->shared_app_sigaction)
            mutex_unlock(info->shared_lock);
    }
}

/* Returns false if should NOT issue syscall */
bool
handle_sigaltstack(dcontext_t *dcontext, const stack_t *stack,
                   stack_t *old_stack)
{
    thread_sig_info_t *info = (thread_sig_info_t *) dcontext->signal_field;
    if (old_stack != NULL) {
        *old_stack = info->app_sigstack;
    }
    if (stack != NULL) {
        info->app_sigstack = *stack;
        LOG(THREAD, LOG_ASYNCH, 2, "app set up signal stack "PFX" - "PFX" %s\n",
            stack->ss_sp, stack->ss_sp + stack->ss_size - 1,
            (APP_HAS_SIGSTACK(info)) ? "enabled" : "disabled");
        return false; /* always cancel syscall */
    }
    return true;
}

/* Blocked signals:
 * In general, we don't need to keep track of blocked signals.
 * We only need to do so for those signals we intercept ourselves.
 * Thus, info->app_sigblocked ONLY contains entries for signals
 * we intercept ourselves.
 * PR 304708: we now intercept all signals.
 */

static void
set_blocked(dcontext_t *dcontext, kernel_sigset_t *set, bool absolute)
{
    thread_sig_info_t *info = (thread_sig_info_t *) dcontext->signal_field;
    int i;
    if (absolute) {
        /* discard current blocked signals, re-set from new mask */
        kernel_sigemptyset(&info->app_sigblocked);
    } /* else, OR in the new set */
    for (i=1; i<=MAX_SIGNUM; i++) {
        if (EMULATE_SIGMASK(info, i) && kernel_sigismember(set, i)) {
            kernel_sigaddset(&info->app_sigblocked, i);
        }
    }
#ifdef DEBUG
    if (stats->loglevel >= 3 && (stats->logmask & LOG_ASYNCH) != 0) {
        LOG(THREAD, LOG_ASYNCH, 3, "blocked signals are now:\n");
        dump_sigset(dcontext, &info->app_sigblocked);
    }
#endif
}

/* Scans over info->sigpending to see if there are any unblocked, pending
 * signals, and sets dcontext->signals_pending if there are.  Do this after
 * modifying the set of signals blocked by the application.
 */
static void
check_signals_pending(dcontext_t *dcontext, thread_sig_info_t *info)
{
    int i;

    if (dcontext->signals_pending)
        return;

    for (i=1; i<=MAX_SIGNUM; i++) {
        if (info->sigpending[i] != NULL &&
            !kernel_sigismember(&info->app_sigblocked, i)) {
            /* We only update the application's set of blocked signals from
             * syscall handlers, so we know we'll go back to dispatch and see
             * this flag right away.
             */
            dcontext->signals_pending = true;
            break;
        }
    }
}

/* Returns whether to execute the syscall */
bool
handle_sigprocmask(dcontext_t *dcontext, int how, kernel_sigset_t *app_set,
                   kernel_sigset_t *oset, size_t sigsetsize)
{
    thread_sig_info_t *info = (thread_sig_info_t *) dcontext->signal_field;
    int i;
    kernel_sigset_t safe_set;
    /* If we're intercepting all, we emulate the whole thing */
    bool execute_syscall = !DYNAMO_OPTION(intercept_all_signals);
    LOG(THREAD, LOG_ASYNCH, 2, "handle_sigprocmask\n");
    if (oset != NULL)
        info->pre_syscall_app_sigblocked = info->app_sigblocked;
    if (app_set != NULL && safe_read(app_set, sizeof(safe_set), &safe_set)) {
        if (execute_syscall) {
            /* The syscall will execute, so remove from the set passed
             * to it.   We restore post-syscall.
             * XXX i#1187: we could crash here touching app memory -- could
             * use TRY, but the app could pass read-only memory and it
             * would work natively!  Better to swap in our own
             * allocated data struct.  There's a transparency issue w/
             * races too if another thread looks at this memory.  This
             * won't happen by default b/c -intercept_all_signals is
             * on by default so we don't try to solve all these
             * issues.
             */
            info->pre_syscall_app_sigprocmask = safe_set;
        }
        if (how == SIG_BLOCK) {
            /* The set of blocked signals is the union of the current
             * set and the set argument.
             */
            for (i=1; i<=MAX_SIGNUM; i++) {
                if (EMULATE_SIGMASK(info, i) && kernel_sigismember(&safe_set, i)) {
                    kernel_sigaddset(&info->app_sigblocked, i);
                    if (execute_syscall)
                        kernel_sigdelset(app_set, i);
                }
            }
        } else if (how == SIG_UNBLOCK) {
            /* The signals in set are removed from the current set of
             *  blocked signals.
             */
            for (i=1; i<=MAX_SIGNUM; i++) {
                if (EMULATE_SIGMASK(info, i) && kernel_sigismember(&safe_set, i)) {
                    kernel_sigdelset(&info->app_sigblocked, i);
                    if (execute_syscall)
                        kernel_sigdelset(app_set, i);
                }
            }
        } else if (how == SIG_SETMASK) {
            /* The set of blocked signals is set to the argument set. */
            kernel_sigemptyset(&info->app_sigblocked);
            for (i=1; i<=MAX_SIGNUM; i++) {
                if (EMULATE_SIGMASK(info, i) && kernel_sigismember(&safe_set, i)) {
                    kernel_sigaddset(&info->app_sigblocked, i);
                    if (execute_syscall)
                        kernel_sigdelset(app_set, i);
                }
            }
        }
#ifdef DEBUG
        if (stats->loglevel >= 3 && (stats->logmask & LOG_ASYNCH) != 0) {
            LOG(THREAD, LOG_ASYNCH, 3, "blocked signals are now:\n");
            dump_sigset(dcontext, &info->app_sigblocked);
        }
#endif
        /* make sure we deliver pending signals that are now unblocked
         * FIXME: consider signal #S, which we intercept ourselves.
         * If S arrives, then app blocks it prior to our delivering it,
         * we then won't deliver it until app unblocks it...is this a
         * problem?  Could have arrived a little later and then we would
         * do same thing, but this way kernel may send one more than would
         * get w/o dynamo?  This goes away if we deliver signals
         * prior to letting app do a syscall.
         */
        check_signals_pending(dcontext, info);
    }
    if (!execute_syscall) {
        handle_post_sigprocmask(dcontext, how, app_set, oset, sigsetsize);
        return false; /* skip syscall */
    } else
        return true;
}

/* need to add in our signals that the app thinks are blocked */
void
handle_post_sigprocmask(dcontext_t *dcontext, int how, kernel_sigset_t *app_set,
                        kernel_sigset_t *oset, size_t sigsetsize)
{
    thread_sig_info_t *info = (thread_sig_info_t *) dcontext->signal_field;
    int i;
    if (!DYNAMO_OPTION(intercept_all_signals)) {
        /* Restore app memory */
        safe_write_ex(app_set, sizeof(*app_set), &info->pre_syscall_app_sigprocmask,
                      NULL);
    }
    if (oset != NULL) {
        if (DYNAMO_OPTION(intercept_all_signals))
            safe_write_ex(oset, sizeof(*oset), &info->pre_syscall_app_sigblocked, NULL);
        else {
            /* the syscall wrote to oset already, so just add any additional */
            for (i=1; i<=MAX_SIGNUM; i++) {
                if (EMULATE_SIGMASK(info, i) &&
                    /* use the pre-syscall value: do not take into account changes
                     * from this syscall itself! (PR 523394)
                     */
                    kernel_sigismember(&info->pre_syscall_app_sigblocked, i)) {
                    kernel_sigaddset(oset, i);
                }
            }
        }
    }
}

void
handle_sigsuspend(dcontext_t *dcontext, kernel_sigset_t *set,
                  size_t sigsetsize)
{
    thread_sig_info_t *info = (thread_sig_info_t *) dcontext->signal_field;
    int i;
    ASSERT(set != NULL);
    LOG(THREAD, LOG_ASYNCH, 2, "handle_sigsuspend\n");
    info->in_sigsuspend = true;
    info->app_sigblocked_save = info->app_sigblocked;
    kernel_sigemptyset(&info->app_sigblocked);
    for (i=1; i<=MAX_SIGNUM; i++) {
        if (EMULATE_SIGMASK(info, i) && kernel_sigismember(set, i)) {
            kernel_sigaddset(&info->app_sigblocked, i);
            kernel_sigdelset(set, i);
        }
    }
#ifdef DEBUG
    if (stats->loglevel >= 3 && (stats->logmask & LOG_ASYNCH) != 0) {
        LOG(THREAD, LOG_ASYNCH, 3, "in sigsuspend, blocked signals are now:\n");
        dump_sigset(dcontext, &info->app_sigblocked);
    }
#endif
}

/***************************************************************************
 * SIGNALFD
 */

/* Strategy: a real signalfd is a read-only file, so we can't write to one to
 * emulate signal delivery.  We also can't block signals we care about (and
 * for clients we don't want to block anything).  Thus we must emulate
 * signalfd via a pipe.  The kernel's pipe buffer should easily hold
 * even a big queue of RT signals.  Xref i#1138.
 *
 * Although signals are per-thread, fds are global, and one thread
 * could use a signalfd to see signals on another thread.
 *
 * Thus we have:
 * + global data struct "sigfd_pipe_t" stores pipe write fd and refcount
 * + global hashtable mapping read fd to sigfd_pipe_t
 * + thread has array of pointers to sigfd_pipe_t, one per signum
 * + on SYS_close, we decrement refcount
 * + on SYS_dup*, we add a new hashtable entry
 *
 * This pipe implementation has a hole: it cannot properly handle two
 * signalfds with different but overlapping signal masks (i#1189: see below).
 */
static generic_table_t *sigfd_table;

struct _sigfd_pipe_t {
    file_t write_fd;
    file_t read_fd;
    uint refcount;
    dcontext_t *dcontext;
};

static void
sigfd_pipe_free(void *ptr)
{
    sigfd_pipe_t *pipe = (sigfd_pipe_t *) ptr;
    ASSERT(pipe->refcount > 0);
    pipe->refcount--;
    if (pipe->refcount == 0) {
        if (pipe->dcontext != NULL) {
            /* Update the owning thread's info.
             * We write a NULL which is atomic.
             * The thread on exit grabs the table lock for synch and clears dcontext.
             */
            thread_sig_info_t *info = (thread_sig_info_t *)
                pipe->dcontext->signal_field;
            int sig;
            for (sig = 1; sig <= MAX_SIGNUM; sig++) {
                if (info->signalfd[sig] == pipe)
                    info->signalfd[sig] = NULL;
            }
        }
        os_close_protected(pipe->write_fd);
        os_close_protected(pipe->read_fd);
        HEAP_TYPE_FREE(GLOBAL_DCONTEXT, pipe, sigfd_pipe_t, ACCT_OTHER, PROTECTED);
    }
}

static void
signalfd_init(void)
{
#   define SIGNALFD_HTABLE_INIT_SIZE 6
    sigfd_table =
        generic_hash_create(GLOBAL_DCONTEXT, SIGNALFD_HTABLE_INIT_SIZE,
                            80 /* load factor: not perf-critical */,
                            HASHTABLE_ENTRY_SHARED | HASHTABLE_SHARED |
                            HASHTABLE_PERSISTENT | HASHTABLE_RELAX_CLUSTER_CHECKS,
                            sigfd_pipe_free _IF_DEBUG("signalfd table"));
    /* XXX: we need our lock rank to be higher than fd_table's so we
     * can call os_close_protected() when freeing.  We should
     * parametrize the generic table rank.  For now we just change it afterward
     * (we'll have issues if we ever call _resurrect):
     */
    ASSIGN_INIT_READWRITE_LOCK_FREE(sigfd_table->rwlock, sigfdtable_lock);
}

static void
signalfd_exit(void)
{
    generic_hash_destroy(GLOBAL_DCONTEXT, sigfd_table);
}

static void
signalfd_thread_exit(dcontext_t *dcontext, thread_sig_info_t *info)
{
    /* We don't free the pipe until the app closes its fd's but we need to
     * update the dcontext in the global data
     */
    int sig;
    TABLE_RWLOCK(sigfd_table, write, lock);
    for (sig = 1; sig <= MAX_SIGNUM; sig++) {
        if (info->signalfd[sig] != NULL)
            info->signalfd[sig]->dcontext = NULL;
    }
    TABLE_RWLOCK(sigfd_table, write, unlock);
}

ptr_int_t
handle_pre_signalfd(dcontext_t *dcontext, int fd, kernel_sigset_t *mask,
                    size_t sizemask, int flags)
{
    thread_sig_info_t *info = (thread_sig_info_t *) dcontext->signal_field;
    int sig;
    kernel_sigset_t local_set;
    kernel_sigset_t *set;
    ptr_int_t retval = -1;
    sigfd_pipe_t *pipe = NULL;
    LOG(THREAD, LOG_ASYNCH, 2, "%s: fd=%d, flags=0x%x\n", __FUNCTION__, fd, flags);
    if (sizemask == sizeof(sigset_t)) {
        copy_sigset_to_kernel_sigset((sigset_t *)mask, &local_set);
        set = &local_set;
    } else {
        ASSERT(sizemask == sizeof(kernel_sigset_t));
        set = mask;
    }
    if (fd != -1) {
        TABLE_RWLOCK(sigfd_table, read, lock);
        pipe = (sigfd_pipe_t *) generic_hash_lookup(GLOBAL_DCONTEXT, sigfd_table, fd);
        TABLE_RWLOCK(sigfd_table, read, unlock);
        if (pipe == NULL)
            return -EINVAL;
    } else {
        /* FIXME i#1189: currently we do not properly handle two signalfds with
         * different but overlapping signal masks, as we do not monitor the
         * read/poll syscalls and thus cannot provide a set of pipes that
         * matches the two signal sets.  For now we err on the side of sending
         * too many signals and simply conflate such sets into a single pipe.
         */
        for (sig = 1; sig <= MAX_SIGNUM; sig++) {
            if (sig == SIGKILL || sig == SIGSTOP)
                continue;
            if (kernel_sigismember(set, sig) && info->signalfd[sig] != NULL) {
                pipe = info->signalfd[sig];
                retval = dup_syscall(pipe->read_fd);
                break;
            }
        }
    }
    if (pipe == NULL) {
        int fds[2];
        /* SYS_signalfd is even newer than SYS_pipe2, so pipe2 must be avail.
         * We pass the flags through b/c the same ones (SFD_NONBLOCK ==
         * O_NONBLOCK, SFD_CLOEXEC == O_CLOEXEC) are accepted by pipe.
         */
        ptr_int_t res = dynamorio_syscall(SYS_pipe2, 2, fds, flags);
        if (res < 0)
            return res;
        pipe = HEAP_TYPE_ALLOC(GLOBAL_DCONTEXT, sigfd_pipe_t, ACCT_OTHER, PROTECTED);
        pipe->dcontext = dcontext;
        pipe->refcount = 1;

        /* Keep our write fd in the private fd space */
        pipe->write_fd = fd_priv_dup(fds[1]);
        os_close(fds[1]);
        if (TEST(SFD_CLOEXEC, flags))
            fd_mark_close_on_exec(pipe->write_fd);
        fd_table_add(pipe->write_fd, 0/*keep across fork*/);

        /* We need an un-closable copy of the read fd in case we need to dup it */
        pipe->read_fd = fd_priv_dup(fds[0]);
        if (TEST(SFD_CLOEXEC, flags))
            fd_mark_close_on_exec(pipe->read_fd);
        fd_table_add(pipe->read_fd, 0/*keep across fork*/);

        TABLE_RWLOCK(sigfd_table, write, lock);
        generic_hash_add(GLOBAL_DCONTEXT, sigfd_table, fds[0], (void *) pipe);
        TABLE_RWLOCK(sigfd_table, write, unlock);
        
        LOG(THREAD, LOG_ASYNCH, 2, "created signalfd pipe app r=%d DR r=%d w=%d\n",
            fds[0], pipe->read_fd, pipe->write_fd);
        retval = fds[0];
    }
    for (sig = 1; sig <= MAX_SIGNUM; sig++) {
        if (sig == SIGKILL || sig == SIGSTOP)
            continue;
        if (kernel_sigismember(set, sig)) {
            if (info->signalfd[sig] == NULL)
                info->signalfd[sig] = pipe;
            else
                ASSERT(info->signalfd[sig] == pipe);
            LOG(THREAD, LOG_ASYNCH, 2,
                "adding signalfd pipe %d for signal %d\n", pipe->write_fd, sig);
        } else if (info->signalfd[sig] != NULL) {
            info->signalfd[sig] = NULL;
            LOG(THREAD, LOG_ASYNCH, 2,
                "removing signalfd pipe=%d for signal %d\n", pipe->write_fd, sig);
        }
    }
    return retval;
}

static bool
notify_signalfd(dcontext_t *dcontext, thread_sig_info_t *info, int sig,
                sigframe_rt_t *frame)
{
    sigfd_pipe_t *pipe = info->signalfd[sig];
    if (pipe != NULL) {
        int res;
        struct signalfd_siginfo towrite = {0,};

        /* XXX: we should limit to a single non-RT signal until it's read (by
         * polling pipe->read_fd to see whether it has data), except we delay
         * signals and thus do want to accumulate multiple non-RT to some extent.
         * For now we go ahead and treat RT and non-RT the same.
         */
        towrite.ssi_signo = sig;
        towrite.ssi_errno = frame->info.si_errno;
        towrite.ssi_code = frame->info.si_code;
        towrite.ssi_pid = frame->info.si_pid;
        towrite.ssi_uid = frame->info.si_uid;
        towrite.ssi_fd = frame->info.si_fd;
        towrite.ssi_band = frame->info.si_band;
        /* XXX: check older glibc headers */
        towrite.ssi_tid = frame->info._sifields._timer.si_tid;
        towrite.ssi_overrun = frame->info.si_overrun;
        towrite.ssi_status = frame->info.si_status;
        towrite.ssi_utime = frame->info.si_utime;
        towrite.ssi_stime = frame->info.si_stime;
#ifdef __ARCH_SI_TRAPNO
        towrite.ssi_trapno = frame->info.si_trapno;
#endif
        towrite.ssi_addr = (ptr_int_t) frame->info.si_addr;

        /* XXX: if the pipe is full, don't write to it as it could block.  We
         * can poll to determine.  This is quite unlikely (kernel buffer is 64K
         * since 2.6.11) so for now we do not do so.
         */
        res = write_syscall(pipe->write_fd, &towrite, sizeof(towrite));
        LOG(THREAD, LOG_ASYNCH, 2,
            "writing to signalfd fd=%d for signal %d => %d\n", pipe->write_fd, sig, res);
        return true; /* signal consumed */
    }
    return false;
}

void
signal_handle_dup(dcontext_t *dcontext, file_t src, file_t dst)
{
    sigfd_pipe_t *pipe;
    TABLE_RWLOCK(sigfd_table, read, lock);
    pipe = (sigfd_pipe_t *) generic_hash_lookup(GLOBAL_DCONTEXT, sigfd_table, src);
    TABLE_RWLOCK(sigfd_table, read, unlock);
    if (pipe == NULL)
        return;
    TABLE_RWLOCK(sigfd_table, write, lock);
    pipe = (sigfd_pipe_t *) generic_hash_lookup(GLOBAL_DCONTEXT, sigfd_table, src);
    if (pipe != NULL) {
        pipe->refcount++;
        generic_hash_add(GLOBAL_DCONTEXT, sigfd_table, dst, (void *) pipe);
    }
    TABLE_RWLOCK(sigfd_table, write, unlock);
}

void
signal_handle_close(dcontext_t *dcontext, file_t fd)
{
    sigfd_pipe_t *pipe;
    TABLE_RWLOCK(sigfd_table, read, lock);
    pipe = (sigfd_pipe_t *) generic_hash_lookup(GLOBAL_DCONTEXT, sigfd_table, fd);
    TABLE_RWLOCK(sigfd_table, read, unlock);
    if (pipe == NULL)
        return;
    TABLE_RWLOCK(sigfd_table, write, lock);
    pipe = (sigfd_pipe_t *) generic_hash_lookup(GLOBAL_DCONTEXT, sigfd_table, fd);
    if (pipe != NULL) {
        /* this will call sigfd_pipe_free() */
        generic_hash_remove(GLOBAL_DCONTEXT, sigfd_table, fd);
    }
    TABLE_RWLOCK(sigfd_table, write, unlock);
}

/**** utility routines ***********************************************/
#ifdef DEBUG

static void
dump_fpstate(dcontext_t *dcontext, struct _fpstate *fp)
{
    int i,j;
#ifdef X64
    LOG(THREAD, LOG_ASYNCH, 1, "\tcwd="PFX"\n", fp->cwd);
    LOG(THREAD, LOG_ASYNCH, 1, "\tswd="PFX"\n", fp->swd);
    LOG(THREAD, LOG_ASYNCH, 1, "\ttwd="PFX"\n", fp->twd);
    LOG(THREAD, LOG_ASYNCH, 1, "\tfop="PFX"\n", fp->fop);
    LOG(THREAD, LOG_ASYNCH, 1, "\trip="PFX"\n", fp->rip);
    LOG(THREAD, LOG_ASYNCH, 1, "\trdp="PFX"\n", fp->rdp);
    LOG(THREAD, LOG_ASYNCH, 1, "\tmxcsr="PFX"\n", fp->mxcsr);
    LOG(THREAD, LOG_ASYNCH, 1, "\tmxcsr_mask="PFX"\n", fp->mxcsr_mask);
    for (i=0; i<8; i++) {
        LOG(THREAD, LOG_ASYNCH, 1, "\tst%d = 0x", i);
        for (j=0; j<4; j++) {
            LOG(THREAD, LOG_ASYNCH, 1, "%08x", fp->st_space[i*4+j]);
        }
        LOG(THREAD, LOG_ASYNCH, 1, "\n");
    }
    for (i=0; i<16; i++) {
        LOG(THREAD, LOG_ASYNCH, 1, "\txmm%d = 0x", i);
        for (j=0; j<4; j++) {
            LOG(THREAD, LOG_ASYNCH, 1, "%08x", fp->xmm_space[i*4+j]);
        }
        LOG(THREAD, LOG_ASYNCH, 1, "\n");
    }
#else
    LOG(THREAD, LOG_ASYNCH, 1, "\tcw="PFX"\n", fp->cw);
    LOG(THREAD, LOG_ASYNCH, 1, "\tsw="PFX"\n", fp->sw);
    LOG(THREAD, LOG_ASYNCH, 1, "\ttag="PFX"\n", fp->tag);
    LOG(THREAD, LOG_ASYNCH, 1, "\tipoff="PFX"\n", fp->ipoff);
    LOG(THREAD, LOG_ASYNCH, 1, "\tcssel="PFX"\n", fp->cssel);
    LOG(THREAD, LOG_ASYNCH, 1, "\tdataoff="PFX"\n", fp->dataoff);
    LOG(THREAD, LOG_ASYNCH, 1, "\tdatasel="PFX"\n", fp->datasel);
    for (i=0; i<8; i++) {
        LOG(THREAD, LOG_ASYNCH, 1, "\tst%d = ", i);
        for (j=0; j<4; j++)
            LOG(THREAD, LOG_ASYNCH, 1, "%04x ", fp->_st[i].significand[j]);
        LOG(THREAD, LOG_ASYNCH, 1, "^ %04x\n", fp->_st[i].exponent);
    }
    LOG(THREAD, LOG_ASYNCH, 1, "\tstatus=0x%04x\n", fp->status);
    LOG(THREAD, LOG_ASYNCH, 1, "\tmagic=0x%04x\n", fp->magic);  

    /* FXSR FPU environment */
    for (i=0; i<6; i++) 
        LOG(THREAD, LOG_ASYNCH, 1, "\tfxsr_env[%d] = "PFX"\n", i, fp->_fxsr_env[i]);
    LOG(THREAD, LOG_ASYNCH, 1, "\tmxcsr="PFX"\n", fp->mxcsr);
    LOG(THREAD, LOG_ASYNCH, 1, "\treserved="PFX"\n", fp->reserved);
    for (i=0; i<8; i++) {
        LOG(THREAD, LOG_ASYNCH, 1, "\tfxsr_st%d = ", i);
        for (j=0; j<4; j++)
            LOG(THREAD, LOG_ASYNCH, 1, "%04x ", fp->_fxsr_st[i].significand[j]);
        LOG(THREAD, LOG_ASYNCH, 1, "^ %04x\n", fp->_fxsr_st[i].exponent);
        /* ignore padding */
    }
    for (i=0; i<8; i++) {
        LOG(THREAD, LOG_ASYNCH, 1, "\txmm%d = ", i);
        for (j=0; j<4; j++)
            LOG(THREAD, LOG_ASYNCH, 1, "%04x ", fp->_xmm[i].element[j]);
        LOG(THREAD, LOG_ASYNCH, 1, "\n");
    }
#endif
    /* ignore padding */
    if (YMM_ENABLED()) {
        struct _xstate *xstate = (struct _xstate *) fp;
        if (fp->sw_reserved.magic1 == FP_XSTATE_MAGIC1) {
            /* i#718: for 32-bit app on 64-bit OS, the xstate_size in sw_reserved
             * is obtained via cpuid, which is the xstate size of 64-bit arch.
             */
            ASSERT(fp->sw_reserved.extended_size >= sizeof(*xstate));
            ASSERT(TEST(XCR0_AVX, fp->sw_reserved.xstate_bv));
            LOG(THREAD, LOG_ASYNCH, 1, "\txstate_bv = 0x"HEX64_FORMAT_STRING"\n",
                xstate->xstate_hdr.xstate_bv);
            for (i=0; i<NUM_XMM_SLOTS; i++) {
                LOG(THREAD, LOG_ASYNCH, 1, "\tymmh%d = ", i);
                for (j=0; j<4; j++)
                    LOG(THREAD, LOG_ASYNCH, 1, "%04x ", xstate->ymmh.ymmh_space[i*4+j]);
                LOG(THREAD, LOG_ASYNCH, 1, "\n");
            }
        }
    }
}

static void
dump_sigcontext(dcontext_t *dcontext, struct sigcontext *sc)
{
    LOG(THREAD, LOG_ASYNCH, 1, "\tgs=0x%04x"IF_NOT_X64(", __gsh=0x%04x")"\n",
        sc->gs _IF_NOT_X64(sc->__gsh));
    LOG(THREAD, LOG_ASYNCH, 1, "\tfs=0x%04x"IF_NOT_X64(", __fsh=0x%04x")"\n",
        sc->fs _IF_NOT_X64(sc->__fsh));
#ifndef X64
    LOG(THREAD, LOG_ASYNCH, 1, "\tes=0x%04x, __esh=0x%04x\n", sc->es, sc->__esh);
    LOG(THREAD, LOG_ASYNCH, 1, "\tds=0x%04x, __dsh=0x%04x\n", sc->ds, sc->__dsh);
#endif
    LOG(THREAD, LOG_ASYNCH, 1, "\txdi="PFX"\n", sc->SC_XDI);
    LOG(THREAD, LOG_ASYNCH, 1, "\txsi="PFX"\n", sc->SC_XSI);
    LOG(THREAD, LOG_ASYNCH, 1, "\txbp="PFX"\n", sc->SC_XBP);
    LOG(THREAD, LOG_ASYNCH, 1, "\txsp="PFX"\n", sc->SC_XSP);
    LOG(THREAD, LOG_ASYNCH, 1, "\txbx="PFX"\n", sc->SC_XBX);
    LOG(THREAD, LOG_ASYNCH, 1, "\txdx="PFX"\n", sc->SC_XDX);
    LOG(THREAD, LOG_ASYNCH, 1, "\txcx="PFX"\n", sc->SC_XCX);
    LOG(THREAD, LOG_ASYNCH, 1, "\txax="PFX"\n", sc->SC_XAX);
#ifdef X64
    LOG(THREAD, LOG_ASYNCH, 1, "\t r8="PFX"\n", sc->r8);
    LOG(THREAD, LOG_ASYNCH, 1, "\t r9="PFX"\n", sc->r8);
    LOG(THREAD, LOG_ASYNCH, 1, "\tr10="PFX"\n", sc->r10);
    LOG(THREAD, LOG_ASYNCH, 1, "\tr11="PFX"\n", sc->r11);
    LOG(THREAD, LOG_ASYNCH, 1, "\tr12="PFX"\n", sc->r12);
    LOG(THREAD, LOG_ASYNCH, 1, "\tr13="PFX"\n", sc->r13);
    LOG(THREAD, LOG_ASYNCH, 1, "\tr14="PFX"\n", sc->r14);
    LOG(THREAD, LOG_ASYNCH, 1, "\tr15="PFX"\n", sc->r15);
#endif
    LOG(THREAD, LOG_ASYNCH, 1, "\ttrapno="PFX"\n", sc->trapno);
    LOG(THREAD, LOG_ASYNCH, 1, "\terr="PFX"\n", sc->err);
    LOG(THREAD, LOG_ASYNCH, 1, "\txip="PFX"\n", sc->SC_XIP);
    LOG(THREAD, LOG_ASYNCH, 1, "\tcs=0x%04x"IF_NOT_X64(", __esh=0x%04x")"\n",
        sc->cs _IF_NOT_X64(sc->__csh));
    LOG(THREAD, LOG_ASYNCH, 1, "\teflags="PFX"\n", sc->SC_XFLAGS);
#ifndef X64
    LOG(THREAD, LOG_ASYNCH, 1, "\tesp_at_signal="PFX"\n", sc->esp_at_signal);
    LOG(THREAD, LOG_ASYNCH, 1, "\tss=0x%04x, __ssh=0x%04x\n", sc->ss, sc->__ssh);
#endif
    if (sc->fpstate == NULL)
        LOG(THREAD, LOG_ASYNCH, 1, "\tfpstate=<NULL>\n");
    else
        dump_fpstate(dcontext, sc->fpstate);
    LOG(THREAD, LOG_ASYNCH, 1, "\toldmask="PFX"\n", sc->oldmask);
    LOG(THREAD, LOG_ASYNCH, 1, "\tcr2="PFX"\n", sc->cr2);
}

static void
dump_sigset(dcontext_t *dcontext, kernel_sigset_t *set)
{
    int sig;
    for (sig=1; sig<=MAX_SIGNUM; sig++) {
        if (kernel_sigismember(set, sig))
            LOG(THREAD, LOG_ASYNCH, 1, "\t%d = blocked\n", sig);
    }
}
#endif /* DEBUG */

/* PR 205795: to avoid lock problems w/ in_fcache (it grabs a lock, we
 * could have interrupted someone holding that), we first check
 * whereami --- if whereami is WHERE_FCACHE we still check the pc
 * to distinguish generated routines, but at least we're certain
 * it's not in DR where it could own a lock.
 * We can't use is_on_dstack() here b/c we need to handle clean call
 * arg crashes -- which is too bad since checking client dll and DR dll is
 * not sufficient due to calls to ntdll, libc, or pc being in gencode.
 */
static bool
safe_is_in_fcache(dcontext_t *dcontext, app_pc pc, app_pc xsp)
{
    if (dcontext->whereami != WHERE_FCACHE ||
        IF_CLIENT_INTERFACE(is_in_client_lib(pc) ||)
        is_in_dynamo_dll(pc) ||
        is_on_initstack(xsp))
        return false;
    /* Reasonably certain not in DR code, so no locks should be held */
    return in_fcache(pc);
}

static bool
safe_is_in_coarse_stubs(dcontext_t *dcontext, app_pc pc, app_pc xsp)
{
    if (dcontext->whereami != WHERE_FCACHE ||
        IF_CLIENT_INTERFACE(is_in_client_lib(pc) ||)
        is_in_dynamo_dll(pc) ||
        is_on_initstack(xsp))
        return false;
    /* Reasonably certain not in DR code, so no locks should be held */
    return in_coarse_stubs(pc);
}

static bool
is_on_alt_stack(dcontext_t *dcontext, byte *sp)
{
#ifdef HAVE_SIGALTSTACK
    thread_sig_info_t *info = (thread_sig_info_t *) dcontext->signal_field;
    return (sp >= (byte *) info->sigstack.ss_sp &&
            /* deliberate equality check since stacks often init to top */
            sp <= (byte *) (info->sigstack.ss_sp + info->sigstack.ss_size));
#else
    return false;
#endif
}

void
sigcontext_to_mcontext(priv_mcontext_t *mc, struct sigcontext *sc)
{
    ASSERT(mc != NULL && sc != NULL);
    mc->xax = sc->SC_XAX;
    mc->xbx = sc->SC_XBX;
    mc->xcx = sc->SC_XCX;
    mc->xdx = sc->SC_XDX;
    mc->xsi = sc->SC_XSI;
    mc->xdi = sc->SC_XDI;
    mc->xbp = sc->SC_XBP;
    mc->xsp = sc->SC_XSP;
    mc->xflags = sc->SC_XFLAGS;
    mc->pc = (app_pc) sc->SC_XIP;
#ifdef X64
    mc->r8  = sc->r8;
    mc->r9  = sc->r9;
    mc->r10 = sc->r10;
    mc->r11 = sc->r11;
    mc->r12 = sc->r12;
    mc->r13 = sc->r13;
    mc->r14 = sc->r14;
    mc->r15 = sc->r15;
#endif
    if (sc->fpstate != NULL) {
        int i;
        for (i=0; i<NUM_XMM_SLOTS; i++) {
            memcpy(&mc->ymm[i], &sc->fpstate->IF_X64_ELSE(xmm_space[i*4],_xmm[i]),
                   XMM_REG_SIZE);
        }
        if (YMM_ENABLED()) {
            struct _xstate *xstate = (struct _xstate *) sc->fpstate;
            if (sc->fpstate->sw_reserved.magic1 == FP_XSTATE_MAGIC1) {
                /* i#718: for 32-bit app on 64-bit OS, the xstate_size in sw_reserved
                 * is obtained via cpuid, which is the xstate size of 64-bit arch.
                 */
                ASSERT(sc->fpstate->sw_reserved.extended_size >= sizeof(*xstate));
                ASSERT(TEST(XCR0_AVX, sc->fpstate->sw_reserved.xstate_bv));
                for (i=0; i<NUM_XMM_SLOTS; i++) {
                    memcpy(&mc->ymm[i].u32[4], &xstate->ymmh.ymmh_space[i*4],
                           YMMH_REG_SIZE);
                }
            }
        }
    }
}

/* Note that unlike mcontext_to_context(), this routine does not fill in
 * any state that is not present in the mcontext: in particular, it assumes
 * the sigcontext already contains the native fpstate.  If the caller
 * is generating a synthetic sigcontext, the caller should call
 * save_fpstate() before calling this routine.
 */
void
mcontext_to_sigcontext(struct sigcontext *sc, priv_mcontext_t *mc)
{
    sc->SC_XAX = mc->xax;
    sc->SC_XBX = mc->xbx;
    sc->SC_XCX = mc->xcx;
    sc->SC_XDX = mc->xdx;
    sc->SC_XSI = mc->xsi;
    sc->SC_XDI = mc->xdi;
    sc->SC_XBP = mc->xbp;
    sc->SC_XSP = mc->xsp;
    sc->SC_XFLAGS = mc->xflags;
    sc->SC_XIP = (ptr_uint_t) mc->pc;
#ifdef X64
    sc->r8  = mc->r8;
    sc->r9  = mc->r9;
    sc->r10 = mc->r10;
    sc->r11 = mc->r11;
    sc->r12 = mc->r12;
    sc->r13 = mc->r13;
    sc->r14 = mc->r14;
    sc->r15 = mc->r15;
#endif
    if (sc->fpstate != NULL) {
        int i;
        for (i=0; i<NUM_XMM_SLOTS; i++) {
            memcpy(&sc->fpstate->IF_X64_ELSE(xmm_space[i*4],_xmm[i]), &mc->ymm[i],
                   XMM_REG_SIZE);
        }
        if (YMM_ENABLED()) {
            struct _xstate *xstate = (struct _xstate *) sc->fpstate;
            if (sc->fpstate->sw_reserved.magic1 == FP_XSTATE_MAGIC1) {
                /* i#718: for 32-bit app on 64-bit OS, the xstate_size in sw_reserved
                 * is obtained via cpuid, which is the xstate size of 64-bit arch.
                 */
                ASSERT(sc->fpstate->sw_reserved.extended_size >= sizeof(*xstate));
                ASSERT(TEST(XCR0_AVX, sc->fpstate->sw_reserved.xstate_bv));
                for (i=0; i<NUM_XMM_SLOTS; i++) {
                    memcpy(&xstate->ymmh.ymmh_space[i*4], &mc->ymm[i].u32[4],
                           YMMH_REG_SIZE);
                }
            }
        }
    }
}

/* Returns whether successful.  If avoid_failure, tries to translate
 * at least pc if not successful.  Pass f if known.
 */
static bool
translate_sigcontext(dcontext_t *dcontext,  struct sigcontext *sc, bool avoid_failure,
                     fragment_t *f)
{
    bool success = false;
    priv_mcontext_t mcontext;
 
    sigcontext_to_mcontext(&mcontext, sc);
    /* FIXME: if cannot find exact match, we're in trouble!
     * probably ok to delay, since that indicates not a synchronous
     * signal. 
     */
    /* FIXME : in_fcache() (called by recreate_app_state) grabs fcache 
     * fcache_unit_areas.lock, we could deadlock! Also on initexit_lock 
     * == PR 205795/1317
     */
    /* For safe recreation we need to either be couldbelinking or hold the
     * initexit lock (to keep someone from flushing current fragment), the
     * initexit lock is easier
     */
    mutex_lock(&thread_initexit_lock);
    /* PR 214962: we assume we're going to relocate to this stored context,
     * so we restore memory now 
     */
    if (translate_mcontext(dcontext->thread_record, &mcontext,
                           true/*restore memory*/, f)) {
        mcontext_to_sigcontext(sc, &mcontext);
        success = true;
    } else {
        if (avoid_failure) {
            ASSERT_NOT_REACHED(); /* is ok to break things, is UNIX :) */
            /* FIXME : what to do? reg state might be wrong at least get pc */
            if (safe_is_in_fcache(dcontext, (cache_pc)sc->SC_XIP, (app_pc)sc->SC_XSP)) {
                sc->SC_XIP = (ptr_uint_t)recreate_app_pc(dcontext, mcontext.pc, f);
                ASSERT(sc->SC_XIP != (ptr_uint_t)NULL);
            } else {
                /* FIXME : can't even get pc right, what do we do here? */
                sc->SC_XIP = 0;
            }
        }
    }
    mutex_unlock(&thread_initexit_lock);
    LOG(THREAD, LOG_ASYNCH, 3,
        "\ttranslate_sigcontext: just set frame's eip to "PFX"\n", sc->SC_XIP);
    return success;
}

/* Takes an os-specific context */
void
thread_set_self_context(void *cxt)
{
    dcontext_t *dcontext = get_thread_private_dcontext();
    /* Unlike Windows we can't say "only set this subset of the
     * full machine state", so we need to get the rest of the state,
     */
    sigframe_rt_t frame;
    struct sigcontext *sc = (struct sigcontext *) cxt;
    app_pc xsp_for_sigreturn;
#ifdef VMX86_SERVER
    ASSERT_NOT_IMPLEMENTED(false); /* PR 405694: can't use regular sigreturn! */
#endif
    /* We need room for full xstate if nec (this is x86=944, x64=832 bytes).
     * A real signal frame would be var-sized but we don't want to dynamically
     * allocate, and only the kernel looks at this, so no risk of some
     * app seeing a weird frame size.
     */
    struct _xstate __attribute__ ((aligned (AVX_ALIGNMENT))) xstate;
    frame.uc.uc_mcontext.fpstate = &xstate.fpstate;
    memset(&frame, 0, sizeof(frame));
    frame.uc.uc_mcontext = *sc;
    save_fpstate(dcontext, &frame);
    /* The kernel calls do_sigaltstack on sys_rt_sigreturn primarily to ensure
     * the frame is ok, but the side effect is we can mess up our own altstack
     * settings if we're not careful.  Having invalid ss_size looks good for
     * kernel 2.6.23.9 at least so we leave frame.uc.uc_stack as all zeros.
     */
    /* make sure sigreturn's mask setting doesn't change anything */
    sigprocmask_syscall(SIG_SETMASK, NULL, &frame.uc.uc_sigmask,
                        sizeof(frame.uc.uc_sigmask));
    LOG(THREAD_GET, LOG_ASYNCH, 2, "thread_set_self_context: pc="PFX"\n", sc->SC_XIP);
    /* set up xsp to point at &frame + sizeof(char*) */
    xsp_for_sigreturn = ((app_pc)&frame) + sizeof(char*);
    asm("mov  %0, %%"ASM_XSP : : "m"(xsp_for_sigreturn));
    asm("jmp dynamorio_sigreturn");
    ASSERT_NOT_REACHED();
}

/* Takes a priv_mcontext_t */
void
thread_set_self_mcontext(priv_mcontext_t *mc)
{
    struct sigcontext sc;
    mcontext_to_sigcontext(&sc, mc);
    thread_set_self_context((void *)&sc);
    ASSERT_NOT_REACHED();
}

static bool
sig_has_restorer(thread_sig_info_t *info, int sig)
{
#ifdef VMX86_SERVER
    /* vmkernel ignores SA_RESTORER (PR 405694) */
    return false;
#endif
    if (info->app_sigaction[sig] == NULL)
        return false;
    if (TEST(SA_RESTORER, info->app_sigaction[sig]->flags))
        return true;
    if (info->app_sigaction[sig]->restorer == NULL)
        return false;
    /* we cache the result due to the safe_read cost */
    if (info->restorer_valid[sig] == -1) {
        /* With older kernels, don't seem to need flag: if sa_restorer !=
         * NULL kernel will use it.  But with newer kernels that's not
         * true, and sometimes libc does pass non-NULL.
         */
        /* Signal restorer code for Ubuntu 7.04:
         *   0xffffe420 <__kernel_sigreturn+0>:      pop    %eax
         *   0xffffe421 <__kernel_sigreturn+1>:      mov    $0x77,%eax
         *   0xffffe426 <__kernel_sigreturn+6>:      int    $0x80
         *   
         *   0xffffe440 <__kernel_rt_sigreturn+0>:   mov    $0xad,%eax
         *   0xffffe445 <__kernel_rt_sigreturn+5>:   int    $0x80
         */
        static const byte SIGRET_NONRT[8] =
          {0x58, 0xb8, 0x77, 0x00, 0x00, 0x00, 0xcd, 0x80};
        static const byte SIGRET_RT[8] = 
          {0xb8, 0xad, 0x00, 0x00, 0x00, 0xcd, 0x80};
        byte buf[MAX(sizeof(SIGRET_NONRT), sizeof(SIGRET_RT))]= {0};
        if (safe_read(info->app_sigaction[sig]->restorer, sizeof(buf), buf) &&
            ((IS_RT_FOR_APP(info, sig) &&
              memcmp(buf, SIGRET_RT, sizeof(SIGRET_RT)) == 0) ||
             (!IS_RT_FOR_APP(info, sig) &&
              memcmp(buf, SIGRET_NONRT, sizeof(SIGRET_NONRT)) == 0))) {
            LOG(THREAD_GET, LOG_ASYNCH, 2,
                "sig_has_restorer %d: "PFX" looks like restorer, using w/o flag\n",
                sig, info->app_sigaction[sig]->restorer);
            info->restorer_valid[sig] = 1;
        } else
            info->restorer_valid[sig] = 0;
    }
    return (info->restorer_valid[sig] == 1);
}

/* Returns the size of the frame for delivering to the app.
 * For x64 this does NOT include struct _fpstate.
 */
static uint
get_app_frame_size(thread_sig_info_t *info, int sig)
{
    if (IS_RT_FOR_APP(info, sig))
        return sizeof(sigframe_rt_t);
    else
        return sizeof(sigframe_plain_t);
}

static struct sigcontext *
get_sigcontext_from_rt_frame(sigframe_rt_t *frame)
{
    return (struct sigcontext *) &(frame->uc.uc_mcontext);
}

static struct sigcontext *
get_sigcontext_from_app_frame(thread_sig_info_t *info, int sig, void *frame)
{
    struct sigcontext *sc;
    bool rtframe = IS_RT_FOR_APP(info, sig);
    if (rtframe) {
        sc = get_sigcontext_from_rt_frame((sigframe_rt_t *)frame);
    } else {
        sc = (struct sigcontext *) &(((sigframe_plain_t *)frame)->sc);
    }
    return sc;
}

static struct sigcontext *
get_sigcontext_from_pending(thread_sig_info_t *info, int sig)
{
    ASSERT(info->sigpending[sig] != NULL);
    return (struct sigcontext *) &(info->sigpending[sig]->rt_frame.uc.uc_mcontext);
}

/* Returns the address on the appropriate signal stack where we should copy
 * the frame.
 * If frame is NULL, assumes signal happened while in DR and has been delayed,
 * and thus we need to provide fpstate regardless of whether the original
 * had it.  If frame is non-NULL, matches frame's amount of fpstate.
 */
static byte *
get_sigstack_frame_ptr(dcontext_t *dcontext, int sig, sigframe_rt_t *frame)
{
    thread_sig_info_t *info = (thread_sig_info_t *) dcontext->signal_field;
    struct sigcontext *sc = (frame == NULL) ?
        get_sigcontext_from_pending(info, sig) :
        get_sigcontext_from_rt_frame(frame);
    byte *sp;

    if (frame != NULL) {
        /* signal happened while in cache, grab interrupted xsp */
        sp = (byte *) sc->SC_XSP;
        LOG(THREAD, LOG_ASYNCH, 3,
            "get_sigstack_frame_ptr: using frame's xsp "PFX"\n", sp);
    } else {
        /* signal happened while in DR, use stored xsp */
        sp = (byte *) get_mcontext(dcontext)->xsp;
        LOG(THREAD, LOG_ASYNCH, 3, "get_sigstack_frame_ptr: using app xsp "PFX"\n", sp);
    }

    if (APP_HAS_SIGSTACK(info)) {
        /* app has own signal stack */
        LOG(THREAD, LOG_ASYNCH, 3,
            "get_sigstack_frame_ptr: app has own stack "PFX"\n",
            info->app_sigstack.ss_sp);
        LOG(THREAD, LOG_ASYNCH, 3,
            "\tcur sp="PFX" vs app stack "PFX"-"PFX"\n",
            sp, info->app_sigstack.ss_sp,
            info->app_sigstack.ss_sp + info->app_sigstack.ss_size);
        if (sp > (byte *)info->app_sigstack.ss_sp &&
            sp - (byte *)info->app_sigstack.ss_sp < info->app_sigstack.ss_size) {
            /* we're currently in the alt stack, so use current xsp */
            LOG(THREAD, LOG_ASYNCH, 3,
                "\tinside alt stack, so using current xsp "PFX"\n", sp);
        } else {
            /* need to go to top, stack grows down */
            sp = info->app_sigstack.ss_sp + info->app_sigstack.ss_size - 1;
            LOG(THREAD, LOG_ASYNCH, 3,
                "\tnot inside alt stack, so using base xsp "PFX"\n", sp);
        }
    } 
    /* now get frame pointer: need to go down to first field of frame */
    sp -= get_app_frame_size(info, sig);
    if (frame == NULL) {
        /* XXX i#641: we always include space for full xstate,
         * even if we don't use it all, which does not match what the
         * kernel does, but we're not tracking app actions to know whether
         * we can skip lazy fpstate on the delay
         */
        sp -= XSTATE_FRAME_EXTRA;
    } else {
        if (sc->fpstate != NULL) {
            /* The kernel doesn't seem to lazily include avx, so we don't either,
             * which simplifies all our frame copying: if YMM_ENABLED() and the
             * fpstate pointer is non-NULL, then we assume there's space for
             * full xstate
             */
            sp -= XSTATE_FRAME_EXTRA;
            DOCHECK(1, {
                if (YMM_ENABLED()) {
                    ASSERT_CURIOSITY(sc->fpstate->sw_reserved.magic1 == FP_XSTATE_MAGIC1);
                    ASSERT(sc->fpstate->sw_reserved.extended_size <= XSTATE_FRAME_EXTRA);
                }
            });
        }
    }
    /* PR 369907: don't forget the redzone */
    sp -= REDZONE_SIZE;

    /* Align to 16-bytes.  The kernel does this for both 32 and 64-bit code
     * these days, so we do as well.
     */
    sp = (byte *) ALIGN_BACKWARD(sp, 16);
    sp -= sizeof(reg_t);  /* Model retaddr. */
    return sp;
}

#ifndef X64
static void
convert_frame_to_nonrt(dcontext_t *dcontext, int sig, sigframe_rt_t *f_old,
                       sigframe_plain_t *f_new)
{
    struct sigcontext *sc_old = get_sigcontext_from_rt_frame(f_old);
    f_new->pretcode = f_old->pretcode;
    f_new->sig = f_old->sig;
    memcpy(&f_new->sc, &f_old->uc.uc_mcontext, sizeof(struct sigcontext));
    if (sc_old->fpstate != NULL) {
        /* up to caller to include enough space for fpstate at end */
        byte *new_fpstate = (byte *)
            ALIGN_FORWARD(((byte *)f_new) + sizeof(*f_new), XSTATE_ALIGNMENT);
        memcpy(new_fpstate, sc_old->fpstate, XSTATE_DATA_SIZE);
        f_new->sc.fpstate = (struct _fpstate *) new_fpstate;
    }
    f_new->sc.oldmask = f_old->uc.uc_sigmask.sig[0];
    memcpy(&f_new->extramask, &f_old->uc.uc_sigmask.sig[1],
           (_NSIG_WORDS-1) * sizeof(uint));
    memcpy(&f_new->retcode, &f_old->retcode, RETCODE_SIZE);
    LOG(THREAD, LOG_ASYNCH, 3, "\tconverted rt frame to non-rt frame\n");
    /* now fill in our extra field */
    f_new->sig_noclobber = f_new->sig;
}

/* separated out to avoid the stack size cost on the common path */
static void
convert_frame_to_nonrt_partial(dcontext_t *dcontext, int sig, sigframe_rt_t *f_old,
                               sigframe_plain_t *f_new, size_t size)
{
    char frame_plus_xstate[sizeof(sigframe_plain_t) + AVX_FRAME_EXTRA];
    sigframe_plain_t *f_plain = (sigframe_plain_t *) frame_plus_xstate;
    convert_frame_to_nonrt(dcontext, sig, f_old, f_plain);
    memcpy(f_new, f_plain, size);
}
#endif

/* Exported for call from master_signal_handler asm routine.
 * For the rt signal frame f_old that was copied to f_new, updates
 * the intra-frame absolute pointers to point to the new addresses
 * in f_new.
 * Only updates the pretcode to the stored app restorer if for_app.
 */
void
fixup_rtframe_pointers(dcontext_t *dcontext, int sig,
                       sigframe_rt_t *f_old, sigframe_rt_t *f_new, bool for_app)
{
    if (dcontext == NULL)
        dcontext = get_thread_private_dcontext();
    ASSERT(dcontext != NULL);
    thread_sig_info_t *info = (thread_sig_info_t *) dcontext->signal_field;
    bool has_restorer = sig_has_restorer(info, sig);
#ifdef DEBUG
    uint level = 3;
# ifndef HAVE_PROC_MAPS
    /* avoid logging every single TRY probe fault */
    if (!dynamo_initialized)
        level = 5;
# endif
#endif

    if (has_restorer && for_app)
        f_new->pretcode = (char *) info->app_sigaction[sig]->restorer;
    else {
#ifdef VMX86_SERVER
        /* PR 404712: skip kernel's restorer code */
        if (for_app)
            f_new->pretcode = (char *) dynamorio_sigreturn;
#else
# ifdef X64
        ASSERT(!for_app);
# else
        /* only point at retcode if old one was -- with newer OS, points at
         * vsyscall page and there is no restorer, yet stack restorer code left
         * there for gdb compatibility
         */
        if (f_old->pretcode == f_old->retcode)
            f_new->pretcode = f_new->retcode;
        /* else, pointing at vsyscall, or we set it to dynamorio_sigreturn in
         * master_signal_handler 
         */
        LOG(THREAD, LOG_ASYNCH, level, "\tleaving pretcode with old value\n");
# endif
#endif
    }
#ifndef X64
    f_new->pinfo = &(f_new->info);
    f_new->puc = &(f_new->uc);
#endif
    if (f_old->uc.uc_mcontext.fpstate != NULL) {
        uint frame_size = get_app_frame_size(info, sig);
        byte *frame_end = ((byte *)f_new) + frame_size;
        byte *tgt = (byte *) ALIGN_FORWARD(frame_end, XSTATE_ALIGNMENT);
        ASSERT(tgt - frame_end <= XSTATE_FRAME_EXTRA);
        memcpy(tgt, f_old->uc.uc_mcontext.fpstate, sizeof(struct _fpstate));
        f_new->uc.uc_mcontext.fpstate = (struct _fpstate *) tgt;
        if (YMM_ENABLED()) {
            struct _xstate *xstate_new = (struct _xstate *) tgt;
            struct _xstate *xstate_old = (struct _xstate *) f_old->uc.uc_mcontext.fpstate;
            memcpy(&xstate_new->xstate_hdr, &xstate_old->xstate_hdr,
                   sizeof(xstate_new->xstate_hdr));
            memcpy(&xstate_new->ymmh, &xstate_old->ymmh, sizeof(xstate_new->ymmh));
        }
        LOG(THREAD, LOG_ASYNCH, level+1, "\tfpstate old="PFX" new="PFX"\n",
            f_old->uc.uc_mcontext.fpstate, f_new->uc.uc_mcontext.fpstate);
    } else {
        /* if fpstate is not set up, we're delivering signal immediately,
         * and we shouldn't need an fpstate since DR code won't modify it;
         * only if we delayed will we need it, and when delaying we make
         * room and set up the pointer in copy_frame_to_pending.
         * xref i#641.
         */
        LOG(THREAD, LOG_ASYNCH, level+1, "\tno fpstate needed\n");
    }
    LOG(THREAD, LOG_ASYNCH, level, "\tretaddr = "PFX"\n", f_new->pretcode);
#ifdef RETURN_AFTER_CALL
    info->signal_restorer_retaddr = (app_pc) f_new->pretcode;
#endif
    /* 32-bit kernel copies to aligned buf first */
    IF_X64(ASSERT(ALIGNED(f_new->uc.uc_mcontext.fpstate, 16)));
}

/* Copies frame to sp.
 * PR 304708: we now leave in rt form right up until we copy to the
 * app stack, so that we can deliver to a client at a safe spot
 * in rt form, so this routine now converts to a plain frame if necessary.
 * If no restorer, touches up pretcode
 * (and if rt_frame, touches up pinfo and puc)
 * Also touches up fpstate pointer
 */
static void
copy_frame_to_stack(dcontext_t *dcontext, int sig, sigframe_rt_t *frame, byte *sp)
{
    thread_sig_info_t *info = (thread_sig_info_t *) dcontext->signal_field;
    bool rtframe = IS_RT_FOR_APP(info, sig);
    uint frame_size = get_app_frame_size(info, sig);
#ifndef X64
    bool has_restorer = sig_has_restorer(info, sig);
#endif
    byte *check_pc;
    uint size = frame_size;
    struct sigcontext *sc = get_sigcontext_from_rt_frame(frame);
    size += (sc->fpstate == NULL ? 0 : XSTATE_FRAME_EXTRA);

    LOG(THREAD, LOG_ASYNCH, 3, "copy_frame_to_stack: rt=%d, src="PFX", sp="PFX"\n",
        rtframe, frame, sp);

    /* before we write to the app's stack we need to see if it's writable */
    check_pc = (byte *) ALIGN_BACKWARD(sp, PAGE_SIZE);
    while (check_pc < (byte *)sp + size) {
        uint prot;
        DEBUG_DECLARE(bool ok = )
            get_memory_info(check_pc, NULL, NULL, &prot);
        ASSERT(ok);
        if (!TEST(MEMPROT_WRITE, prot)) {
            size_t rest = (byte *)sp + size - check_pc;
            if (is_executable_area_writable(check_pc)) {
                LOG(THREAD, LOG_ASYNCH, 2,
                    "\tcopy_frame_to_stack: part of stack is unwritable-by-us @"PFX"\n",
                    check_pc);
                flush_fragments_and_remove_region(dcontext, check_pc, rest,
                                                  false /* don't own initexit_lock */,
                                                  false /* keep futures */);
            } else {
                LOG(THREAD, LOG_ASYNCH, 2,
                    "\tcopy_frame_to_stack: part of stack is unwritable @"PFX"\n",
                    check_pc);
                /* copy what we can */
                if (rtframe)
                    memcpy(sp, frame, rest);
#ifndef X64
                else {
                    convert_frame_to_nonrt_partial(dcontext, sig, frame,
                                                   (sigframe_plain_t *) sp, rest);
                }
#endif
                /* now throw exception
                 * FIXME: what give as address?  what does kernel use?
                 * If the app intercepts SIGSEGV then we'll come right back
                 * here, so we terminate explicitly instead.  FIXME: set exit
                 * code properly: xref PR 205310.
                 */
                if (info->app_sigaction[SIGSEGV] == NULL)
                    os_forge_exception(0, UNREADABLE_MEMORY_EXECUTION_EXCEPTION);
                else
                    os_terminate(dcontext, TERMINATE_PROCESS);
                ASSERT_NOT_REACHED();
            }
        }
        check_pc += PAGE_SIZE;
    }
    if (rtframe)
        memcpy(sp, frame, frame_size);
#ifndef X64
    else
        convert_frame_to_nonrt(dcontext, sig, frame, (sigframe_plain_t *) sp);
#endif

    /* if !has_restorer we do NOT add the restorer code to the exec list here,
     * to avoid removal problems (if handler never returns) and consistency problems
     * (would have to mark as selfmod right now if on stack).
     * for PROGRAM_SHEPHERDING we recognize as a pattern, and for consistency we
     * allow entire region once try to execute -- not a performance worry since should
     * very rarely be on the stack: should either be libc restorer code or with recent
     * OS in rx vsyscall page.
     */

    /* fix up pretcode, pinfo, puc, fpstate */
    if (rtframe) {
        fixup_rtframe_pointers(dcontext, sig, frame, (sigframe_rt_t *) sp,
                               true/*for app*/);
    } else {
#ifdef X64
        ASSERT_NOT_REACHED();
#else
        sigframe_plain_t *f_new = (sigframe_plain_t *) sp;
# ifndef VMX86_SERVER
        sigframe_plain_t *f_old = (sigframe_plain_t *) frame;
# endif
        if (has_restorer)
            f_new->pretcode = (char *) info->app_sigaction[sig]->restorer;
        else {
# ifdef VMX86_SERVER
            /* PR 404712: skip kernel's restorer code */
            f_new->pretcode = (char *) dynamorio_nonrt_sigreturn;
# else
            /* see comments in rt case above */
            if (f_old->pretcode == f_old->retcode)
                f_new->pretcode = f_new->retcode;
            else {
                /* whether we set to dynamorio_sigreturn in master_signal_handler
                 * or it's still vsyscall page, we have to convert to non-rt
                 */
                f_new->pretcode = (char *) dynamorio_nonrt_sigreturn;
            } /* else, pointing at vsyscall most likely */
            LOG(THREAD, LOG_ASYNCH, 3, "\tleaving pretcode with old value\n");
# endif
        }
        /* convert_frame_to_nonrt*() should have updated fpstate pointer.
         * The inlined fpstate is no longer used on new kernels, and we do that
         * as well on older kernels.
         */
        ASSERT(f_new->sc.fpstate != &f_new->fpstate);
        LOG(THREAD, LOG_ASYNCH, 3, "\tretaddr = "PFX"\n", f_new->pretcode);
# ifdef RETURN_AFTER_CALL
        info->signal_restorer_retaddr = (app_pc) f_new->pretcode;
# endif
        /* 32-bit kernel copies to aligned buf so no assert on fpstate alignment */
#endif
    }
}

/* Copies frame to pending slot.
 * PR 304708: we now leave in rt form right up until we copy to the
 * app stack, so that we can deliver to a client at a safe spot
 * in rt form.
 */
static void
copy_frame_to_pending(dcontext_t *dcontext, int sig, sigframe_rt_t *frame
                      _IF_CLIENT(byte *access_address))
{
    thread_sig_info_t *info = (thread_sig_info_t *) dcontext->signal_field;
    sigframe_rt_t *dst = &(info->sigpending[sig]->rt_frame);
    LOG(THREAD, LOG_ASYNCH, 3, "copy_frame_to_pending\n");
#ifdef DEBUG
    if (stats->loglevel >= 3 && (stats->logmask & LOG_ASYNCH) != 0) {
        LOG(THREAD, LOG_ASYNCH, 3, "sigcontext:\n");
        dump_sigcontext(dcontext, get_sigcontext_from_rt_frame(frame));
    }
#endif
    memcpy(dst, frame, sizeof(*dst));

    /* For lazy fpstate, it's possible there was no fpstate when the kernel
     * sent us the frame, but in between then and now the app executed some
     * fp or xmm/ymm instrs.  Today we always add fpstate just in case.
     * XXX i#641 optimization: track whether any fp/xmm/ymm
     * instrs happened and avoid this.
     */
    /* we'll fill in updated fpstate at delivery time, but we go ahead and
     * copy now in case our own retrieval somehow misses some fields
     */
    if (frame->uc.uc_mcontext.fpstate != NULL) {
        memcpy(&info->sigpending[sig]->xstate, frame->uc.uc_mcontext.fpstate,
               /* XXX: assuming full xstate if avx is enabled */
               XSTATE_DATA_SIZE);
    }
    /* we must set the pointer now so that later save_fpstate, etc. work */
    dst->uc.uc_mcontext.fpstate = (struct _fpstate *) &info->sigpending[sig]->xstate;        
#ifdef CLIENT_INTERFACE
    info->sigpending[sig]->access_address = access_address;
#endif
    info->sigpending[sig]->use_sigcontext = false;
}

/**** real work ***********************************************/

#ifdef CLIENT_INTERFACE
static dr_signal_action_t
send_signal_to_client(dcontext_t *dcontext, int sig, sigframe_rt_t *frame,
                      struct sigcontext *raw_sc, byte *access_address,
                      bool blocked, fragment_t *fragment)
{
    struct sigcontext *sc = (struct sigcontext *) &(frame->uc.uc_mcontext);
    dr_siginfo_t si;
    dr_signal_action_t action;
    if (!dr_signal_hook_exists())
        return DR_SIGNAL_DELIVER;
    LOG(THREAD, LOG_ASYNCH, 2, "sending signal to client\n");
    si.sig = sig;
    si.drcontext = (void *) dcontext;
    /* It's safe to allocate since we do not send signals that interrupt DR.
     * With priv_mcontext_t x2 that's a little big for stack alloc.
     */
    si.mcontext = heap_alloc(dcontext, sizeof(*si.mcontext) HEAPACCT(ACCT_OTHER));
    si.raw_mcontext = heap_alloc(dcontext, sizeof(*si.raw_mcontext) HEAPACCT(ACCT_OTHER));
    dr_mcontext_init(si.mcontext);
    dr_mcontext_init(si.raw_mcontext);
    /* i#207: fragment tag and fcache start pc on fault. */
    si.fault_fragment_info.tag = NULL;
    si.fault_fragment_info.cache_start_pc = NULL;
    /* i#182/PR 449996: we provide the pre-translation context */
    if (raw_sc != NULL) {
        fragment_t  wrapper;
        si.raw_mcontext_valid = true;
        sigcontext_to_mcontext(dr_mcontext_as_priv_mcontext(si.raw_mcontext), raw_sc);
        /* i#207: fragment tag and fcache start pc on fault. */
        /* FIXME: we should avoid the fragment_pclookup since it is expensive 
         * and since we already did the work of a lookup when translating 
         */
        if (fragment == NULL)
            fragment = fragment_pclookup(dcontext, si.raw_mcontext->pc, &wrapper);
        if (fragment != NULL && !hide_tag_from_client(fragment->tag)) {
            si.fault_fragment_info.tag = fragment->tag;
            si.fault_fragment_info.cache_start_pc = FCACHE_ENTRY_PC(fragment);
            si.fault_fragment_info.is_trace = TEST(FRAG_IS_TRACE, 
                                                    fragment->flags);
            si.fault_fragment_info.app_code_consistent = 
                !TESTANY(FRAG_WAS_DELETED|FRAG_SELFMOD_SANDBOXED, 
                         fragment->flags);
        }
    } else
        si.raw_mcontext_valid = false;
    /* The client has no way to calculate this when using
     * instrumentation that deliberately faults (to shift a rare event
     * out of the fastpath) so we provide it.  When raw_mcontext is
     * available the client can calculate it, but we provide it as a
     * convenience anyway.
     */
    si.access_address = access_address;
    si.blocked = blocked;
    sigcontext_to_mcontext(dr_mcontext_as_priv_mcontext(si.mcontext), sc);
    /* We disallow the client calling dr_redirect_execution(), so we
     * will not leak si
     */
    action = instrument_signal(dcontext, &si);
    if (action == DR_SIGNAL_DELIVER ||
        action == DR_SIGNAL_REDIRECT) {
        /* propagate client changes */
        CLIENT_ASSERT(si.mcontext->flags == DR_MC_ALL,
                      "signal mcontext flags cannot be changed");
        mcontext_to_sigcontext(sc, dr_mcontext_as_priv_mcontext(si.mcontext));
    } else if (action == DR_SIGNAL_SUPPRESS && raw_sc != NULL) {
        /* propagate client changes */
        CLIENT_ASSERT(si.raw_mcontext->flags == DR_MC_ALL,
                      "signal mcontext flags cannot be changed");
        mcontext_to_sigcontext(raw_sc, dr_mcontext_as_priv_mcontext(si.raw_mcontext));
    }
    heap_free(dcontext, si.mcontext, sizeof(*si.mcontext) HEAPACCT(ACCT_OTHER));
    heap_free(dcontext, si.raw_mcontext, sizeof(*si.raw_mcontext) HEAPACCT(ACCT_OTHER));
    return action;
}

/* Returns false if caller should exit */
static bool
handle_client_action_from_cache(dcontext_t *dcontext, int sig, dr_signal_action_t action,
                                sigframe_rt_t *our_frame, struct sigcontext *sc_orig,
                                bool blocked)
{
    thread_sig_info_t *info = (thread_sig_info_t *) dcontext->signal_field;
    struct sigcontext *sc = get_sigcontext_from_rt_frame(our_frame);
    /* in order to pass to the client, we come all the way here for signals
     * the app has no handler for
     */
    if (action == DR_SIGNAL_REDIRECT) {
        /* send_signal_to_client copied mcontext into our
         * master_signal_handler frame, so we set up for fcache_return w/
         * the mcontext state and this as next_tag
         */
        sigcontext_to_mcontext(get_mcontext(dcontext), sc);
        dcontext->next_tag = (app_pc) sc->SC_XIP;
        sc->SC_XIP = (ptr_uint_t) fcache_return_routine(dcontext);
        sc->SC_XAX = (ptr_uint_t) get_sigreturn_linkstub();
        if (is_building_trace(dcontext)) {
            LOG(THREAD, LOG_ASYNCH, 3, "\tsquashing trace-in-progress\n");
            trace_abort(dcontext);
        }
        return false;
    }
    else if (action == DR_SIGNAL_SUPPRESS ||
             (!blocked && info->app_sigaction[sig] != NULL &&
              info->app_sigaction[sig]->handler == (handler_t)SIG_IGN)) {
        LOG(THREAD, LOG_ASYNCH, 2, "%s: not delivering!\n",
            (action == DR_SIGNAL_SUPPRESS) ?
            "client suppressing signal" :
            "app signal handler is SIG_IGN");
        /* restore original (untranslated) sc */
        our_frame->uc.uc_mcontext = *sc_orig;
        return false;
    }
    else if (!blocked && /* no BYPASS for blocked */
             (action == DR_SIGNAL_BYPASS ||
              (info->app_sigaction[sig] == NULL ||
               info->app_sigaction[sig]->handler == (handler_t)SIG_DFL))) {
        LOG(THREAD, LOG_ASYNCH, 2, "%s: executing default action\n",
            (action == DR_SIGNAL_BYPASS) ?
            "client forcing default" :
            "app signal handler is SIG_DFL");
        if (execute_default_from_cache(dcontext, sig, our_frame, sc_orig)) {
            /* if we haven't terminated, restore original (untranslated) sc
             * on request.
             */
            our_frame->uc.uc_mcontext = *sc_orig;
            LOG(THREAD, LOG_ASYNCH, 2, "%s: restored xsp="PFX", xip="PFX"\n",
                __FUNCTION__, our_frame->uc.uc_mcontext.SC_XSP,
                our_frame->uc.uc_mcontext.SC_XIP);
        }
        return false;
    }
    CLIENT_ASSERT(action == DR_SIGNAL_DELIVER, "invalid signal event return value");
    return true;
}
#endif

static void
abort_on_fault(dcontext_t *dcontext, uint dumpcore_flag, app_pc pc, struct sigcontext *sc,
               const char *prefix, const char *signame, const char *where)
{
    const char *fmt =
        "%s at PC "PFX"\n"
        "Received SIG%s at%s pc "PFX" in thread %d\n"
        "Base: "PFX"\n"
        "Registers: eax="PFX" ebx="PFX" ecx="PFX" edx="PFX"\n"
        "\tesi="PFX" edi="PFX" esp="PFX" ebp="PFX"\n"
#ifdef X64
        "\tr8 ="PFX" r9 ="PFX" r10="PFX" r11="PFX"\n"
        "\tr12="PFX" r13="PFX" r14="PFX" r15="PFX"\n"
#endif
        "\teflags="PFX;

    report_dynamorio_problem(dcontext, dumpcore_flag,
                             pc, (app_pc) sc->SC_XBP,
                             fmt, prefix, pc, signame, where, pc, get_thread_id(),
                             get_dynamorio_dll_start(),
                             sc->SC_XAX, sc->SC_XBX, sc->SC_XCX, sc->SC_XDX,
                             sc->SC_XSI, sc->SC_XDI, sc->SC_XSP, sc->SC_XBP,
#ifdef X64
                             sc->r8, sc->r9, sc->r10, sc->r11,
                             sc->r12, sc->r13, sc->r14, sc->r15,
#endif
                             sc->SC_XFLAGS);
    os_terminate(dcontext, TERMINATE_PROCESS);
    ASSERT_NOT_REACHED();
}

static void
abort_on_DR_fault(dcontext_t *dcontext, app_pc pc, struct sigcontext *sc,
                  const char *signame, const char *where)
{
    abort_on_fault(dcontext, DUMPCORE_INTERNAL_EXCEPTION, pc, sc, "Unrecoverable error",
                   signame, where);
    ASSERT_NOT_REACHED();
}

/* Returns whether unlinked or mangled syscall.
 * Restored in receive_pending_signal.
 */
static bool
unlink_fragment_for_signal(dcontext_t *dcontext, fragment_t *f,
                           byte *pc/*interruption pc*/)
{
    /* We only come here if we interrupted a fragment in the cache,
     * which means that this thread's DR state is safe, and so it
     * should be ok to acquire a lock.  xref PR 596069.
     *
     * There is a race where if two threads hit a signal in the same
     * shared fragment, the first could re-link after the second
     * un-links but before the second exits, and the second could then
     * execute the syscall, resulting in arbitrary delay prior to
     * signal delivery.  We don't want to allocate global memory,
     * but we could use a static array of counters (since should
     * be small # of interrupted shared fragments at any one time)
     * used as refcounts so we only unlink when all are done.
     * Not bothering to implement now: going to live w/ chance of
     * long signal delays.  xref PR 596069.
     */
    bool changed = false;
    /* may not be linked if trace_relink or something */
    if (TEST(FRAG_COARSE_GRAIN, f->flags)) {
        /* XXX PR 213040: we don't support unlinking coarse, so we try
         * not to come here, but for indirect branch and other spots
         * where we don't yet support translation (since can't fault)
         * we end up w/ no bound on delivery...
         */
    } else if (TEST(FRAG_LINKED_OUTGOING, f->flags)) {
        LOG(THREAD, LOG_ASYNCH, 3,
            "\tunlinking outgoing for interrupted F%d\n", f->id);
        SHARED_FLAGS_RECURSIVE_LOCK(f->flags, acquire, change_linking_lock);
        unlink_fragment_outgoing(dcontext, f);
        SHARED_FLAGS_RECURSIVE_LOCK(f->flags, release, change_linking_lock);
        changed = true;
    } else {
        LOG(THREAD, LOG_ASYNCH, 3,
            "\toutgoing already unlinked for interrupted F%d\n", f->id);
    }
    if (TEST(FRAG_HAS_SYSCALL, f->flags)) {
        /* Syscalls are signal barriers!
         * Make sure the next syscall (if any) in f is not executed!
         * instead go back to dispatch right before the syscall
         */
        /* syscall mangling does a bunch of decodes but only one write,
         * changing the target of a short jmp, which is atomic
         * since a one-byte write, so we don't need the change_linking_lock.
         */
        changed = changed ||
            mangle_syscall_code(dcontext, f, pc, false/*do not skip exit cti*/);
    }
    return changed;
}

static bool
interrupted_inlined_syscall(dcontext_t *dcontext, fragment_t *f,
                            byte *pc/*interruption pc*/)
{
    bool pre_or_post_syscall = false;
    if (TEST(FRAG_HAS_SYSCALL, f->flags)) {
        /* PR 596147: if the thread is currently in an inlined
         * syscall when a signal comes in, we can't delay and bound the
         * delivery time: we need to deliver now.  Should decode
         * backward and see if syscall.  We assume our translation of
         * the interruption state is fine to re-start: i.e., the syscall
         * is complete if kernel has pc at post-syscall point, and
         * kernel set EINTR in eax if necessary.
         */
        /* Interrupted fcache, so ok to alloc memory for decode */
        instr_t instr;
        byte *nxt_pc;
        instr_init(dcontext, &instr);
        nxt_pc = decode(dcontext, pc, &instr);
        if (nxt_pc != NULL && instr_valid(&instr) &&
            instr_is_syscall(&instr)) {
            /* pre-syscall but post-jmp so can't skip syscall */
            pre_or_post_syscall = true;
        } else {
            instr_reset(dcontext, &instr);
            ASSERT(INT_LENGTH == SYSCALL_LENGTH);
            ASSERT(SYSENTER_LENGTH == SYSCALL_LENGTH);
            nxt_pc = decode(dcontext, pc - SYSCALL_LENGTH, &instr);
            if (nxt_pc != NULL && instr_valid(&instr) &&
                instr_is_syscall(&instr)) {
                /* decoding backward so check for exit cti jmp prior
                 * to syscall to ensure no mismatch
                 */
                instr_reset(dcontext, &instr);
                nxt_pc = decode(dcontext, pc - SYSCALL_LENGTH - JMP_LONG_LENGTH, &instr);
                if (nxt_pc != NULL && instr_valid(&instr) &&
                    instr_get_opcode(&instr) == OP_jmp) {
                    /* post-inlined-syscall */
                    pre_or_post_syscall = true;
                }
            }
        }
        instr_free(dcontext, &instr);
    }
    return pre_or_post_syscall;
}

bool
sysnum_is_not_restartable(int sysnum)
{
    /* Check the list of non-restartable syscalls.
     * We're missing:
     * + SYS_read from an inotify file descriptor.
     * We're overly aggressive on:
     * + Socket interfaces: supposed to restart if no timeout has been set
     *   but we never restart for simplicity for now.
     */
    return (sysnum == SYS_pause || sysnum == SYS_rt_sigsuspend ||
            sysnum == SYS_rt_sigtimedwait || IF_X64(sysnum == SYS_epoll_wait_old ||)
            sysnum == SYS_epoll_wait || sysnum == SYS_epoll_pwait ||
            sysnum == SYS_poll || sysnum == SYS_ppoll ||
            sysnum == SYS_select || sysnum == SYS_pselect6 ||
#ifdef X64
            sysnum == SYS_msgrcv || sysnum == SYS_msgsnd || sysnum == SYS_semop ||
            sysnum == SYS_semtimedop ||
            /* XXX: these should be restarted if there's no timeout */
            sysnum == SYS_accept || sysnum == SYS_accept4 ||
            sysnum == SYS_recvfrom || sysnum == SYS_recvmsg || sysnum == SYS_recvmmsg ||
            sysnum == SYS_connect || sysnum == SYS_sendto ||
            sysnum == SYS_sendmmsg || sysnum == SYS_sendfile ||
#else
            /* XXX: some should be restarted if there's no timeout */
            sysnum == SYS_ipc ||
#endif
            sysnum == SYS_clock_nanosleep || sysnum == SYS_nanosleep ||
            sysnum == SYS_io_getevents);
}

/* i#1145: auto-restart syscalls interrupted by signals */
static bool
adjust_syscall_for_restart(dcontext_t *dcontext, thread_sig_info_t *info, int sig,
                           struct sigcontext *sc, fragment_t *f)
{
    byte *pc = (byte *) sc->SC_XIP;
    instr_t instr;

    if (sc->SC_XAX != -EINTR) {
        /* The syscall succeeded, so no reason to interrupt.
         * Some syscalls succeed on a signal coming in.
         * E.g., SYS_wait4 on SIGCHLD, or reading from a slow device.
         */
        return false;
    }
    /* Don't restart if the app's handler says not to */
    if (info->app_sigaction[sig] != NULL &&
        !TEST(SA_RESTART, info->app_sigaction[sig]->flags)) {
        return false;
    }
    /* XXX i#1145: some syscalls are never restarted when interrupted by a signal.
     * We check those that are simple to distinguish below, but not all are.  We have
     * this under an option so it can be disabled if necessary.
     */
    if (!DYNAMO_OPTION(restart_syscalls))
        return false;

    /* The kernel has already put -EINTR into eax, so we must
     * restore the syscall number.  We assume no other register or
     * memory values have been clobbered from their pre-syscall
     * values.
     */
    int sysnum = -1;
    if (f != NULL) {
        /* Inlined syscall.  I'd use find_syscall_num() but we'd need to call
         * decode_fragment() and tweak find_syscall_num() to handle the skip-syscall
         * jumps, or grab locks and call recreate_fragment_ilist() -- both are
         * heavyweight, so we do our own decode loop.
         * We assume we'll find a mov-imm b/c otherwise we wouldn't have inlined this.
         */
        LOG(THREAD, LOG_ASYNCH, 3, "%s: decoding to find syscall #\n", __FUNCTION__);
        instr_init(dcontext, &instr);
        pc = FCACHE_ENTRY_PC(f);
        do {
            DOLOG(3, LOG_ASYNCH, {
                disassemble_with_bytes(dcontext, pc, THREAD);
            });
            instr_reset(dcontext, &instr);
            pc = decode(dcontext, pc, &instr);
            if (instr_get_opcode(&instr) == OP_mov_imm &&
                opnd_is_reg(instr_get_dst(&instr, 0)) &&
                opnd_get_reg(instr_get_dst(&instr, 0)) == REG_EAX &&
                opnd_is_immed_int(instr_get_src(&instr, 0))) {
                sysnum = (int) opnd_get_immed_int(instr_get_src(&instr, 0));
                /* don't break: find last one before syscall */
            }
        } while (pc != NULL && instr_valid(&instr) && !instr_is_syscall(&instr) &&
                 pc < FCACHE_ENTRY_PC(f) + f->size);
        instr_free(dcontext, &instr);
        ASSERT(DYNAMO_OPTION(ignore_syscalls));
        ASSERT(sysnum > -1);
   } else {
        /* do_syscall => eax should be in mcontext */
        sysnum = (int) get_mcontext(dcontext)->xax;
    }
    LOG(THREAD, LOG_ASYNCH, 2, "%s: syscall # is %d\n", __FUNCTION__, sysnum);
    if (sysnum_is_not_restartable(sysnum)) {
        LOG(THREAD, LOG_ASYNCH, 2, "%s: syscall is non-restartable\n", __FUNCTION__);
        return false;
    }
    sc->SC_XAX = sysnum;

    /* Now adjust the pc to point at the syscall instruction instead of after it,
     * so when we resume we'll go back to the syscall.
     *
     * XXX: this is a transparency issue: the app might expect a pc after the
     * syscall.  We live with it for now.
     */
    ASSERT(INT_LENGTH == SYSCALL_LENGTH &&
           INT_LENGTH == SYSENTER_LENGTH);
    if (pc == vsyscall_sysenter_return_pc) {
        sc->SC_XIP = (ptr_uint_t) (vsyscall_syscall_end_pc - SYSENTER_LENGTH);
        /* To restart sysenter we must re-copy xsp into xbp, as xbp is
         * clobbered by the kernel.
         */
        sc->SC_XBP = sc->SC_XSP;
    } else if (is_after_syscall_address(dcontext, pc)) {
        /* We're at do_syscall: point at app syscall instr */
        sc->SC_XIP = (ptr_uint_t) (dcontext->asynch_target - INT_LENGTH);
        DODEBUG({
            instr_init(dcontext, &instr);
            ASSERT(decode(dcontext, (app_pc) sc->SC_XIP, &instr) != NULL &&
                   instr_is_syscall(&instr));
            instr_free(dcontext, &instr);
        });
    } else {
        instr_init(dcontext, &instr);
        pc = decode(dcontext, pc - INT_LENGTH, &instr);
        if (instr_is_syscall(&instr))
            sc->SC_XIP -= INT_LENGTH;
        else
            ASSERT_NOT_REACHED();
        instr_free(dcontext, &instr);
    }
    LOG(THREAD, LOG_ASYNCH, 2, "%s: sigreturn pc is now "PFX"\n", __FUNCTION__,
        sc->SC_XIP);
    return true;
}

static void
record_pending_signal(dcontext_t *dcontext, int sig, kernel_ucontext_t *ucxt,
                      sigframe_rt_t *frame, bool forged
                      _IF_CLIENT(byte *access_address))
{
    thread_sig_info_t *info = (thread_sig_info_t *) dcontext->signal_field;
    os_thread_data_t *ostd = (os_thread_data_t *) dcontext->os_field;
    struct sigcontext *sc = (struct sigcontext *) &(ucxt->uc_mcontext);
    struct sigcontext sc_orig;
    byte *pc = (byte *) sc->SC_XIP;
    byte *xsp = (byte*) sc->SC_XSP;
    bool receive_now = false;
    bool blocked = false;
    bool handled = false;
    bool at_syscall = false;
    sigpending_t *pend;
    fragment_t *f = NULL;
    fragment_t wrapper;

    /* We no longer block SUSPEND_SIGNAL (i#184/PR 450670) or SIGSEGV (i#193/PR 287309).
     * But we can have re-entrancy issues in this routine if the app uses the same
     * SUSPEND_SIGNAL, or the nested SIGSEGV needs to be sent to the app.  The
     * latter shouldn't happen unless the app sends SIGSEGV via SYS_kill().
     */
    if (ostd->processing_signal > 0 ||
        /* If we interrupted receive_pending_signal() we can't prepend a new
         * pending or delete an old b/c we might mess up the state so we
         * just drop this one: should only happen for alarm signal
         */
        (info->accessing_sigpending &&
         /* we do want to report a crash in receive_pending_signal() */
         (can_always_delay[sig] ||
          is_sys_kill(dcontext, pc, (byte*)sc->SC_XSP, &frame->info)))) {
        LOG(THREAD, LOG_ASYNCH, 1, "nested signal %d\n", sig);
        ASSERT(ostd->processing_signal == 0 || sig == SUSPEND_SIGNAL || sig == SIGSEGV);
        ASSERT(can_always_delay[sig] ||
               is_sys_kill(dcontext, pc, (byte*)sc->SC_XSP, &frame->info));
        /* To avoid re-entrant execution of special_heap_alloc() and of
         * prepending to the pending list we just drop this signal.
         * FIXME i#194/PR 453996: do better.
         */
        STATS_INC(num_signals_dropped);
        SYSLOG_INTERNAL_WARNING_ONCE("dropping nested signal");
        return;
    }
    ostd->processing_signal++; /* no need for atomicity: thread-private */

    if (info->in_sigsuspend) {
        /* sigsuspend ends when a signal is received, so restore the
         * old blocked set
         */
        info->app_sigblocked = info->app_sigblocked_save;
        info->in_sigsuspend = false;
#ifdef DEBUG
        if (stats->loglevel >= 3 && (stats->logmask & LOG_ASYNCH) != 0) {
            LOG(THREAD, LOG_ASYNCH, 3, "after sigsuspend, blocked signals are now:\n");
            dump_sigset(dcontext, &info->app_sigblocked);
        }
#endif
    }

    if (info->app_sigaction[sig] != NULL &&
        info->app_sigaction[sig]->handler == (handler_t)SIG_IGN
        /* If a client registered a handler, put this in the queue.
         * Races between registering, queueing, and delivering are fine.
         */
        IF_CLIENT_INTERFACE(&& !dr_signal_hook_exists())) {
        LOG(THREAD, LOG_ASYNCH, 3,
            "record_pending_signal (%d at pc "PFX"): action is SIG_IGN!\n",
            sig, pc);
        ostd->processing_signal--;
        return;
    } else if (kernel_sigismember(&info->app_sigblocked, sig)) {
        /* signal is blocked by app, so just record it, don't receive now */
        LOG(THREAD, LOG_ASYNCH, 2,
            "record_pending_signal(%d at pc "PFX"): signal is currently blocked\n",
            sig, pc);
        blocked = true;
        handled = notify_signalfd(dcontext, info, sig, frame);
    } else if (safe_is_in_fcache(dcontext, pc, xsp)) {
        LOG(THREAD, LOG_ASYNCH, 2,
            "record_pending_signal(%d) from cache pc "PFX"\n", sig, pc);
        if (forged || can_always_delay[sig]) {
            /* to make translation easier, want to delay if can until dispatch
             * unlink cur frag, wait for dispatch
             */
            /* check for coarse first to avoid cost of coarse pclookup */
            if (get_fcache_coarse_info(pc) != NULL) {
                /* PR 213040: we can't unlink coarse.  If we fail to translate
                 * we'll switch back to delaying, below.
                 */
                if (sig_is_alarm_signal(sig) && 
                    info->sigpending[sig] != NULL &&
                    info->sigpending[sig]->next != NULL &&
                    info->skip_alarm_xl8 > 0) {
                    /* Translating coarse fragments is very expensive so we
                     * avoid doing it when we're having trouble keeping up w/
                     * the alarm frequency (PR 213040), but we make sure we try
                     * every once in a while to avoid unbounded signal delay
                     */
                    info->skip_alarm_xl8--;
                    STATS_INC(num_signals_coarse_delayed);
                } else {
                    if (sig_is_alarm_signal(sig))
                        info->skip_alarm_xl8 = SKIP_ALARM_XL8_MAX;
                    receive_now = true;
                    LOG(THREAD, LOG_ASYNCH, 2,
                        "signal interrupted coarse fragment so delivering now\n");
                }
            } else {
                f = fragment_pclookup(dcontext, pc, &wrapper);
                ASSERT(f != NULL);
                ASSERT(!TEST(FRAG_COARSE_GRAIN, f->flags)); /* checked above */
                LOG(THREAD, LOG_ASYNCH, 2, "\tdelaying until exit F%d\n", f->id);
                if (interrupted_inlined_syscall(dcontext, f, pc)) {
                    /* PR 596147: if delayable signal arrives after syscall-skipping
                     * jmp, either at syscall or post-syscall, we deliver
                     * immediately, since we can't bound the delay
                     */
                    receive_now = true;
                    LOG(THREAD, LOG_ASYNCH, 2,
                        "signal interrupted pre/post syscall itself so delivering now\n");
                    at_syscall = true;
                } else {
                    /* could get another signal but should be in same fragment */
                    ASSERT(info->interrupted == NULL || info->interrupted == f);
                    if (unlink_fragment_for_signal(dcontext, f, pc)) {
                        info->interrupted = f;
                        info->interrupted_pc = pc;
                    } else {
                        /* either was unlinked for trace creation, or we got another
                         * signal before exiting cache to handle 1st
                         */
                        ASSERT(info->interrupted == NULL ||
                               info->interrupted == f);
                    }
                }
            }
        } else {
            /* the signal interrupted code cache => run handler now! */
            receive_now = true;
            LOG(THREAD, LOG_ASYNCH, 2, "\tnot certain can delay so handling now\n");
        }
    } else if (in_generated_routine(dcontext, pc) ||
               /* XXX: should also check fine stubs */
               safe_is_in_coarse_stubs(dcontext, pc, xsp)) {
        /* Assumption: dynamo errors have been caught already inside
         * the master_signal_handler, thus any error in a generated routine
         * is an asynch signal that can be delayed
         */
        /* FIXME: dispatch on routine:
         * if fcache_return, treat as dynamo
         * if fcache_enter, unlink next frag, treat as dynamo
         *   what if next frag has syscall in it?
         * if indirect_branch_lookup prior to getting target...?!?
         */
        LOG(THREAD, LOG_ASYNCH, 2,
            "record_pending_signal(%d) from gen routine or stub "PFX"\n", sig, pc);
        /* i#1206: the syscall was interrupted, so we can go back to dispatch
         * and don't need to receive it now (which complicates post-syscall handling)
         * w/o any extra delay.
         */
        at_syscall = is_after_syscall_address(dcontext, pc);
        /* This could come from another thread's SYS_kill (via our gen do_syscall) */
        DOLOG(1, LOG_ASYNCH, {
            if (!is_after_syscall_address(dcontext, pc) &&
                !forged && !can_always_delay[sig]) {
                LOG(THREAD, LOG_ASYNCH, 1,
                    "WARNING: signal %d in gen routine: may cause problems!\n", sig);
            }
        });
    } else if (pc == vsyscall_sysenter_return_pc) {
        LOG(THREAD, LOG_ASYNCH, 2,
            "record_pending_signal(%d) from vsyscall "PFX"\n", sig, pc);
        /* i#1206: the syscall was interrupted, so we can go back to dispatch
         * and don't need to receive it now (which complicates post-syscall handling)
         */
        at_syscall = true;
    } else {
        /* the signal interrupted dynamo => do not run handler now! */
        LOG(THREAD, LOG_ASYNCH, 2,
            "record_pending_signal(%d) from dynamo or lib at pc "PFX"\n", sig, pc);
        if (!forged &&
            !can_always_delay[sig] &&
            !is_sys_kill(dcontext, pc, (byte*)sc->SC_XSP, &frame->info)) {
            /* i#195/PR 453964: don't re-execute if will just re-fault.
             * Our checks for dstack, etc. in master_signal_handler should
             * have accounted for everything
             */
            ASSERT_NOT_REACHED();
            abort_on_DR_fault(dcontext, pc, sc,
                              (sig == SIGSEGV) ? "SEGV" : "other", "unknown");
        }
    }

    LOG(THREAD, LOG_ASYNCH, 3, "\taction is not SIG_IGN\n");
    LOG(THREAD, LOG_ASYNCH, 3, "\tretaddr = "PFX"\n",
        frame->pretcode); /* pretcode has same offs for plain */

    if (receive_now) {
        /* we need to translate sc before we know whether client wants to
         * suppress, so we need a backup copy
         */
        bool xl8_success;

        /* i#1145: update the context for an auto-restart syscall
         * before we make the sc_orig copy or translate.
         */
        if (at_syscall)
            adjust_syscall_for_restart(dcontext, info, sig, sc, f);

        sc_orig = *sc;
        ASSERT(!forged);
        /* cache the fragment since pclookup is expensive for coarse (i#658) */
        f = fragment_pclookup(dcontext, (cache_pc)sc->SC_XIP, &wrapper);
        xl8_success = translate_sigcontext(dcontext, sc, !can_always_delay[sig], f);

        if (can_always_delay[sig] && !xl8_success) {
            /* delay: we expect this for coarse fragments if alarm arrives
             * in middle of ind branch region or sthg (PR 213040)
             */
            LOG(THREAD, LOG_ASYNCH, 2,
                "signal is in un-translatable spot in coarse fragment: delaying\n");
            receive_now = false;
        }
    }

    if (receive_now) {

        /* N.B.: since we abandon the old context for synchronous signals,
         * we do not need to mark this fragment as FRAG_CANNOT_DELETE
         */
#ifdef DEBUG
        if (stats->loglevel >= 2 && (stats->logmask & LOG_ASYNCH) != 0 &&
            safe_is_in_fcache(dcontext, pc, xsp)) {
            ASSERT(f != NULL);
            LOG(THREAD, LOG_ASYNCH, 2,
                "Got signal at pc "PFX" in this fragment:\n", pc);
            disassemble_fragment(dcontext, f, false);
        }
#endif

        LOG(THREAD, LOG_ASYNCH, 2, "Going to receive signal now\n");
        /* If we end up executing the default action, we'll go native
         * since we translated the context.  If there's a handler,
         * we'll copy the context to the app stack and then adjust the
         * original on our stack so we take over.
         */
        execute_handler_from_cache(dcontext, sig, frame, &sc_orig, f
                                   _IF_CLIENT(access_address));

    } else if (!handled) {

#ifdef CLIENT_INTERFACE
        /* i#182/PR 449996: must let client act on blocked non-delayable signals to
         * handle instrumentation faults.  Make sure we're at a safe spot: i.e.,
         * only raise for in-cache faults.  Checking forged and no-delay
         * to avoid the in-cache check for delayable signals => safer.
         */
        if (blocked && !forged && !can_always_delay[sig] &&
            safe_is_in_fcache(dcontext, pc, xsp)) {
            dr_signal_action_t action;
            /* cache the fragment since pclookup is expensive for coarse (i#658) */
            f = fragment_pclookup(dcontext, (cache_pc)sc->SC_XIP, &wrapper);
            sc_orig = *sc;
            translate_sigcontext(dcontext, sc, true/*shouldn't fail*/, f);
            action = send_signal_to_client(dcontext, sig, frame, &sc_orig,
                                           access_address, true/*blocked*/, f);
            /* For blocked signal early event we disallow BYPASS (xref i#182/PR 449996) */
            CLIENT_ASSERT(action != DR_SIGNAL_BYPASS,
                          "cannot bypass a blocked signal event");
            if (!handle_client_action_from_cache(dcontext, sig, action, frame,
                                                 &sc_orig, true/*blocked*/)) {
                ostd->processing_signal--;
                return;
            }
            /* restore original (untranslated) sc */
            frame->uc.uc_mcontext = sc_orig;
        }
#endif

        /* i#196/PR 453847: avoid infinite loop of signals if try to re-execute */
        if (blocked && !forged && !can_always_delay[sig] &&
            !is_sys_kill(dcontext, pc, (byte*)sc->SC_XSP, &frame->info)) {
            ASSERT(default_action[sig] == DEFAULT_TERMINATE ||
                   default_action[sig] == DEFAULT_TERMINATE_CORE);
            LOG(THREAD, LOG_ASYNCH, 1,
                "blocked fatal signal %d cannot be delayed: terminating\n", sig);
            sc_orig = *sc;
            translate_sigcontext(dcontext, sc, true/*shouldn't fail*/, NULL);
            /* the process should be terminated */
            execute_default_from_cache(dcontext, sig, frame, &sc_orig);
            ASSERT_NOT_REACHED();
        }
        
        /* Happened in DR, do not translate context.  Record for later processing
         * at a safe point with a clean app state.
         */
        if (!blocked || sig >= OFFS_RT ||
            (blocked && info->sigpending[sig] == NULL)) {
            /* only have 1 pending for blocked non-rt signals */

            /* special heap alloc always uses sizeof(sigpending_t) blocks */
            pend = special_heap_alloc(info->sigheap);
            ASSERT(sig > 0 && sig <= MAX_SIGNUM);

            /* to avoid accumulating signals if we're slow in presence of
             * a high-rate itimer we only keep 2 alarm signals (PR 596768)
             */
            if (sig_is_alarm_signal(sig)) {
                if (info->sigpending[sig] != NULL &&
                    info->sigpending[sig]->next != NULL) {
                    ASSERT(info->sigpending[sig]->next->next == NULL);
                    /* keep the oldest, replace newer w/ brand-new one, for
                     * more spread-out alarms
                     */
                     sigpending_t *temp = info->sigpending[sig];
                     info->sigpending[sig] = temp->next;
                     special_heap_free(info->sigheap, temp);
                     LOG(THREAD, LOG_ASYNCH, 2,
                         "3rd pending alarm %d => dropping 2nd\n", sig);
                     STATS_INC(num_signals_dropped);
                     SYSLOG_INTERNAL_WARNING_ONCE("dropping 3rd pending alarm signal");
                }
            }

            pend->next = info->sigpending[sig];
            info->sigpending[sig] = pend;

            /* FIXME: note that for asynchronous signals we don't need to
             *  bother to record exact machine context, even entire frame,
             *  since don't want to pass dynamo pc context to app handler.
             *  only copy frame for synchronous signals?  those only
             *  happen while in cache?  but for asynch, we would have to
             *  construct our own frame...kind of a pain.  
             */
            copy_frame_to_pending(dcontext, sig, frame _IF_CLIENT(access_address));

            /* i#1145: check whether we should auto-restart an interrupted syscall */
            if (at_syscall) {
                /* Adjust the pending frame to restart the syscall, if applicable */
                sigframe_rt_t *frame = &(info->sigpending[sig]->rt_frame);
                struct sigcontext *sc_pend = get_sigcontext_from_rt_frame(frame);
                if (adjust_syscall_for_restart(dcontext, info, sig, sc_pend, f)) {
                    /* We're going to re-start this syscall after we go
                     * back to dispatch, run the post-syscall handler (for -EINTR),
                     * and deliver the signal.  We've adjusted the sigcontext
                     * for re-start on the sigreturn, but we need to tell
                     * execute_handler_from_dispatch() to use our sigcontext
                     * and not the mcontext.
                     * A client will see a second set of pre + post handlers for
                     * the restart, which seems reasonable, given the signal in
                     * between.
                     */
                    info->sigpending[sig]->use_sigcontext = true;
                }
            }

       } else {
            /* For clients, we document that we do not pass to them
             * unless we're prepared to deliver to app.  We would have
             * to change our model to pass them non-final-translated
             * contexts for delayable signals in order to give them
             * signals as soon as they come in.  Xref i#182/PR 449996.
             */
            LOG(THREAD, LOG_ASYNCH, 3,
                "\tnon-rt signal already in queue, ignoring this one!\n");
        }

        if (!blocked)
            dcontext->signals_pending = true;
    }
    ostd->processing_signal--;
}

/* Distinguish SYS_kill-generated from instruction-generated signals.
 * If sent from another process we can't tell, but if sent from this
 * thread the interruption point should be our own post-syscall.
 * FIXME PR 368277: for other threads in same process we should set a flag
 * and identify them as well.
 * FIXME: for faults like SIGILL we could examine the interrupted pc
 * to see whether it is capable of generating such a fault (see code
 * used in handle_nudge_signal()).
 */
static bool
is_sys_kill(dcontext_t *dcontext, byte *pc, byte *xsp, siginfo_t *info)
{
#ifndef VMX86_SERVER /* does not properly set si_code */
    /* i#133: use si_code to distinguish user-sent signals.
     * Even 2.2 Linux kernel supports <=0 meaning user-sent (except
     * SIGIO) so we assume we can rely on it.
     */
    if (info->si_code <= 0)
        return true;
#endif
    return (is_at_do_syscall(dcontext, pc, xsp) &&
            (dcontext->sys_num == SYS_kill ||
             dcontext->sys_num == SYS_tkill ||
             dcontext->sys_num == SYS_tgkill ||
             dcontext->sys_num == SYS_rt_sigqueueinfo));
}

static byte *
compute_memory_target(dcontext_t *dcontext, cache_pc instr_cache_pc,
                      struct sigcontext *sc, siginfo_t *si, bool *write)
{
    byte *target = NULL;
    instr_t instr;
    priv_mcontext_t mc;
    uint memopidx, memoppos, memopsize;
    opnd_t memop;
    bool found_target = false;
    bool in_maps;
    bool use_allmem = false;
    uint prot;

    LOG(THREAD, LOG_ALL, 2,
        "computing memory target for "PFX" causing SIGSEGV, kernel claims it is "PFX"\n",
        instr_cache_pc, (byte*)si->si_addr);

    /* We used to do a memory query to check if instr_cache_pc is readable, but
     * now we use TRY/EXCEPT because we don't have the instr length and the OS
     * query is expensive.  If decoding faults, the signal handler will longjmp
     * out before it calls us recursively.
     */
    instr_init(dcontext, &instr);
    TRY_EXCEPT(dcontext, {
        decode(dcontext, instr_cache_pc, &instr);
    }, {
        return NULL;  /* instr_cache_pc was unreadable */
    });

    if (!instr_valid(&instr)) {
        LOG(THREAD, LOG_ALL, 2,
            "WARNING: got SIGSEGV for invalid instr at cache pc "PFX"\n", instr_cache_pc);
        ASSERT_NOT_REACHED();
        instr_free(dcontext, &instr);
        return NULL;
    }

    sigcontext_to_mcontext(&mc, sc);
    ASSERT(write != NULL);

    /* i#1009: If si_addr is plausibly one of the memory operands of the
     * faulting instruction, assume the target was si_addr.  If none of the
     * memops match, fall back to checking page protections, which can be racy.
     * For si_addr == NULL, we fall back to the protection check because it's
     * too likely to be a valid memop and we can live with a race on a page that
     * is typically unmapped.
     */
    if (si->si_code == SEGV_ACCERR && si->si_addr != NULL) {
        for (memopidx = 0;
             instr_compute_address_ex_priv(&instr, &mc, memopidx,
                                           &target, write, &memoppos);
             memopidx++) {
            /* i#1045: check whether operand and si_addr overlap */
            memop = *write ? instr_get_dst(&instr, memoppos) :
                instr_get_src(&instr, memoppos);
            memopsize = opnd_size_in_bytes(opnd_get_size(memop));
            LOG(THREAD, LOG_ALL, 2,
                "memory operand %u has address "PFX" and size %u\n",
                memopidx, target, memopsize);
            if ((byte*)si->si_addr >= target &&
                (byte*)si->si_addr < target + memopsize) {
                target = (byte*)si->si_addr;
                found_target = true;
                break;
            }
        }
    }

    /* For fcache faults, use all_memory_areas, which is faster but acquires
     * locks.  If it's possible we're in DR, go to the OS to avoid deadlock.
     */
    if (DYNAMO_OPTION(use_all_memory_areas)) {
        use_allmem = safe_is_in_fcache(dcontext, instr_cache_pc,
                                       (byte *)sc->SC_XSP);
    }
    if (!found_target) {
        if (si->si_addr != NULL) {
            LOG(THREAD, LOG_ALL, 3,
                "%s: falling back to racy protection checks\n", __FUNCTION__);
        }
        /* i#115/PR 394984: consider all memops */
        for (memopidx = 0;
             instr_compute_address_ex_priv(&instr, &mc, memopidx,
                                           &target, write, NULL);
             memopidx++) {
            if (use_allmem) {
                in_maps = get_memory_info(target, NULL, NULL, &prot);
            } else {
                in_maps = get_memory_info_from_os(target, NULL, NULL, &prot);
            }
            if ((!in_maps || !TEST(MEMPROT_READ, prot)) ||
                (*write && !TEST(MEMPROT_WRITE, prot))) {
                found_target = true;
                break;
            }
        }
    }

    if (!found_target) {
        /* probably an NX fault: how tell whether kernel enforcing? */
        in_maps = get_memory_info_from_os(instr_cache_pc, NULL, NULL, &prot);
        if (!in_maps || !TEST(MEMPROT_EXEC, prot)) {
            target = instr_cache_pc;
            found_target = true;
        }
    }

    /* we may still not find target, e.g. for SYS_kill(SIGSEGV) */
    if (!found_target)
        target = NULL;
    DOLOG(2, LOG_ALL, {
        LOG(THREAD, LOG_ALL, 2,
            "For SIGSEGV at cache pc "PFX", computed target %s "PFX"\n",
            instr_cache_pc, *write ? "write" : "read", target);
        loginst(dcontext, 2, &instr, "\tfaulting instr");
    });
    instr_free(dcontext, &instr);
    return target;
}

static bool
check_for_modified_code(dcontext_t *dcontext, cache_pc instr_cache_pc,
                        struct sigcontext *sc, byte *target)
{
    /* special case: we expect a seg fault for executable regions
     * that were writable and marked read-only by us.
     * have to figure out the target address!
     * unfortunately the OS doesn't tell us, nor whether it's a write.
     * FIXME: if sent from SYS_kill(SIGSEGV), the pc will be post-syscall,
     * and if that post-syscall instr is a write that could have faulted,
     * how can we tell the difference?
     */
    if (was_executable_area_writable(target)) {
        /* translate instr_cache_pc to original app pc
         * DO NOT use translate_sigcontext, don't want to change the
         * signal frame or else we'll lose control when we try to
         * return to signal pc!
         */
        app_pc next_pc, translated_pc;
        fragment_t *f = NULL;
        fragment_t wrapper;
        ASSERT((cache_pc)sc->SC_XIP == instr_cache_pc);
        /* For safe recreation we need to either be couldbelinking or hold 
         * the initexit lock (to keep someone from flushing current 
         * fragment), the initexit lock is easier
         */
        mutex_lock(&thread_initexit_lock);
        /* cache the fragment since pclookup is expensive for coarse units (i#658) */
        f = fragment_pclookup(dcontext, instr_cache_pc, &wrapper);
        translated_pc = recreate_app_pc(dcontext, instr_cache_pc, f);
        ASSERT(translated_pc != NULL);
        mutex_unlock(&thread_initexit_lock);
        next_pc =
            handle_modified_code(dcontext, instr_cache_pc, translated_pc,
                                 target, f);

        /* going to exit from middle of fragment (at the write) so will mess up
         * trace building
         */
        if (is_building_trace(dcontext)) {
            LOG(THREAD, LOG_ASYNCH, 3, "\tsquashing trace-in-progress\n");
            trace_abort(dcontext);
        }

        if (next_pc == NULL) {
            /* re-execute the write -- just have master_signal_handler return */
            return true;
        } else {
            /* Do not resume execution in cache, go back to dispatch.
             * Set our sigreturn context to point to fcache_return!
             * Then we'll go back through kernel, appear in fcache_return,
             * and go through dispatch & interp, without messing up dynamo stack.
             * Note that even if this is a write in the shared cache, we
             * still go to the private fcache_return for simplicity.
             */
            sc->SC_XIP = (ptr_uint_t) fcache_return_routine(dcontext);
#ifdef X64
            /* x64 always uses shared gencode */
            get_local_state_extended()->spill_space.xax = sc->SC_XAX;
#else
            get_mcontext(dcontext)->xax = sc->SC_XAX;
#endif
            LOG(THREAD, LOG_ASYNCH, 2, "\tsaved xax "PFX"\n", sc->SC_XAX);
            sc->SC_XAX = (ptr_uint_t) get_selfmod_linkstub();
            /* fcache_return will save rest of state */
            dcontext->next_tag = next_pc;
            LOG(THREAD, LOG_ASYNCH, 2,
                "\tset next_tag to "PFX", resuming in fcache_return\n",
                next_pc);
            /* now have master_signal_handler return */
            return true;
        }
    }
    return false;
}

#ifndef HAVE_SIGALTSTACK
/* The exact layout of this struct is relied on in master_signal_handler()
 * in x86.asm.
 */
struct clone_and_swap_args {
    byte *stack;
    byte *tos;
};

/* Helper function for swapping handler to dstack */
bool
sig_should_swap_stack(struct clone_and_swap_args *args, kernel_ucontext_t *ucxt)
{
    byte *cur_esp;
    dcontext_t *dcontext = get_thread_private_dcontext();
    if (dcontext == NULL)
        return false;
    GET_STACK_PTR(cur_esp);
    if (!is_on_dstack(dcontext, cur_esp)) {
        struct sigcontext *sc = (struct sigcontext *) &(ucxt->uc_mcontext);
        /* Pass back the proper args to clone_and_swap_stack: we want to
         * copy to dstack from the tos at the signal interruption point.
         */
        args->stack = dcontext->dstack;
        /* leave room for fpstate */
        args->stack -= XSTATE_FRAME_EXTRA;
        args->stack = (byte *) ALIGN_BACKWARD(args->stack, XSTATE_ALIGNMENT);
        args->tos = (byte *) sc->SC_XSP;
        return true;
    } else
        return false;
}
#endif

/* Helper that takes over the current thread signaled via SUSPEND_SIGNAL.  Kept
 * separate mostly to keep the priv_mcontext_t allocation out of
 * master_signal_handler_C.
 */
static void
sig_take_over(struct sigcontext *sc)
{
    priv_mcontext_t mc;
    sigcontext_to_mcontext(&mc, sc);
    os_thread_take_over(&mc);
    ASSERT_NOT_REACHED();
}

static bool
is_safe_read_ucxt(kernel_ucontext_t *ucxt)
{
    app_pc pc = (app_pc)ucxt->uc_mcontext.SC_XIP;
    return is_safe_read_pc(pc);
}

/* the master signal handler
 * WARNING: behavior varies with different versions of the kernel!
 * sigaction support was only added with 2.2
 */
#ifdef X64
/* stub in x86.asm passes our xsp to us */
void
master_signal_handler_C(int sig, siginfo_t *siginfo, kernel_ucontext_t *ucxt,
                        byte *xsp)
#else
/* On ia32, adding a parameter disturbs the frame we're trying to capture, so we
 * add an intermediate frame and read the normal params off the stack directly.
 */
void
master_signal_handler_C(byte *xsp)
#endif
{
    sigframe_rt_t *frame = (sigframe_rt_t *) xsp;
#ifndef X64
    /* Read the normal arguments from the frame. */
    int sig = frame->sig;
    siginfo_t *siginfo = frame->pinfo;
    kernel_ucontext_t *ucxt = frame->puc;
#endif /* !X64 */
#ifdef DEBUG
    uint level = 2;
# ifdef INTERNAL
    struct sigcontext *sc = (struct sigcontext *) &(ucxt->uc_mcontext);
# endif
# ifndef HAVE_PROC_MAPS
    /* avoid logging every single TRY probe fault */
    if (!dynamo_initialized)
        level = 5;
# endif
#endif
    bool local;
    dcontext_t *dcontext = get_thread_private_dcontext();

    /* i#350: To support safe_read or TRY_EXCEPT without a dcontext, use the
     * global dcontext
     * when handling safe_read faults.  This lets us pass the check for a
     * dcontext below and causes us to use the global log.
     */
    if (dcontext == NULL && (sig == SIGSEGV || sig == SIGBUS) &&
        (is_safe_read_ucxt(ucxt) ||
         (!dynamo_initialized && global_try_except.try_except_state != NULL))) {
        dcontext = GLOBAL_DCONTEXT;
    }

    if (dynamo_exited && get_num_threads() > 1 && sig == SIGSEGV) {
        /* PR 470957: this is almost certainly a race so just squelch it.
         * We live w/ the risk that it was holding a lock our release-build
         * exit code needs.
         */
        exit_thread_syscall(1);
    }
    /* FIXME: ensure the path for recording a pending signal does not grab any DR locks
     * that could have been interrupted
     * e.g., synchronize_dynamic_options grabs the stats_lock!
     */
    if (dcontext == NULL ||
        (dcontext != GLOBAL_DCONTEXT &&
         (dcontext->signal_field == NULL ||
          !((thread_sig_info_t*)dcontext->signal_field)->fully_initialized))) {
        /* FIXME: || !intercept_asynch, or maybe !under_our_control */
        /* FIXME i#26: this could be a signal arbitrarily sent to this thread.
         * We could try to route it to another thread, using a global queue
         * of pending signals.  But what if it was targeted to this thread
         * via SYS_{tgkill,tkill}?  Can we tell the difference, even if
         * we watch the kill syscalls: could come from another process?
         */
        if (sig_is_alarm_signal(sig)) {
            /* assuming an alarm during thread exit or init (xref PR 596127,
             * i#359): suppressing is fine
             */
        } else if (sig == SUSPEND_SIGNAL && dcontext == NULL) {
            /* We sent SUSPEND_SIGNAL to a thread we don't control (no
             * dcontext), which means we want to take over.
             */
            struct sigcontext *sc = (struct sigcontext *) &(ucxt->uc_mcontext);
            sig_take_over(sc);  /* no return */
            ASSERT_NOT_REACHED();
        } else {
            /* Using global dcontext because dcontext is NULL here. */
            DOLOG(1, LOG_ASYNCH, { dump_sigcontext(GLOBAL_DCONTEXT, sc); });
            SYSLOG_INTERNAL_ERROR("ERROR: master_signal_handler with no siginfo "
                                  "(i#26?): tid=%d, sig=%d", get_sys_thread_id(), sig);
        }
        /* see FIXME comments above.
         * workaround for now: suppressing is better than dying.
         */
        if (can_always_delay[sig])
            return;
        else
            exit_process_syscall(1);
    }

    /* we may be entering dynamo from code cache! */
    /* Note that this is unsafe if -single_thread_in_DR => we grab a lock =>
     * hang if signal interrupts DR: but we don't really support that option
     */
    ENTERING_DR();
    if (dcontext == GLOBAL_DCONTEXT) {
        local = false;
    } else {
        local = local_heap_protected(dcontext);
        if (local)
            SELF_PROTECT_LOCAL(dcontext, WRITABLE);
    }

    LOG(THREAD, LOG_ASYNCH, level, "\nmaster_signal_handler: sig=%d, retaddr="PFX"\n",
        sig, *((byte **)xsp));
    LOG(THREAD, LOG_ASYNCH, level+1,
        "siginfo: pid = %d, status = %d, errno = %d, si_code = %d\n",
        siginfo->si_pid, siginfo->si_status, siginfo->si_errno, 
        siginfo->si_code);
    DOLOG(level+1, LOG_ASYNCH, { dump_sigcontext(dcontext, sc); });

#ifndef X64
# ifndef VMX86_SERVER
    /* FIXME case 6700: 2.6.9 (FC3) kernel sets up our frame with a pretcode
     * of 0x440.  This happens if our restorer is unspecified (though 2.6.9
     * src code shows setting the restorer to a default value in that case...)
     * or if we explicitly point at dynamorio_sigreturn.  I couldn't figure
     * out why it kept putting 0x440 there.  So we fix the issue w/ this
     * hardcoded return.
     * This hack causes vmkernel to kill the process on sigreturn due to 
     * vmkernel's non-standard sigreturn semantics.  PR 404712.
     */
    *((byte **)xsp) = (byte *) dynamorio_sigreturn;
# endif
#endif

    /* N.B.:
     * ucontext_t is defined in two different places.  The one we get
     * included is /usr/include/sys/ucontext.h, which would have us
     * doing this:
     *     void *pc = (void *) ucxt->uc_mcontext.gregs[EIP];
     * However, EIP is not defined for us (used to be in older
     * RedHat version) unless we define __USE_GNU, which we don't want to do
     * for other reasons, so we'd have to also say:
     *     #define EIP 14
     * Instead we go by the ucontext_t definition in
     * /usr/include/asm/ucontext.h, which has it containing a sigcontext struct,
     * defined in /usr/include/asm/sigcontext.h.  This is the definition used
     * by the kernel.  The two definitions are field-for-field
     * identical except that the sys one has an fpstate struct at the end --
     * but the next field in the frame is an fpstate.  The only mystery
     * is why the rt frame is declared as ucontext instead of sigcontext.
     * The kernel's version of ucontext must be the asm one!
     * And the sys one grabs the next field of the frame.
     * Also note that mcontext_t.fpregs == sigcontext.fpstate is NULL if
     * floating point operations have not been used (lazy fp state saving).
     * Also, sigset_t has different sizes according to kernel (8 bytes) vs.
     * glibc (128 bytes?).
     */

    switch (sig) {

    case SIGBUS: /* PR 313665: look for DR crashes on unaligned memory or mmap bounds */
    case SIGSEGV: {
        /* Older kernels do NOT fill out the signal-specific fields of siginfo,
         * except for SIGCHLD.  Thus we cannot do this:
         *     void *pc = (void*) siginfo->si_addr;
         * Thus we must use the third argument, which is a ucontext_t (see above)
         */
        struct sigcontext *sc = (struct sigcontext *) &(ucxt->uc_mcontext);
        void *pc = (void *) sc->SC_XIP;
        bool syscall_signal = false; /* signal came from syscall? */
        bool is_write = false;
        byte *target;
        bool is_DR_exception = false;

#ifdef SIDELINE
        if (dcontext == NULL) {
            SYSLOG_INTERNAL_ERROR("seg fault in sideline thread -- NULL dcontext!");
            ASSERT_NOT_REACHED();
        }
#endif
        if (is_safe_read_ucxt(ucxt) ||
            (!dynamo_initialized && global_try_except.try_except_state != NULL) ||
            dcontext->try_except.try_except_state != NULL) {
            /* handle our own TRY/EXCEPT */
            try_except_context_t *try_cxt;
#ifdef HAVE_PROC_MAPS
            /* our probe produces many of these every run */
            /* since we use for safe_*, making a _ONCE */
            SYSLOG_INTERNAL_WARNING_ONCE("(1+x) Handling our fault in a TRY at "PFX, pc);
#endif
            LOG(THREAD, LOG_ALL, level, "TRY fault at "PFX"\n", pc);
            if (TEST(DUMPCORE_TRY_EXCEPT, DYNAMO_OPTION(dumpcore_mask)))
                os_dump_core("try/except fault");

            if (is_safe_read_ucxt(ucxt)) {
                ucxt->uc_mcontext.SC_XIP = (reg_t) safe_read_resume_pc();
                /* Break out to log the normal return from the signal handler.
                 */
                break;
            }
            try_cxt = (dcontext != NULL) ? dcontext->try_except.try_except_state :
                global_try_except.try_except_state;
            ASSERT(try_cxt != NULL);

            /* The exception interception code did an ENTER so we must EXIT here */
            EXITING_DR();
            /* Since we have no sigreturn we have to restore the mask
             * manually, just like siglongjmp().  i#226/PR 492568: we rely
             * on the kernel storing the prior mask in ucxt, so we do not
             * need to store it on every setjmp.
             */
            /* Verify that there's no scenario where the mask gets changed prior
             * to a fault inside a try
             */
            ASSERT(memcmp(&try_cxt->context.sigmask,
                          &ucxt->uc_sigmask, sizeof(ucxt->uc_sigmask)) == 0);
            sigprocmask_syscall(SIG_SETMASK, &ucxt->uc_sigmask, NULL,
                                sizeof(ucxt->uc_sigmask));
            DR_LONGJMP(&try_cxt->context, LONGJMP_EXCEPTION);
            ASSERT_NOT_REACHED();
        }

#ifdef CLIENT_INTERFACE
        if (!IS_INTERNAL_STRING_OPTION_EMPTY(client_lib) && is_in_client_lib(pc)) {
            abort_on_fault(dcontext, DUMPCORE_CLIENT_EXCEPTION, pc, sc,
                           "Client exception",  (sig == SIGSEGV) ? "SEGV" : "BUS",
                           " client library");
            ASSERT_NOT_REACHED();
        }
#endif

        /* For !HAVE_PROC_MAPS, we cannot compute the target until
         * after the try/except check b/c compute_memory_target()
         * calls get_memory_info_from_os() which does a probe: and the
         * try/except could be from a probe itself.  A try/except that
         * triggers a stack overflow should recover on the longjmp, so
         * this order should be fine.
         */

        target = compute_memory_target(dcontext, pc, sc, siginfo, &is_write);
#ifdef STACK_GUARD_PAGE
        if (sig == SIGSEGV && is_write && is_stack_overflow(dcontext, target)) {
            SYSLOG_INTERNAL_CRITICAL(PRODUCT_NAME" stack overflow at pc "PFX, pc);
            /* options are already synchronized by the SYSLOG */
            if (TEST(DUMPCORE_INTERNAL_EXCEPTION, dynamo_options.dumpcore_mask))
                os_dump_core("stack overflow");
            os_terminate(dcontext, TERMINATE_PROCESS);
        }
#endif /* STACK_GUARD_PAGE */

        /* FIXME: share code with Windows callback.c */
        /* FIXME PR 205795: in_fcache and is_dynamo_address do grab locks! */
        if ((is_on_dstack(dcontext, (byte *)sc->SC_XSP)
             /* PR 302951: clean call arg processing => pass to app/client.
              * Rather than call the risky in_fcache we check whereami. */
             IF_CLIENT_INTERFACE(&& (dcontext->whereami != WHERE_FCACHE))) ||
            is_on_alt_stack(dcontext, (byte *)sc->SC_XSP) ||
            is_on_initstack((byte *)sc->SC_XSP)) {
            /* Checks here need to cover everything that record_pending_signal()
             * thinks is non-fcache, non-gencode: else that routine will kill
             * process since can't delay or re-execute (i#195/PR 453964).
             */
            is_DR_exception = true;
        } else if (!safe_is_in_fcache(dcontext, pc, (byte*)sc->SC_XSP) &&
                   (in_generated_routine(dcontext, pc) ||
                    is_at_do_syscall(dcontext, pc, (byte*)sc->SC_XSP) ||
                    is_dynamo_address(pc))) {
#ifdef CLIENT_INTERFACE
            if (!in_generated_routine(dcontext, pc) &&
                !is_at_do_syscall(dcontext, pc, (byte*)sc->SC_XSP)) {
                /* PR 451074: client needs a chance to handle exceptions in its
                 * own gencode.  client_exception_event() won't return if client
                 * wants to re-execute faulting instr.
                 */
                dr_signal_action_t action =
                    send_signal_to_client(dcontext, sig, frame, sc,
                                          target, false/*!blocked*/, NULL);
                if (action != DR_SIGNAL_DELIVER && /* for delivery, continue below */
                    !handle_client_action_from_cache(dcontext, sig, action, frame,
                                                     sc, false/*!blocked*/)) {
                    /* client handled fault */
                    break;
                }
            }
#endif
            is_DR_exception = true;
        }
        if (is_DR_exception) {
            /* kill(getpid(), SIGSEGV) looks just like a SIGSEGV in the store of eax
             * to mcontext after the syscall instr in do_syscall -- try to distinguish:
             */
            if (is_sys_kill(dcontext, pc, (byte*)sc->SC_XSP, siginfo)) {
                LOG(THREAD, LOG_ALL, 2,
                    "assuming SIGSEGV at post-do-syscall is kill, not our write fault\n");
                syscall_signal = true;
            }
            if (!syscall_signal) {
                if (check_in_last_thread_vm_area(dcontext, target)) {
                    /* See comments in callback.c as well.
                     * FIXME: try to share code
                     */
                    SYSLOG_INTERNAL_WARNING("(decode) exception in last area, "
                                            "DR pc="PFX", app pc="PFX, pc, target);
                    STATS_INC(num_exceptions_decode);
                    if (is_building_trace(dcontext)) {
                        LOG(THREAD, LOG_ASYNCH, 2, "intercept_exception: "
                                                   "squashing old trace\n");
                        trace_abort(dcontext);
                    }
                    /* we do get faults when not building a bb: e.g.,
                     * ret_after_call_check does decoding (case 9396) */
                    if (dcontext->bb_build_info != NULL) {
                        /* must have been building a bb at the time */
                        bb_build_abort(dcontext, true/*clean vm area*/, true/*unlock*/);
                    }
                    /* Since we have no sigreturn we have to restore the mask manually */
                    unblock_all_signals(NULL);
                    /* Let's pass it back to the application - memory is unreadable */
                    if (TEST(DUMPCORE_FORGE_UNREAD_EXEC, DYNAMO_OPTION(dumpcore_mask)))
                        os_dump_core("Warning: Racy app execution (decode unreadable)");
                    os_forge_exception(target, UNREADABLE_MEMORY_EXECUTION_EXCEPTION);
                    ASSERT_NOT_REACHED();
                } else {
                    abort_on_DR_fault(dcontext, pc, sc, (sig == SIGSEGV) ? "SEGV" : "BUS",
                                      in_generated_routine(dcontext, pc) ?
                                      " generated" : "");
                }
            }
        }
        /* if get here, pass the signal to the app */

        ASSERT(pc != 0); /* shouldn't get here */
        if (sig == SIGSEGV && !syscall_signal/*only for in-cache signals*/) {
            /* special case: we expect a seg fault for executable regions
             * that were writable and marked read-only by us.
             */
            if (is_write && check_for_modified_code(dcontext, pc, sc, target)) {
                /* it was our signal, so don't pass to app -- return now */
                break;
            }
        }
        /* pass it to the application (or client) */
        LOG(THREAD, LOG_ALL, 1,
            "** Received SIG%s at cache pc "PFX" in thread %d\n",
            (sig == SIGSEGV) ? "SEGV" : "BUS", pc, get_thread_id());
        ASSERT(syscall_signal || safe_is_in_fcache(dcontext, pc, (byte *)sc->SC_XSP));
        /* we do not call trace_abort() here since we may need to
         * translate from a temp private bb (i#376): but all paths
         * that deliver the signal or redirect will call it
         */
        record_pending_signal(dcontext, sig, ucxt, frame, false _IF_CLIENT(target));
        break;
    }

    /* PR 212090: the signal we use to suspend threads */
    case SUSPEND_SIGNAL:
        if (handle_suspend_signal(dcontext, ucxt))
            record_pending_signal(dcontext, sig, ucxt, frame, false _IF_CLIENT(NULL));
        /* else, don't deliver to app */
        break;

    /* i#61/PR 211530: the signal we use for nudges */
    case NUDGESIG_SIGNUM:
        if (handle_nudge_signal(dcontext, siginfo, ucxt))
            record_pending_signal(dcontext, sig, ucxt, frame, false _IF_CLIENT(NULL));
        /* else, don't deliver to app */
        break;

    case SIGALRM:
    case SIGVTALRM:
    case SIGPROF:
        if (handle_alarm(dcontext, sig, ucxt))
            record_pending_signal(dcontext, sig, ucxt, frame, false _IF_CLIENT(NULL));
        /* else, don't deliver to app */
        break;

#ifdef SIDELINE
    case SIGCHLD: {
        int status = siginfo->si_status;
        if (siginfo->si_pid == 0) {
            /* FIXME: with older versions of linux the sigchld fields of
             * siginfo are not filled in properly!
             * This is my attempt to handle that, pid seems to be 0
             */
            break;
        }
        if (status != 0) {
            LOG(THREAD, LOG_ALL, 0, "*** Child thread died with error %d\n",
                status);
            ASSERT_NOT_REACHED();
        }
        break;
    }
#endif
    
    default: {
        record_pending_signal(dcontext, sig, ucxt, frame, false _IF_CLIENT(NULL));
        break;
    }
    } /* end switch */
    
    LOG(THREAD, LOG_ASYNCH, level, "\tmaster_signal_handler %d returning now\n\n", sig);

    /* restore protections */
    if (local)
        SELF_PROTECT_LOCAL(dcontext, READONLY);
    EXITING_DR();
}

static bool
execute_handler_from_cache(dcontext_t *dcontext, int sig, sigframe_rt_t *our_frame,
                           struct sigcontext *sc_orig, fragment_t *f
                           _IF_CLIENT(byte *access_address))
{
    thread_sig_info_t *info = (thread_sig_info_t *) dcontext->signal_field;
    /* we want to modify the sc in DR's frame */
    struct sigcontext *sc = get_sigcontext_from_rt_frame(our_frame);
    kernel_sigset_t blocked;
    /* Need to get xsp now before get new dcontext.
     * This is the translated xsp, so we avoid PR 306410 (cleancall arg fault
     * on dstack => handler run on dstack) that Windows hit.
     */
    byte *xsp = get_sigstack_frame_ptr(dcontext, sig,
                                       our_frame/* take xsp from (translated)
                                                 * interruption point */);

#ifdef CLIENT_INTERFACE
    dr_signal_action_t action = 
        send_signal_to_client(dcontext, sig, our_frame, sc_orig, access_address,
                              false/*not blocked*/, f);
    if (!handle_client_action_from_cache(dcontext, sig, action, our_frame, sc_orig,
                                         false/*!blocked*/))
        return false;
#else
    if (info->app_sigaction[sig] == NULL ||
        info->app_sigaction[sig]->handler == (handler_t)SIG_DFL) {
        LOG(THREAD, LOG_ASYNCH, 3, "\taction is SIG_DFL\n");
        if (execute_default_from_cache(dcontext, sig, our_frame, sc_orig)) {
            /* if we haven't terminated, restore original (untranslated) sc
             * on request.
             */
            our_frame->uc.uc_mcontext = *sc_orig;
        }
        return false;
    }
    ASSERT(info->app_sigaction[sig] != NULL &&
           info->app_sigaction[sig]->handler != (handler_t)SIG_IGN &&
           info->app_sigaction[sig]->handler != (handler_t)SIG_DFL);
#endif

    LOG(THREAD, LOG_ASYNCH, 2, "execute_handler_from_cache for signal %d\n", sig);
    RSTATS_INC(num_signals);

    /* now that we know it's not a client-involved fault, dump as app fault */
    report_app_problem(dcontext, APPFAULT_FAULT, (byte *)sc->SC_XIP, (byte *)sc->SC_XBP,
                       "\nSignal %d delivered to application handler.\n", sig);

    LOG(THREAD, LOG_ASYNCH, 3, "\txsp is "PFX"\n", xsp);

    /* copy frame to appropriate stack and convert to non-rt if necessary */
    copy_frame_to_stack(dcontext, sig, our_frame, (void *)xsp);
    LOG(THREAD, LOG_ASYNCH, 3, "\tcopied frame from "PFX" to "PFX"\n", our_frame, xsp);

    /* Because of difficulties determining when/if a signal handler
     * returns, we do what the kernel does: abandon all of our current
     * state, copy what we might need to the handler frame if we come back,
     * and then it's ok if the handler doesn't return.
     * If it does, we start interpreting afresh when we see sigreturn().
     * This routine assumes anything needed to return has been put in the
     * frame (only needed for signals queued up while in dynamo), and goes
     * ahead and trashes the current dcontext.
     */

    /* if we were building a trace, kill it */
    if (is_building_trace(dcontext)) {
        LOG(THREAD, LOG_ASYNCH, 3, "\tsquashing trace-in-progress\n");
        trace_abort(dcontext);
    }

    /* add to set of blocked signals those in sigaction mask */
    blocked = info->app_sigaction[sig]->mask;
    /* SA_NOMASK says whether to block sig itself or not */
    if ((info->app_sigaction[sig]->flags & SA_NOMASK) == 0)
        kernel_sigaddset(&blocked, sig);
    set_blocked(dcontext, &blocked, false/*relative: OR these in*/);

    /* Set our sigreturn context (NOT for the app: we already copied the
     * translated context to the app stack) to point to fcache_return!
     * Then we'll go back through kernel, appear in fcache_return,
     * and go through dispatch & interp, without messing up DR stack.
     */
    sc->SC_XIP = (ptr_uint_t) fcache_return_routine(dcontext);
    sc->SC_XAX = (ptr_uint_t) get_sigreturn_linkstub();
    /* Doesn't matter what most app registers are, signal handler doesn't
     * expect anything except the frame on the stack.  We do need to set xsp,
     * only because if app wants special signal stack we need to point xsp
     * there.  (If no special signal stack, this is a nop.)
     */
    sc->SC_XSP = (ptr_uint_t) xsp;
#ifdef X64
    /* Set up args to handler: int sig, siginfo_t *siginfo, kernel_ucontext_t *ucxt */
    sc->SC_XDI = sig;
    sc->SC_XSI = (reg_t) &((sigframe_rt_t *)xsp)->info;
    sc->SC_XDX = (reg_t) &((sigframe_rt_t *)xsp)->uc;
#endif
    /* Make sure handler is next thing we execute */       
    dcontext->next_tag = (app_pc) info->app_sigaction[sig]->handler;

    if ((info->app_sigaction[sig]->flags & SA_ONESHOT) != 0) {
        /* clear handler now -- can't delete memory since sigreturn,
         * others may look at sigaction struct, so we just set to default
         */
        info->app_sigaction[sig]->handler = (handler_t) SIG_DFL;
    }

    LOG(THREAD, LOG_ASYNCH, 3, "\tset next_tag to handler "PFX", xsp to "PFX"\n",
        info->app_sigaction[sig]->handler, xsp);
    return true;
}

static bool
execute_handler_from_dispatch(dcontext_t *dcontext, int sig)
{
    thread_sig_info_t *info = (thread_sig_info_t *) dcontext->signal_field;
    handler_t handler;
    byte *xsp = get_sigstack_frame_ptr(dcontext, sig, NULL);
    sigframe_rt_t *frame = &(info->sigpending[sig]->rt_frame);
    priv_mcontext_t *mcontext = get_mcontext(dcontext);
    struct sigcontext *sc;
    kernel_sigset_t blocked;

#ifdef CLIENT_INTERFACE
    dr_signal_action_t action;
#else
    if (info->app_sigaction[sig] == NULL ||
        info->app_sigaction[sig]->handler == (handler_t)SIG_DFL) {
        LOG(THREAD, LOG_ASYNCH, 3, "\taction is SIG_DFL\n");
        execute_default_from_dispatch(dcontext, sig, frame);
        return true;
    }
    ASSERT(info->app_sigaction[sig] != NULL &&
           info->app_sigaction[sig]->handler != (handler_t)SIG_IGN &&
           info->app_sigaction[sig]->handler != (handler_t)SIG_DFL);
#endif

    LOG(THREAD, LOG_ASYNCH, 2, "execute_handler_from_dispatch for signal %d\n", sig);
    RSTATS_INC(num_signals);

    /* modify the rtframe before copying to stack so we can pass final
     * version to client, and propagate its mods
     */
    sc = get_sigcontext_from_rt_frame(frame);

    /* Because of difficulties determining when/if a signal handler
     * returns, we do what the kernel does: abandon all of our current
     * state, copy what we might need to the handler frame if we come back,
     * and then it's ok if the handler doesn't return.
     * If it does, we start interpreting afresh when we see sigreturn().
     */

#ifdef DEBUG
    if (stats->loglevel >= 3 && (stats->logmask & LOG_ASYNCH) != 0) {
        LOG(THREAD, LOG_ASYNCH, 3, "original sigcontext:\n");
        dump_sigcontext(dcontext, sc);
    }
#endif
    if (info->sigpending[sig]->use_sigcontext) {
        LOG(THREAD, LOG_ASYNCH, 2,
            "%s: using sigcontext, not mcontext (syscall restart)\n", __FUNCTION__);
    } else {
        /* copy currently-interrupted-context to frame's context, so we can
         * abandon the currently-interrupted context.
         */
        mcontext_to_sigcontext(sc, mcontext);
    }
    /* mcontext does not contain fp or mmx or xmm state, which may have
     * changed since the frame was created (while finishing up interrupted
     * fragment prior to returning to dispatch).  Since DR does not touch
     * this state except for xmm on x64, we go ahead and copy the
     * current state into the frame, and then touch up xmm for x64.
     */
    /* FIXME: should this be done for all pending as soon as reach
     * dispatch?  what if get two asynch inside same frag prior to exiting
     * cache?  have issues with fpstate, but also prob with next_tag? FIXME
     */
    /* FIXME: we should clear fpstate for app handler itself as that's
     * how our own handler is executed.
     */
    ASSERT(sc->fpstate != NULL); /* not doing i#641 yet */
    save_fpstate(dcontext, frame);
#ifdef DEBUG
    if (stats->loglevel >= 3 && (stats->logmask & LOG_ASYNCH) != 0) {
        LOG(THREAD, LOG_ASYNCH, 3, "new sigcontext:\n");
        dump_sigcontext(dcontext, sc);
        LOG(THREAD, LOG_ASYNCH, 3, "\n");
    }
#endif
    /* FIXME: other state?  debug regs?
     * if no syscall allowed between master_ (when frame created) and
     * receiving, then don't have to worry about debug regs, etc.
     * check for syscall when record pending, if it exists, try to
     * receive in pre_system_call or something? what if ignorable?  FIXME!
     */

    if (!info->sigpending[sig]->use_sigcontext) {
        /* for the pc we want the app pc not the cache pc */
        sc->SC_XIP = (ptr_uint_t) dcontext->next_tag;
        LOG(THREAD, LOG_ASYNCH, 3, "\tset frame's eip to "PFX"\n", sc->SC_XIP);
    }

#ifdef CLIENT_INTERFACE
    action = send_signal_to_client(dcontext, sig, frame, NULL,
                                   info->sigpending[sig]->access_address,
                                   false/*not blocked*/, NULL);
    /* in order to pass to the client, we come all the way here for signals
     * the app has no handler for
     */
    if (action == DR_SIGNAL_REDIRECT) {
        /* send_signal_to_client copied mcontext into frame's sc */
        sigcontext_to_mcontext(get_mcontext(dcontext), sc);
        dcontext->next_tag = (app_pc) sc->SC_XIP;
        if (is_building_trace(dcontext)) {
            LOG(THREAD, LOG_ASYNCH, 3, "\tsquashing trace-in-progress\n");
            trace_abort(dcontext);
        }
        return true; /* don't try another signal */
    }
    else if (action == DR_SIGNAL_SUPPRESS ||
        (info->app_sigaction[sig] != NULL &&
         info->app_sigaction[sig]->handler == (handler_t)SIG_IGN)) {
        LOG(THREAD, LOG_ASYNCH, 2, "%s: not delivering!\n",
            (action == DR_SIGNAL_SUPPRESS) ?
            "client suppressing signal" :
            "app signal handler is SIG_IGN");
        return false;
    }
    else if (action == DR_SIGNAL_BYPASS ||
        (info->app_sigaction[sig] == NULL ||
         info->app_sigaction[sig]->handler == (handler_t)SIG_DFL)) {
        LOG(THREAD, LOG_ASYNCH, 2, "%s: executing default action\n",
            (action == DR_SIGNAL_BYPASS) ?
            "client forcing default" :
            "app signal handler is SIG_DFL");
        if (info->sigpending[sig]->use_sigcontext) {
            /* after the default action we want to go to the sigcontext */
            dcontext->next_tag = (app_pc) sc->SC_XIP;
            sigcontext_to_mcontext(get_mcontext(dcontext), sc);
        }
        execute_default_from_dispatch(dcontext, sig, frame);
        return true;
    }
    CLIENT_ASSERT(action == DR_SIGNAL_DELIVER, "invalid signal event return value");
#endif

    /* now that we've made all our changes and given the client a
     * chance to make changes, copy the frame to the appropriate stack
     * location and convert to non-rt if necessary 
     */
    copy_frame_to_stack(dcontext, sig, frame, xsp);
    /* now point at the app's frame */
    sc = get_sigcontext_from_app_frame(info, sig, (void *) xsp);

    ASSERT(info->app_sigaction[sig] != NULL);
    handler = info->app_sigaction[sig]->handler;

    /* add to set of blocked signals those in sigaction mask */
    blocked = info->app_sigaction[sig]->mask;
    /* SA_NOMASK says whether to block sig itself or not */
    if ((info->app_sigaction[sig]->flags & SA_NOMASK) == 0)
        kernel_sigaddset(&blocked, sig);
    set_blocked(dcontext, &blocked, false/*relative: OR these in*/);

    /* if we were building a trace, kill it */
    if (is_building_trace(dcontext)) {
        LOG(THREAD, LOG_ASYNCH, 3, "\tsquashing trace-in-progress\n");
        trace_abort(dcontext);
    }

    /* Doesn't matter what most app registers are, signal handler doesn't
     * expect anything except the frame on the stack.  We do need to set xsp.
     */
    mcontext->xsp = (ptr_uint_t) xsp;
#ifdef X64
    /* Set up args to handler: int sig, siginfo_t *siginfo, kernel_ucontext_t *ucxt */
    mcontext->xdi = sig;
    mcontext->xsi = (reg_t) &((sigframe_rt_t *)xsp)->info;
    mcontext->xdx = (reg_t) &((sigframe_rt_t *)xsp)->uc;
#endif
    /* Clear eflags DF (signal handler should match function entry ABI) */
    mcontext->xflags &= ~EFLAGS_DF;
    /* Make sure handler is next thing we execute */       
    dcontext->next_tag = (app_pc) handler;

    if ((info->app_sigaction[sig]->flags & SA_ONESHOT) != 0) {
        /* clear handler now -- can't delete memory since sigreturn,
         * others may look at sigaction struct, so we just set to default
         */
        info->app_sigaction[sig]->handler = (handler_t) SIG_DFL;
    }

    LOG(THREAD, LOG_ASYNCH, 3, "\tset xsp to "PFX"\n", xsp);
    return true;
}

/* The arg to SYS_kill, i.e., the signal number, should be in dcontext->sys_param0 */
static void
terminate_via_kill(dcontext_t *dcontext)
{
    ASSERT(dcontext == get_thread_private_dcontext());

    /* FIXME PR 541760: there can be multiple thread groups and thus
     * this may not exit all threads in the address space
     */
    cleanup_and_terminate(dcontext, SYS_kill,
                          /* Pass -pid in case main thread has exited
                           * in which case will get -ESRCH
                           */
                          IF_VMX86(os_in_vmkernel_userworld() ?
                                   -(int)get_process_id() :)
                          get_process_id(),
                          dcontext->sys_param0, true);
    ASSERT_NOT_REACHED();
}

static bool
execute_default_action(dcontext_t *dcontext, int sig, sigframe_rt_t *frame,
                       struct sigcontext *sc_orig, bool from_dispatch)
{
    thread_sig_info_t *info = (thread_sig_info_t *) dcontext->signal_field;
    struct sigcontext *sc = get_sigcontext_from_rt_frame(frame);
    byte *pc = (byte *) sc->SC_XIP;

    LOG(THREAD, LOG_ASYNCH, 3, "execute_default_action for signal %d\n", sig);

    /* should only come here for signals we catch, or signal with ONESHOT
     * that didn't sigreturn
     */
    ASSERT(info->we_intercept[sig] ||
           (info->app_sigaction[sig]->flags & SA_ONESHOT) != 0);

    if (info->app_sigaction[sig] != NULL &&
        (info->app_sigaction[sig]->flags & SA_ONESHOT) != 0) {
        if (!info->we_intercept[sig]) {
            handler_free(dcontext, info->app_sigaction[sig], sizeof(kernel_sigaction_t));
            info->app_sigaction[sig] = NULL;
        }
    }

    /* FIXME PR 205310: we can't always perfectly emulate the default
     * behavior.  To execute the default action, we have to un-register our
     * handler, if we have one, for signals whose default action is not
     * ignore or that will just be re-raised upon returning to the
     * interrupted context -- FIXME: are any of the ignores repeated?
     * SIGURG?
     *
     * If called from execute_handler_from_cache(), our master_signal_handler()
     * is going to return directly to the translated context: which means we
     * go native to re-execute the instr, which if it does in fact generate
     * the signal again means we have a nice transparent core dump.
     *
     * If called from execute_handler_from_dispatch(), we need to generate
     * the signal ourselves.
     */
    if (default_action[sig] != DEFAULT_IGNORE) {
        kernel_sigset_t set;
        DEBUG_DECLARE(bool ok =)
            set_default_signal_action(sig);
        ASSERT(ok);

        /* If we're in our handler now, we have to unblock */
        kernel_sigemptyset(&set);
        kernel_sigaddset(&set, sig);
        sigprocmask_syscall(SIG_UNBLOCK, &set, NULL, sizeof(set));

        /* FIXME: to avoid races w/ shared handlers should set a flag to
         * prevent another thread from re-enabling.
         * Perhaps worse: what if this signal arrives for another thread
         * in the meantime (and the default is not terminate)?
         */
        if (info->shared_app_sigaction) {
            LOG(THREAD, LOG_ASYNCH, 1,
                "WARNING: having to install SIG_DFL for thread %d, but will be shared!\n",
                get_thread_id());
        }
        if (default_action[sig] == DEFAULT_TERMINATE ||
            default_action[sig] == DEFAULT_TERMINATE_CORE) {
            report_app_problem(dcontext, APPFAULT_CRASH, pc, (byte *)sc->SC_XBP,
                               "\nSignal %d delivered to application as default action.\n",
                               sig);
            /* N.B.: we don't have to restore our handler because the
             * default action is for the process (entire thread group for NPTL) to die!
             */
            if (from_dispatch ||
                can_always_delay[sig] ||
                is_sys_kill(dcontext, pc, (byte*)sc->SC_XSP, &frame->info)) {
                /* This must have come from SYS_kill rather than raised by
                 * a faulting instruction.  Thus we can't go re-execute the
                 * instr in order to re-raise the signal (if from_dispatch,
                 * we delayed and can't re-execute anyway).  Instead we
                 * re-generate via SYS_kill.  An alternative, if we don't
                 * care about generating a core dump, is to use SYS_exit
                 * and pass the right exit code to indicate the signal
                 * number: that would avoid races w/ the sigaction.
                 *
                 * FIXME: should have app make the syscall to get a more
                 * transparent core dump!
                 */
                byte *cur_esp;
                if (!from_dispatch)
                    KSTOP_NOT_MATCHING_NOT_PROPAGATED(fcache_default);
                KSTOP_NOT_MATCHING_NOT_PROPAGATED(dispatch_num_exits);
                if (is_couldbelinking(dcontext)) /* won't be for SYS_kill (i#1159) */
                    enter_nolinking(dcontext, NULL, false);
                GET_STACK_PTR(cur_esp);
                dcontext->sys_param0 = sig; /* store arg to SYS_kill */
                if (cur_esp >= (byte *)info->sigstack.ss_sp &&
                    cur_esp <  (byte *)info->sigstack.ss_sp + info->sigstack.ss_size) {
                    /* We can't clean up our sigstack properly when we're on it
                     * (i#1160) so we terminate on the dstack.
                     */
                    call_switch_stack(dcontext, dcontext->dstack, terminate_via_kill,
                                      false /*!initstack */, false/*no return */);
                } else {
                    terminate_via_kill(dcontext);
                }
                ASSERT_NOT_REACHED();
            } else {
                /* We assume that re-executing the interrupted instr will
                 * re-raise the fault.  We could easily be wrong:
                 * xref PR 363811 infinite loop due to memory we
                 * thought was unreadable and thus thought would raise
                 * a signal; xref PR 368277 to improve is_sys_kill().
                 * FIXME PR 205310: we should check whether we come out of
                 * the cache when we expected to terminate!
                 *
                 * An alternative is to abandon transparent core dumps and
                 * do the same explicit SYS_kill we do for from_dispatch.
                 * That would let us clean up DR as well.
                 * FIXME: currently we do not clean up DR for a synchronous
                 * signal death, but we do for asynch.
                 */
                /* i#552: cleanup and raise client exit event */
                int   instr_sz;
                thread_sig_info_t *info;
                /* We are on the sigstack now, so assign it to NULL to avoid being
                 * freed during process exit cleanup
                 */
                info = (thread_sig_info_t *)dcontext->signal_field;
                info->sigstack.ss_sp = NULL;
                /* We enter from several different places, so rewind until
                 * top-level kstat.
                 */
                KSTOP_REWIND_UNTIL(thread_measured);
                /* We try to raise the same signal in app's context so a correct
                 * coredump can be generated. However, the client might change
                 * the code in a way that the corresponding app code won't 
                 * raise the signal, so we first check if the app instr is the
                 * same as instr in the cache, and raise the signal (by return).
                 * Otherwise, we kill the process instead.
                 */
                ASSERT(sc_orig != NULL);
                instr_sz = decode_sizeof(dcontext, (byte *) sc_orig->SC_XIP,
                                         NULL _IF_X64(NULL));
                if (instr_sz != 0 &&
                    instr_sz == decode_sizeof(dcontext, pc, NULL _IF_X64(NULL)) &&
                    memcmp(pc, (byte *) sc_orig->SC_XIP, instr_sz) == 0) {
                    /* the app instr matches the cache instr; cleanup and raise the
                     * the signal in the app context
                     */
                    dynamo_process_exit();
                    /* we cannot re-enter the cache, which is freed by now */
                    ASSERT(!from_dispatch);
                    return false;
                } else {
                    /* mismatch, cleanup and terminate */
                    cleanup_and_terminate(dcontext, SYS_kill,
                                          /* Pass -pid in case main thread has exited
                                           * in which case will get -ESRCH
                                           */
                                          IF_VMX86(os_in_vmkernel_userworld() ?
                                                   -(int)get_process_id() :)
                                          get_process_id(),
                                          sig, true);
                }
            }
        } else {
            /* FIXME PR 297033: in order to intercept DEFAULT_STOP /
             * DEFAULT_CONTINUE signals, we need to set sigcontext to point
             * to some kind of regain-control routine, so that when our
             * thread gets to run again we can reset our handler.  So far
             * we have no signals that fall here that we intercept.
             */
            CLIENT_ASSERT(false, "STOP/CONT signals not supported");
        }
#if defined(DEBUG) && defined(INTERNAL)
        if (sig == SIGSEGV && !dynamo_exited) {
            /* pc should be an app pc at this point (it was translated) --
             * check for bad cases here
             */
            if (safe_is_in_fcache(dcontext, pc, (byte *)sc->SC_XSP)) {
                fragment_t wrapper;
                fragment_t *f;
                LOG(THREAD, LOG_ALL, 1,
                    "Received SIGSEGV at pc "PFX" in thread %d\n", pc, get_thread_id());
                f = fragment_pclookup(dcontext, pc, &wrapper);
                if (f)
                    disassemble_fragment(dcontext, f, false);
                ASSERT_NOT_REACHED();
            } else if (in_generated_routine(dcontext, pc)) {
                LOG(THREAD, LOG_ALL, 1,
                    "Received SIGSEGV at generated non-code-cache pc "PFX"\n", pc);
                ASSERT_NOT_REACHED();
            }
        }
#endif
    }

    /* now continue at the interruption point and re-raise the signal */
    return true;
}

static bool
execute_default_from_cache(dcontext_t *dcontext, int sig, sigframe_rt_t *frame,
                           struct sigcontext *sc_orig)
{
    return execute_default_action(dcontext, sig, frame, sc_orig, false);
}

static void
execute_default_from_dispatch(dcontext_t *dcontext, int sig, sigframe_rt_t *frame)
{
    execute_default_action(dcontext, sig, frame, NULL, true);
}

void
receive_pending_signal(dcontext_t *dcontext)
{
    thread_sig_info_t *info = (thread_sig_info_t *) dcontext->signal_field;
    sigpending_t *temp;
    int sig;
    LOG(THREAD, LOG_ASYNCH, 3, "receive_pending_signal\n");
    if (info->interrupted != NULL) {
        LOG(THREAD, LOG_ASYNCH, 3, "\tre-linking outgoing for interrupted F%d\n",
            info->interrupted->id);
        SHARED_FLAGS_RECURSIVE_LOCK(info->interrupted->flags, acquire,
                                    change_linking_lock);
        link_fragment_outgoing(dcontext, info->interrupted, false);
        SHARED_FLAGS_RECURSIVE_LOCK(info->interrupted->flags, release,
                                    change_linking_lock);
        if (TEST(FRAG_HAS_SYSCALL, info->interrupted->flags)) {
            /* restore syscall (they're a barrier to signals, so signal
             * handler has cur frag exit before it does a syscall)
             */
            ASSERT(info->interrupted_pc != NULL);
            mangle_syscall_code(dcontext, info->interrupted,
                                info->interrupted_pc, true/*skip exit cti*/);
        }
        info->interrupted = NULL;
        info->interrupted_pc = NULL;
    }
    /* grab first pending signal
     * XXX: start with real-time ones?
     */
    /* "lock" the array to prevent a new signal that interrupts this bit of
     * code from prepended or deleting from the array while we're accessing it
     */
    info->accessing_sigpending = true;
    /* barrier to prevent compiler from moving the above write below the loop */
    __asm__ __volatile__("" : : : "memory");
    for (sig = 1; sig <= MAX_SIGNUM; sig++) {
        if (info->sigpending[sig] != NULL) {
            bool executing = true;
            if (kernel_sigismember(&info->app_sigblocked, sig)) {
                LOG(THREAD, LOG_ASYNCH, 3, "\tsignal %d is blocked!\n", sig);
                continue;
            }
            LOG(THREAD, LOG_ASYNCH, 3, "\treceiving signal %d\n", sig);
            executing = execute_handler_from_dispatch(dcontext, sig);
            temp = info->sigpending[sig];
            info->sigpending[sig] = temp->next;
            special_heap_free(info->sigheap, temp);

            /* only one signal at a time! */
            if (executing)
                break;
        }
    }
    /* barrier to prevent compiler from moving the below write above the loop */
    __asm__ __volatile__("" : : : "memory");
    info->accessing_sigpending = false;

    /* we only clear this on a call to us where we find NO pending signals */
    if (sig > MAX_SIGNUM) {
        LOG(THREAD, LOG_ASYNCH, 3, "\tclearing signals_pending flag\n");
        dcontext->signals_pending = false;
    }
}

/* Returns false if should NOT issue syscall. */
bool
handle_sigreturn(dcontext_t *dcontext, bool rt)
{
    thread_sig_info_t *info = (thread_sig_info_t *) dcontext->signal_field;
    struct sigcontext *sc;
    int sig = 0;
    app_pc next_pc;
    /* xsp was put in mcontext prior to pre_system_call() */
    reg_t xsp = get_mcontext(dcontext)->xsp;

    LOG(THREAD, LOG_ASYNCH, 3, "%ssigreturn()\n", rt?"rt_":"");
    LOG(THREAD, LOG_ASYNCH, 3, "\txsp is "PFX"\n", xsp);

#ifdef PROGRAM_SHEPHERDING
    /* if (!sig_has_restorer, region was never added to exec list,
     * allowed as pattern only and kicked off at first write via
     * selfmod detection or otherwise if vsyscall, so no worries
     * about having to remove it here
     */
#endif

    /* get sigframe: it's the top thing on the stack, except the ret
     * popped off pretcode.
     * WARNING: handler for tcsh's window_change (SIGWINCH) clobbers its
     * signal # arg, so don't use frame->sig!  (kernel doesn't look at sig
     * so app can get away with it)
     */
    if (rt) {
        sigframe_rt_t *frame = (sigframe_rt_t *) (xsp - sizeof(char*));
        /* use si_signo instead of sig, less likely to be clobbered by app */
        sig = frame->info.si_signo;
#ifndef X64
        LOG(THREAD, LOG_ASYNCH, 3, "\tsignal was %d (did == param %d)\n", 
            sig, frame->sig);
        if (frame->sig != sig)
            LOG(THREAD, LOG_ASYNCH, 1, "WARNING: app sig handler clobbered sig param\n");
#endif
        ASSERT(sig > 0 && sig <= MAX_SIGNUM && IS_RT_FOR_APP(info, sig));
        /* FIXME: what if handler called sigaction and requested rt
         * when itself was non-rt?
         */
        sc = get_sigcontext_from_app_frame(info, sig, (void *) frame);
        /* discard blocked signals, re-set from prev mask stored in frame */
        set_blocked(dcontext, &frame->uc.uc_sigmask, true/*absolute*/);
    } else {
        /* FIXME: libc's restorer pops prior to calling sigreturn, I have
         * no idea why, but kernel asks for xsp-8 not xsp-4...weird!
         */
        kernel_sigset_t prevset;
        sigframe_plain_t *frame = (sigframe_plain_t *) (xsp-8);
        /* We don't trust frame->sig (app sometimes clobbers it), and for
         * plain frame there's no other place that sig is stored,
         * so as a hack we added a new frame!
         * FIXME: this means we won't support nonstandard use of SYS_sigreturn,
         * e.g., as NtContinue, if frame didn't come from a real signal and so
         * wasn't copied to stack by us.
         */
        sig = frame->sig_noclobber;
        LOG(THREAD, LOG_ASYNCH, 3, "\tsignal was %d (did == param %d)\n", 
            sig, frame->sig);
        if (frame->sig != sig)
            LOG(THREAD, LOG_ASYNCH, 1, "WARNING: app sig handler clobbered sig param\n");
        ASSERT(sig > 0 && sig <= MAX_SIGNUM && !IS_RT_FOR_APP(info, sig));
        sc = get_sigcontext_from_app_frame(info, sig, (void *) frame);
        /* discard blocked signals, re-set from prev mask stored in frame */
        prevset.sig[0] = frame->sc.oldmask;
        if (_NSIG_WORDS > 1)
            memcpy(&prevset.sig[1], &frame->extramask, sizeof(frame->extramask));
        set_blocked(dcontext, &prevset, true/*absolute*/);
    }

    /* Make sure we deliver pending signals that are now unblocked.
     */
    check_signals_pending(dcontext, info);

    /* We abandoned the previous context, so we need to start
     * interpreting anew.  Regardless of whether we handled the signal
     * from dispatch or the fcache, we want to go to the context
     * stored in the frame.  So we have the kernel send us to
     * fcache_return and set up for dispatch to use the frame's
     * context.
     */

    /* if we were building a trace, kill it */
    if (is_building_trace(dcontext)) {
        LOG(THREAD, LOG_ASYNCH, 3, "\tsquashing trace-in-progress\n");
        trace_abort(dcontext);
    }

    if ((info->app_sigaction[sig]->flags & SA_ONESHOT) != 0) {
        ASSERT(info->app_sigaction[sig]->handler == (handler_t) SIG_DFL);
        if (!info->we_intercept[sig]) {
            /* let kernel do default independent of us */
            handler_free(dcontext, info->app_sigaction[sig], sizeof(kernel_sigaction_t));
            info->app_sigaction[sig] = NULL;
        }
    }

    ASSERT(!safe_is_in_fcache(dcontext, (app_pc) sc->SC_XIP, (byte *)sc->SC_XSP));

#ifdef DEBUG
    if (stats->loglevel >= 3 && (stats->logmask & LOG_ASYNCH) != 0) {
        LOG(THREAD, LOG_ASYNCH, 3, "returning-to sigcontext:\n");
        dump_sigcontext(dcontext, sc);
    }
#endif

    /* XXX i#1206: if we interrupted a non-ignorable syscall to run the app's
     * handler, and we set up to restart the syscall, we'll come here with the
     * translated syscall pc -- thus we can't distinguish from a signal interrupting
     * the prior app instr.  So we can't simply point at do_syscall and call
     * set_at_syscall -- we have to re-interpret the syscall and re-run the
     * pre-syscall handler.  Hopefully all our pre-syscall handlers can handle that.
     */

    /* set up for dispatch */
    /* we have to use a different slot since next_tag ends up holding the do_syscall
     * entry when entered from dispatch (we're called from pre_syscall, prior to entering cache)
     */
    dcontext->asynch_target = (app_pc) sc->SC_XIP;
    next_pc = dcontext->asynch_target;

#ifdef VMX86_SERVER
    /* PR 404712: kernel only restores gp regs so we do it ourselves and avoid
     * complexities of kernel's non-linux-like sigreturn semantics
     */
    sigcontext_to_mcontext(get_mcontext(dcontext), sc);
#else
    /* HACK to get eax put into mcontext AFTER do_syscall */
    dcontext->next_tag = (app_pc) sc->SC_XAX;
    /* use special linkstub so we know why we came out of the cache */
    sc->SC_XAX = (ptr_uint_t) get_sigreturn_linkstub();

    /* set our sigreturn context to point to fcache_return */
    sc->SC_XIP = (ptr_uint_t) fcache_return_routine(dcontext);

    /* if we overlaid inner frame on nested signal, will end up with this
     * error -- disable in release build since this is often app's fault (stack
     * too small)
     * FIXME: how make this transparent?  what ends up happening is that we
     * get a segfault when we start interpreting dispatch, we want to make it
     * look like whatever would happen to the app...
     */
    ASSERT((app_pc)sc->SC_XIP != next_pc);
#endif

    LOG(THREAD, LOG_ASYNCH, 3, "\tset next tag to "PFX", sc->SC_XIP to "PFX"\n",
        next_pc, sc->SC_XIP);

    return IF_VMX86_ELSE(false, true);
}

bool
is_signal_restorer_code(byte *pc, size_t *len)
{
    /* is this a sigreturn pattern placed by kernel on the stack or vsyscall page?
     * for non-rt frame:
     *    0x58           popl %eax
     *    0xb8 <sysnum>  movl SYS_sigreturn, %eax
     *    0xcd 0x80      int 0x80
     * for rt frame:
     *    0xb8 <sysnum>  movl SYS_rt_sigreturn, %eax
     *    0xcd 0x80      int 0x80
     */
    /* optimized we only need two uint reads, but we have to do
     * some little-endian byte-order reverses to get the right result
     */
#   define reverse(x) ((((x) & 0xff) << 24) | (((x) & 0xff00) << 8) | \
                       (((x) & 0xff0000) >> 8) | (((x) & 0xff000000) >> 24))
#ifndef X64
    /* 58 b8 s4 s3 s2 s1 cd 80 */
    static const uint non_rt_1w =  reverse(0x58b80000 | (reverse(SYS_sigreturn) >> 16));
    static const uint non_rt_2w = reverse((reverse(SYS_sigreturn) << 16) | 0xcd80);
#endif
    /* b8 s4 s3 s2 s1 cd 80 XX */
    static const uint rt_1w = reverse(0xb8000000 | (reverse(SYS_rt_sigreturn) >> 8));
    static const uint rt_2w = reverse((reverse(SYS_rt_sigreturn) << 24) | 0x00cd8000);
    /* test rt first as it's the most common 
     * only 7 bytes here so we ignore the last one (becomes msb since little-endian)
     */
    if (*((uint *)pc) == rt_1w && (*((uint *)(pc+4)) & 0x00ffffff) == rt_2w) {
        if (len != NULL)
            *len = 7;
        return true;
    }
#ifndef X64
    if (*((uint *)pc) == non_rt_1w && *((uint *)(pc+4)) == non_rt_2w) {
        if (len != NULL)
            *len = 8;
        return true;
    }
#endif
    return false;
}


void
os_forge_exception(app_pc target_pc, exception_type_t type)
{
    /* PR 205136:
     * We want to deliver now, and the caller expects us not to return.
     * We have two alternatives:
     * 1) Emulate stack frame, and call transfer_to_dispatch() for delivery.  We
     *    may not know how to fill out every field of the frame (cr2, etc.).  Plus,
     *    we have problems w/ default actions (PR 205310) but we have to solve
     *    those long-term anyway.  We also have to create different frames based on
     *    whether app intercepts via rt or not.
     * 2) Call SYS_tgkill from a special location that our handler can
     *    recognize and know it's a signal meant for the app and that the
     *    interrupted DR can be discarded.  We'd then essentially repeat 1,
     *    but modifying the kernel-generated frame.  We'd have to always
     *    intercept SIGILL.
     * I'm going with #1 for now b/c the common case is simpler.
     */
    dcontext_t *dcontext = get_thread_private_dcontext();
    thread_sig_info_t *info = (thread_sig_info_t *) dcontext->signal_field;
    char frame_plus_xstate[sizeof(sigframe_rt_t) + AVX_FRAME_EXTRA];
    sigframe_rt_t *frame = (sigframe_rt_t *) frame_plus_xstate;
    int sig;
    where_am_i_t cur_whereami = dcontext->whereami;
    switch (type) {
    case ILLEGAL_INSTRUCTION_EXCEPTION: sig = SIGILL; break;
    case UNREADABLE_MEMORY_EXECUTION_EXCEPTION: sig = SIGSEGV; break;
    case IN_PAGE_ERROR_EXCEPTION: /* fall-through: Windows only */
    default: ASSERT_NOT_REACHED(); sig = SIGSEGV; break;
    }

    LOG(GLOBAL, LOG_ASYNCH, 1, "os_forge_exception sig=%d\n", sig);

    /* since we always delay delivery, we always want an rt frame.  we'll convert
     * to a plain frame on delivery.
     */
    memset(frame, 0, sizeof(*frame));
    frame->info.si_signo = sig;
#ifndef X64
    frame->sig = sig;
    frame->pinfo = &frame->info;
    frame->puc = (void *) &frame->uc;
#endif
    frame->uc.uc_mcontext.fpstate = (struct _fpstate *)
        ALIGN_FORWARD(frame_plus_xstate + sizeof(*frame), XSTATE_ALIGNMENT);
    mcontext_to_sigcontext(&frame->uc.uc_mcontext, get_mcontext(dcontext));
    frame->uc.uc_mcontext.SC_XIP = (reg_t) target_pc;
    /* we'll fill in fpstate at delivery time
     * FIXME: it seems to work w/o filling in the other state:
     * I'm leaving segments, cr2, etc. all zero.
     * Note that x64 kernel restore_sigcontext() only restores cs: it
     * claims onus is on app's signal handler for other segments.
     * We should try to share part of the GET_OWN_CONTEXT macro used for
     * Windows.  Or we can switch to approach #2.
     */
    if (sig_has_restorer(info, sig))
        frame->pretcode = (char *) info->app_sigaction[sig]->restorer;
    else
        frame->pretcode = (char *) dynamorio_sigreturn;

    /* We assume that we do not need to translate the context when forged.
     * If we did, we'd move this below enter_nolinking() (and update
     * record_pending_signal() to do the translation).
     */
    record_pending_signal(dcontext, sig, &frame->uc, frame, true/*forged*/
                          _IF_CLIENT(NULL));

    /* For most callers this is not necessary and we only do it to match
     * the Windows usage model: but for forging from our own handler,
     * this is good b/c it resets us to the base of dstack.
     */
    /* tell dispatch() why we're coming there */
    dcontext->whereami = WHERE_TRAMPOLINE;
    KSTART(dispatch_num_exits);
    /* we overload the meaning of the sigreturn linkstub */
    set_last_exit(dcontext, (linkstub_t *) get_sigreturn_linkstub());
    if (is_couldbelinking(dcontext))
        enter_nolinking(dcontext, NULL, false);
    transfer_to_dispatch(dcontext, get_mcontext(dcontext),
                         cur_whereami != WHERE_FCACHE &&
                         cur_whereami != WHERE_SIGNAL_HANDLER
                         /*full_DR_state*/);
    ASSERT_NOT_REACHED();
}

void
os_request_fatal_coredump(const char *msg)
{
    set_default_signal_action(SIGSEGV);
    SYSLOG_INTERNAL_ERROR("Crashing the process deliberately for a core dump!");
    /* We try both the SIGKILL and the immediate crash since on some platforms
     * the SIGKILL is delayed and on others the *-1 is hanging(?): should investigate
     */
    dynamorio_syscall(SYS_kill, 2, get_process_id(), SIGSEGV);
    *((int *)PTR_UINT_MINUS_1) = 0;
    /* To enable getting a coredump just make sure that rlimits are
     * not preventing getting one, e.g. ulimit -c unlimited
     */
    return;
}

void
os_request_live_coredump(const char *msg)
{
#ifdef VMX86_SERVER
   if (os_in_vmkernel_userworld()) {
      vmk_request_live_coredump(msg);
      return;
   }
#endif
   LOG(GLOBAL, LOG_ASYNCH, 1, "LiveCoreDump unsupported (PR 365105).  "
       "Continuing execution without a core.\n");
   return;
}

void
os_dump_core(const char *msg)
{
    /* FIXME Case 3408: fork stack dump crashes on 2.6 kernel, so moving the getchar
     * ahead to aid in debugging */
    if (TEST(DUMPCORE_WAIT_FOR_DEBUGGER, dynamo_options.dumpcore_mask)) {
        SYSLOG_INTERNAL_ERROR("looping so you can use gdb to attach to pid %s",
                              get_application_pid());
        IF_CLIENT_INTERFACE(SYSLOG(SYSLOG_CRITICAL, WAITING_FOR_DEBUGGER, 2,
                                   get_application_name(), get_application_pid()));
        /* getchar() can hit our own vsyscall hook (from PR 212570); typically we
         * want to attach and not continue anyway, so doing an infinite loop:
         */
        while (true)
            thread_yield();
    }

    if (DYNAMO_OPTION(live_dump)) {
        os_request_live_coredump(msg);
    }

    if (TEST(DUMPCORE_INCLUDE_STACKDUMP, dynamo_options.dumpcore_mask)) {
        /* fork, dump core, then use gdb to get a stack dump
         * we can get into an infinite loop if there's a seg fault
         * in the process of doing this -- so we have a do-once test,
         * and if it failed we do the no-symbols dr callstack dump
         */
        static bool tried_stackdump = false;
        if (!tried_stackdump) {
            tried_stackdump = true;
            stackdump();
        } else {
            static bool tried_calldump = false;
            if  (!tried_calldump) {
                tried_calldump = true;
                dump_dr_callstack(STDERR);
            }
        }
    }

    if (!DYNAMO_OPTION(live_dump)) {
        os_request_fatal_coredump(msg);
        ASSERT_NOT_REACHED();
    }
}

#ifdef RETURN_AFTER_CALL
bool
at_known_exception(dcontext_t *dcontext, app_pc target_pc, app_pc source_fragment)
{
    /* There is a known exception in signal restorers and the Linux dynamic symbol resoulution */
    /* The latter we assume it is the only other recurring known exception, 
       so the first time we pattern match to help make sure it is indeed _dl_runtime_resolve
       (since with LD_BIND_NOW it will never be called).  After that we compare with the known value. */

    static app_pc known_exception = 0;
    thread_sig_info_t *info = (thread_sig_info_t *) dcontext->signal_field;

    LOG(THREAD, LOG_INTERP, 1, "RCT: testing for KNOWN exception "PFX" "PFX"\n",
        target_pc, source_fragment);

    /* Check if this is a signal return.   
       FIXME: we should really get that from the frame itself.
       Since currently grabbing restorer only when copying a frame,
       this will work with nested signals only if they all have same restorer
       (I haven't seen restorers other than the one in libc)
    */
    if (target_pc == info->signal_restorer_retaddr) {
        LOG(THREAD, LOG_INTERP, 1, "RCT: KNOWN exception this is a signal restorer --ok \n");
        STATS_INC(ret_after_call_signal_restorer);
        return true;
    }

    if (source_fragment == known_exception) {
        LOG(THREAD, LOG_INTERP, 1, "RCT: KNOWN exception again _dl_runtime_resolve --ok\n");
        return true;
    }

    if (known_exception == 0) {
        /* It works for the UNIX loader hack in  _dl_runtime_resolve */
        /* The offending sequence in ld-linux.so is
           <_dl_runtime_resolve>:
           c270: 5a                      pop    %edx
           c271: 59                      pop    %ecx
           c272: 87 04 24                xchg   %eax,(%esp)
           c275: c2 08 00                ret    $0x8
        */
        /* The same code also is in 0000c280 <_dl_runtime_profile>
           It maybe that either one or the other is ever used. 
           Although performancewise this pattern matching is very cheap, 
           for stricter security we assume only one is used in a session.
        */
        /* FIXME: This may change with future versions of libc, tested on
         * RH8 and RH9 only.  Also works for whatever libc was in ubuntu 7.10.
         */
        /* However it does not work for ubuntu 8.04 where the code sequence has
         * changed to the still similar :
         * 2c50:  5a                   pop    %edx 
         * 2c51:  8b 0c 24             mov    (%esp) -> %ecx 
         * 2c54:  89 04 24             mov    %eax -> (%esp) 
         * 2c57:  8b 44 24 04          mov    0x04(%esp) -> %eax 
         * 2c5b:  c2 0c 00             ret    $0xc
         * So we check for that sequence too.
         */
        static const byte DL_RUNTIME_RESOLVE_MAGIC_1[8] =
          /* pop edx, pop ecx; xchg eax, (esp) ret 8 */
          {0x5a, 0x59, 0x87, 0x04, 0x24, 0xc2, 0x08, 0x00};
        static const byte DL_RUNTIME_RESOLVE_MAGIC_2[14] = 
          /* pop edx, mov (esp)->ecx, mov eax->(esp), mov 4(esp)->eax, ret 12 */
          {0x5a, 0x8b, 0x0c, 0x24, 0x89, 0x04, 0x24, 0x8b, 0x44, 0x24,
           0x04, 0xc2, 0x0c, 0x00};
        byte buf[MAX(sizeof(DL_RUNTIME_RESOLVE_MAGIC_1),
                     sizeof(DL_RUNTIME_RESOLVE_MAGIC_2))]= {0};

        if ((safe_read(source_fragment, sizeof(DL_RUNTIME_RESOLVE_MAGIC_1), buf)
             && memcmp(buf, DL_RUNTIME_RESOLVE_MAGIC_1,
                       sizeof(DL_RUNTIME_RESOLVE_MAGIC_1)) == 0) ||
            (safe_read(source_fragment, sizeof(DL_RUNTIME_RESOLVE_MAGIC_2), buf)
             && memcmp(buf, DL_RUNTIME_RESOLVE_MAGIC_2,
                       sizeof(DL_RUNTIME_RESOLVE_MAGIC_2)) == 0)) {
            LOG(THREAD, LOG_INTERP, 1, "RCT: KNOWN exception this is "
                "_dl_runtime_resolve --ok \n");
            known_exception = source_fragment;
            return true;
        } else {
            return false;
        }
    }
    return false;
}
#endif /* RETURN_AFTER_CALL */

/***************************************************************************
 * ITIMERS
 *
 * We support combining an app itimer with a DR itimer for each of the 3 types
 * (PR 204556).
 */

static inline uint64
timeval_to_usec(struct timeval *t1)
{
    return ((uint64)(t1->tv_sec))*1000000 + t1->tv_usec;
}

static inline void
usec_to_timeval(uint64 usec, struct timeval *t1)
{
    t1->tv_sec = (long) usec / 1000000;
    t1->tv_usec = (long) usec % 1000000;
}

static void
init_itimer(dcontext_t *dcontext, bool first)
{
    thread_sig_info_t *info = (thread_sig_info_t *) dcontext->signal_field;
    ASSERT(info != NULL);
    ASSERT(!info->shared_itimer); /* else inherit */
    LOG(THREAD, LOG_ASYNCH, 2, "thread has private itimers%s\n",
        os_itimers_thread_shared() ? " (for now)" : "");
    if (os_itimers_thread_shared()) {
        /* we have to allocate now even if no itimer is installed until later,
         * so that all child threads point to the same data
         */
        info->itimer = (thread_itimer_info_t (*)[NUM_ITIMERS])
            global_heap_alloc(sizeof(*info->itimer) HEAPACCT(ACCT_OTHER));
    } else {
        /* for simplicity and parllel w/ shared we allocate proactively */
        info->itimer = (thread_itimer_info_t (*)[NUM_ITIMERS])
            heap_alloc(dcontext, sizeof(*info->itimer) HEAPACCT(ACCT_OTHER));
    }
    memset(info->itimer, 0, sizeof(*info->itimer));
    if (first) {
        /* see if app has set up an itimer before we were loaded */
        struct itimerval prev;
        int rc;
        int which;
        for (which = 0; which < NUM_ITIMERS; which++) {
            rc = getitimer_syscall(which, &prev);
            ASSERT(rc == SUCCESS);
            (*info->itimer)[which].app.interval = timeval_to_usec(&prev.it_interval);
            (*info->itimer)[which].app.value = timeval_to_usec(&prev.it_value);
        }
    }
}

/* Up to caller to hold lock for shared itimers */
static bool
set_actual_itimer(dcontext_t *dcontext, int which, thread_sig_info_t *info,
                  bool enable)
{
    struct itimerval val;
    int rc;
    ASSERT(info != NULL && info->itimer != NULL);
    ASSERT(which >= 0 && which < NUM_ITIMERS);
    if (enable) {
        ASSERT(!info->shared_itimer || self_owns_recursive_lock(info->shared_itimer_lock));
        usec_to_timeval((*info->itimer)[which].actual.interval, &val.it_interval);
        usec_to_timeval((*info->itimer)[which].actual.value, &val.it_value);
        LOG(THREAD, LOG_ASYNCH, 2, "installing itimer %d interval="INT64_FORMAT_STRING
            ", value="INT64_FORMAT_STRING"\n", which,
            (*info->itimer)[which].actual.interval, (*info->itimer)[which].actual.value);
    } else {
        LOG(THREAD, LOG_ASYNCH, 2, "disabling itimer %d\n", which);
        memset(&val, 0, sizeof(val));
    }
    rc = setitimer_syscall(which, &val, NULL);
    return (rc == SUCCESS);
}

/* Caller should hold lock */
bool
itimer_new_settings(dcontext_t *dcontext, int which, bool app_changed)
{
    struct itimerval val;
    bool res = true;
    int rc;
    thread_sig_info_t *info = (thread_sig_info_t *) dcontext->signal_field;
    ASSERT(info != NULL && info->itimer != NULL);
    ASSERT(which >= 0 && which < NUM_ITIMERS);
    ASSERT(!info->shared_itimer || self_owns_recursive_lock(info->shared_itimer_lock));
    /* the general strategy is to set the actual value to the smaller,
     * update the larger on each signal, and when the larger becomes
     * smaller do a one-time swap for the remaining
     */
    if ((*info->itimer)[which].dr.interval > 0 &&
        ((*info->itimer)[which].app.interval == 0 ||
         (*info->itimer)[which].dr.interval < (*info->itimer)[which].app.interval))
        (*info->itimer)[which].actual.interval = (*info->itimer)[which].dr.interval;
    else
        (*info->itimer)[which].actual.interval = (*info->itimer)[which].app.interval;

    if ((*info->itimer)[which].actual.value > 0) {
        if ((*info->itimer)[which].actual.interval == 0 &&
            (*info->itimer)[which].dr.value == 0 &&
            (*info->itimer)[which].app.value == 0) {
            (*info->itimer)[which].actual.value = 0;
            res = set_actual_itimer(dcontext, which, info, false/*disabled*/);
        } else {
            /* one of app or us has an in-flight timer which we should not interrupt.
             * but, we already set the new requested value (for app or us), so we
             * need to update the actual value so we subtract properly.
             */
            rc = getitimer_syscall(which, &val);
            ASSERT(rc == SUCCESS);
            uint64 left = timeval_to_usec(&val.it_value);
            if (!app_changed &&
                (*info->itimer)[which].actual.value == (*info->itimer)[which].app.value)
                (*info->itimer)[which].app.value = left;
            if (app_changed &&
                (*info->itimer)[which].actual.value == (*info->itimer)[which].dr.value)
                (*info->itimer)[which].dr.value = left;
            (*info->itimer)[which].actual.value = left;
        }
    } else {
        if ((*info->itimer)[which].dr.value > 0 &&
            ((*info->itimer)[which].app.value == 0 ||
             (*info->itimer)[which].dr.value < (*info->itimer)[which].app.value))
            (*info->itimer)[which].actual.value = (*info->itimer)[which].dr.value;
        else {
            (*info->itimer)[which].actual.value = (*info->itimer)[which].app.value;
        }
        res = set_actual_itimer(dcontext, which, info, true/*enable*/);
    }
    return res;
}

bool
set_itimer_callback(dcontext_t *dcontext, int which, uint millisec,
                    void (*func)(dcontext_t *, priv_mcontext_t *),
                    void (*func_api)(dcontext_t *, dr_mcontext_t *))
{
    thread_sig_info_t *info = (thread_sig_info_t *) dcontext->signal_field;
    bool rc;
    if (which < 0 || which >= NUM_ITIMERS) {
        CLIENT_ASSERT(false, "invalid itimer type");
        return false;
    }
    if (func == NULL && func_api == NULL && millisec != 0) {
        CLIENT_ASSERT(false, "invalid function");
        return false;
    }
    ASSERT(info != NULL && info->itimer != NULL);
    if (info->shared_itimer)
        acquire_recursive_lock(info->shared_itimer_lock);
    (*info->itimer)[which].dr.interval = ((uint64)millisec)*1000;
    (*info->itimer)[which].dr.value = (*info->itimer)[which].dr.interval;
    (*info->itimer)[which].cb = func;
    (*info->itimer)[which].cb_api = func_api;
    rc = itimer_new_settings(dcontext, which, false/*us*/);
    if (info->shared_itimer)
        release_recursive_lock(info->shared_itimer_lock);
    return rc;
}

uint
get_itimer_frequency(dcontext_t *dcontext, int which)
{
    thread_sig_info_t *info = (thread_sig_info_t *) dcontext->signal_field;
    uint ms = 0;
    if (which < 0 || which >= NUM_ITIMERS) {
        CLIENT_ASSERT(false, "invalid itimer type");
        return 0;
    }
    ASSERT(info != NULL && info->itimer != NULL);
    if (info->shared_itimer)
        acquire_recursive_lock(info->shared_itimer_lock);
    ms = (*info->itimer)[which].dr.interval / 1000;
    if (info->shared_itimer)
        release_recursive_lock(info->shared_itimer_lock);
    return ms;
}

static bool
handle_alarm(dcontext_t *dcontext, int sig, kernel_ucontext_t *ucxt)
{
    thread_sig_info_t *info = (thread_sig_info_t *) dcontext->signal_field;
    ASSERT(info != NULL && info->itimer != NULL);
    struct sigcontext *sc = (struct sigcontext *) &(ucxt->uc_mcontext);
    int which = 0;
    bool invoke_cb = false, pass_to_app = false, reset_timer_manually = false;
    bool acquired_lock = false;

    /* i#471: suppress alarms coming in after exit */
    if (dynamo_exited)
        return pass_to_app;

    if (sig == SIGALRM)
        which = ITIMER_REAL;
    else if (sig == SIGVTALRM)
        which = ITIMER_VIRTUAL;
    else if (sig == SIGPROF)
        which = ITIMER_PROF;
    else
        ASSERT_NOT_REACHED();
    LOG(THREAD, LOG_ASYNCH, 2, "received alarm %d @"PFX"\n", which, sc->SC_XIP);

    /* This alarm could have interrupted an app thread making an itimer syscall */
    if (info->shared_itimer) {
        if (self_owns_recursive_lock(info->shared_itimer_lock)) {
            /* What can we do?  We just go ahead and hope conflicting writes work out. 
             * We don't re-acquire in case app was in middle of acquiring.
             */
        } else if (try_recursive_lock(info->shared_itimer_lock) ||
                   try_recursive_lock(info->shared_itimer_lock)) {
            acquired_lock = true;
        } else {
            /* Heuristic: if fail twice then assume interrupted lock routine.
             * What can we do?  Just continue and hope conflicting writes work out.
             */
        }
    }
    if ((*info->itimer)[which].app.value > 0) {
        /* Alarm could have been on its way when app value changed */
        if ((*info->itimer)[which].app.value >= (*info->itimer)[which].actual.value) {
            (*info->itimer)[which].app.value -= (*info->itimer)[which].actual.value;
            LOG(THREAD, LOG_ASYNCH, 2,
                "\tapp value is now %d\n", (*info->itimer)[which].app.value);
            if ((*info->itimer)[which].app.value == 0) {
                pass_to_app = true;
                (*info->itimer)[which].app.value = (*info->itimer)[which].app.interval;
            } else
                reset_timer_manually = true;
        }
    }
    if ((*info->itimer)[which].dr.value > 0) {
        /* Alarm could have been on its way when DR value changed */
        if ((*info->itimer)[which].dr.value >= (*info->itimer)[which].actual.value) {
            (*info->itimer)[which].dr.value -= (*info->itimer)[which].actual.value;
            LOG(THREAD, LOG_ASYNCH, 2,
                "\tdr value is now %d\n", (*info->itimer)[which].dr.value);
            if ((*info->itimer)[which].dr.value == 0) {
                invoke_cb = true;
                (*info->itimer)[which].dr.value = (*info->itimer)[which].dr.interval;
            } else
                reset_timer_manually = true;
        }
    }
    /* for efficiency we let the kernel reset the value to interval if
     * there's only one timer
     */
    if (reset_timer_manually) {
        (*info->itimer)[which].actual.value = 0;
        itimer_new_settings(dcontext, which, true/*doesn't matter: actual.value==0*/);
    } else
        (*info->itimer)[which].actual.value = (*info->itimer)[which].actual.interval;

    if (invoke_cb) {
        /* invoke after setting new itimer value */
        /* we save stack space by allocating superset dr_mcontext_t */
        dr_mcontext_t dmc;
        priv_mcontext_t *mc;
        dr_mcontext_init(&dmc);
        mc = dr_mcontext_as_priv_mcontext(&dmc);
        sigcontext_to_mcontext(mc, sc);
        if ((*info->itimer)[which].cb != NULL)
            (*(*info->itimer)[which].cb)(dcontext, mc);
        else
            (*(*info->itimer)[which].cb_api)(dcontext, &dmc);
    }
    if (info->shared_itimer && acquired_lock)
        release_recursive_lock(info->shared_itimer_lock);
    return pass_to_app;
}

/* Starts itimer if stopped, or increases refcount of existing itimer if already
 * started.  It is *not* safe to call this more than once for the same thread,
 * since it will inflate the refcount and prevent cleanup.
 */
void
start_itimer(dcontext_t *dcontext)
{
    thread_sig_info_t *info = (thread_sig_info_t *) dcontext->signal_field;
    ASSERT(info != NULL && info->itimer != NULL);
    bool start = false;
    if (info->shared_itimer) {
        acquire_recursive_lock(info->shared_itimer_lock);
        (*info->shared_itimer_underDR)++;
        start = (*info->shared_itimer_underDR == 1);
    } else
        start = true;
    if (start) {
        /* Enable all DR itimers b/c at least one thread in this set of threads
         * sharing itimers is under DR control
         */
        int which;
        LOG(THREAD, LOG_ASYNCH, 2, "starting DR itimers from thread %d\n",
            get_thread_id());
        for (which = 0; which < NUM_ITIMERS; which++) {
            /* May have already been started if there was no stop_itimer() since
             * init time
             */
            if ((*info->itimer)[which].dr.value == 0 &&
                (*info->itimer)[which].dr.interval > 0) {
                (*info->itimer)[which].dr.value = (*info->itimer)[which].dr.interval;
                itimer_new_settings(dcontext, which, false/*!app*/);
            }
        }
    }
    if (info->shared_itimer)
        release_recursive_lock(info->shared_itimer_lock);
}

/* Decrements the itimer refcount, and turns off the itimer once there are no
 * more threads listening for it.  It is not safe to call this more than once on
 * the same thread.
 */
void
stop_itimer(dcontext_t *dcontext)
{
    thread_sig_info_t *info = (thread_sig_info_t *) dcontext->signal_field;
    ASSERT(info != NULL && info->itimer != NULL);
    bool stop = false;
    if (info->shared_itimer) {
        acquire_recursive_lock(info->shared_itimer_lock);
        ASSERT(*info->shared_itimer_underDR > 0);
        (*info->shared_itimer_underDR)--;
        stop = (*info->shared_itimer_underDR == 0);
    } else
        stop = true;
    if (stop) {
        /* Disable all DR itimers b/c this set of threads sharing this
         * itimer is now compmletely native
         */
        int which;
        LOG(THREAD, LOG_ASYNCH, 2, "stopping DR itimers from thread %d\n",
            get_thread_id());
        for (which = 0; which < NUM_ITIMERS; which++) {
            if ((*info->itimer)[which].dr.value > 0) {
                (*info->itimer)[which].dr.value = 0;
                if ((*info->itimer)[which].app.value > 0) {
                    (*info->itimer)[which].actual.interval =
                        (*info->itimer)[which].app.interval;
                } else
                    set_actual_itimer(dcontext, which, info, false/*disable*/);
            }
        }
    }
    if (info->shared_itimer)
        release_recursive_lock(info->shared_itimer_lock);
}

/* handle app itimer syscalls */
/* handle_pre_alarm also calls this function and passes NULL as prev_timer */
void
handle_pre_setitimer(dcontext_t *dcontext,
                     int which, const struct itimerval *new_timer,
                     struct itimerval *prev_timer)
{
    if (new_timer == NULL || which < 0 || which >= NUM_ITIMERS)
        return;
    thread_sig_info_t *info = (thread_sig_info_t *) dcontext->signal_field;
    ASSERT(info != NULL && info->itimer != NULL);
    struct itimerval val;
    if (safe_read(new_timer, sizeof(val), &val)) {
        if (info->shared_itimer)
            acquire_recursive_lock(info->shared_itimer_lock);
        /* save a copy in case the syscall fails */
        (*info->itimer)[which].app_saved = (*info->itimer)[which].app;
        (*info->itimer)[which].app.interval = timeval_to_usec(&val.it_interval);
        (*info->itimer)[which].app.value = timeval_to_usec(&val.it_value);
        LOG(THREAD, LOG_ASYNCH, 2,
            "app setitimer type=%d interval="SZFMT" value="SZFMT"\n",
            which, (*info->itimer)[which].app.interval,
            (*info->itimer)[which].app.value);
        itimer_new_settings(dcontext, which, true/*app*/);
        if (info->shared_itimer)
            release_recursive_lock(info->shared_itimer_lock);
    }
}

void
handle_post_setitimer(dcontext_t *dcontext, bool success,
                      int which, const struct itimerval *new_timer,
                      struct itimerval *prev_timer)
{
    if (new_timer == NULL || which < 0 || which >= NUM_ITIMERS) {
        ASSERT(new_timer == NULL || !success);
        return;
    }
    thread_sig_info_t *info = (thread_sig_info_t *) dcontext->signal_field;
    ASSERT(info != NULL && info->itimer != NULL);
    ASSERT(which >= 0 && which < NUM_ITIMERS);
    if (!success && new_timer != NULL) {
        if (info->shared_itimer)
            acquire_recursive_lock(info->shared_itimer_lock);
        /* restore saved pre-syscall settings */
        (*info->itimer)[which].app = (*info->itimer)[which].app_saved;
        itimer_new_settings(dcontext, which, true/*app*/);
        if (info->shared_itimer)
            release_recursive_lock(info->shared_itimer_lock);
    }
    if (success && prev_timer != NULL)
        handle_post_getitimer(dcontext, success, which, prev_timer);
}

void
handle_post_getitimer(dcontext_t *dcontext, bool success,
                      int which, struct itimerval *cur_timer)
{
    thread_sig_info_t *info = (thread_sig_info_t *) dcontext->signal_field;
    ASSERT(info != NULL && info->itimer != NULL);
    if (success) {
        /* write succeeded for kernel but we're user and can have races */
        struct timeval val;
        DEBUG_DECLARE(bool ok;)
        ASSERT(which >= 0 && which < NUM_ITIMERS);
        ASSERT(cur_timer != NULL);
        if (info->shared_itimer)
            acquire_recursive_lock(info->shared_itimer_lock);
        usec_to_timeval((*info->itimer)[which].app.interval, &val);
        IF_DEBUG(ok = )
            safe_write_ex(&cur_timer->it_interval, sizeof(val), &val, NULL);
        ASSERT(ok);
        if (safe_read(&cur_timer->it_value, sizeof(val), &val)) {
            /* subtract the difference between last-asked-for value
             * and current value to reflect elapsed time
             */
            uint64 left = (*info->itimer)[which].app.value -
                ((*info->itimer)[which].actual.value - timeval_to_usec(&val));
            usec_to_timeval(left, &val);
            IF_DEBUG(ok = )
                safe_write_ex(&cur_timer->it_value, sizeof(val), &val, NULL);
            ASSERT(ok);
        } else
            ASSERT_NOT_REACHED();
        if (info->shared_itimer)
            release_recursive_lock(info->shared_itimer_lock);
    }
}

/* handle app alarm syscall */
/* alarm uses the same itimer and could be defined in terms of setitimer */
void
handle_pre_alarm(dcontext_t *dcontext, unsigned int sec)
{
    struct itimerval val;
    val.it_interval.tv_usec = 0;
    val.it_interval.tv_sec = 0;
    val.it_value.tv_usec = 0;
    val.it_value.tv_sec = sec;
    handle_pre_setitimer(dcontext, ITIMER_REAL, &val, NULL);
}

void
handle_post_alarm(dcontext_t *dcontext, bool success, unsigned int sec)
{
    /* alarm is always successful, so do nothing in post */
    ASSERT(success);
    return;
}

/***************************************************************************/

/* Returns whether to pass on to app */
static bool
handle_suspend_signal(dcontext_t *dcontext, kernel_ucontext_t *ucxt)
{
    os_thread_data_t *ostd = (os_thread_data_t *) dcontext->os_field;
    struct sigcontext *sc = (struct sigcontext *) &(ucxt->uc_mcontext);
    kernel_sigset_t prevmask;
    ASSERT(ostd != NULL);

    if (ostd->terminate) {
         /* PR 297902: exit this thread, without using any stack */
        LOG(THREAD, LOG_ASYNCH, 2, "handle_suspend_signal: exiting\n");
        if (kernel_futex_support) {
            /* can't use stack once set terminated to 1 so in asm we do:
             *   ostd->terminated = 1;
             *   futex_wake_all(&ostd->terminated);
             */
            volatile int *term = &ostd->terminated;
            asm("mov %0, %%"ASM_XAX : : "m"(term));
            asm("movl $1,(%"ASM_XAX")");
            asm("jmp dynamorio_futex_wake_and_exit");
        } else {
            ostd->terminated = 1;
            asm("jmp dynamorio_sys_exit");
        }
        ASSERT_NOT_REACHED();
        return false;
    }

    /* If suspend_count is 0, we are not trying to suspend this thread
     * (thread_resume() may have already decremented suspend_count to 0, but
     * thread_suspend() will not send a signal until this thread unsets
     * ostd->suspended, so not having a lock around the suspend_count read is
     * ok), so pass signal to app.
     * If we are trying or have already suspended this thread, our own
     * thread_suspend() will not send a 2nd suspend signal until we are
     * completely resumed, so we can distinguish app uses of SUSPEND_SIGNAL.  We
     * can't have a race between the read and write of suspended_sigcxt b/c
     * signals are blocked.  It's fine to have a race and reorder the app's
     * signal w/ DR's.
     */
    if (ostd->suspend_count == 0 || ostd->suspended_sigcxt != NULL)
        return true; /* pass to app */

    ostd->suspended_sigcxt = sc;

    /* We're sitting on our sigaltstack w/ all signals blocked.  We're
     * going to stay here but unblock all signals so we don't lose any
     * delivered while we're waiting.  We're at a safe enough point to
     * re-enter master_signal_handler().  We use a mutex in
     * thread_{suspend,resume} to prevent our own re-suspension signal
     * from arriving before we've re-blocked on the resume.
     */
    sigprocmask_syscall(SIG_SETMASK, &ucxt->uc_sigmask, &prevmask,
                        sizeof(ucxt->uc_sigmask));

    LOG(THREAD, LOG_ASYNCH, 2, "handle_suspend_signal: suspended now\n");
    /* We cannot use mutexes here as we have interrupted DR at an
     * arbitrary point!  Thus we can't use the event_t routines.
     * However, the existing synch and check above prevent any
     * re-entrance here, and our cond vars target just a single thread,
     * so we can get away w/o a mutex.
     */
    /* Notify thread_suspend that it can now return, as this thread is
     * officially suspended now and is ready for thread_{get,set}_mcontext.
     */
    ASSERT(ostd->suspended == 0);
    ostd->suspended = 1;
    futex_wake_all(&ostd->suspended);
    /* i#96/PR 295561: use futex(2) if available */
    while (ostd->wakeup == 0) {
        /* Waits only if the wakeup flag is not set as 1. Return value
         * doesn't matter because the flag will be re-checked. 
         */
        futex_wait(&ostd->wakeup, 0);
        if (ostd->wakeup == 0) {
            /* If it still has to wait, give up the cpu. */
            thread_yield();
        }
    }
    LOG(THREAD, LOG_ASYNCH, 2, "handle_suspend_signal: awake now\n");

    /* re-block so our exit from master_signal_handler is not interrupted */
    sigprocmask_syscall(SIG_SETMASK, &prevmask, NULL, sizeof(prevmask));
    ostd->suspended_sigcxt = NULL;

    /* Notify thread_resume that it can return now, which (assuming
     * suspend_count is back to 0) means it's then safe to re-suspend. 
     */
    ostd->suspended = 0; /* reset prior to signalling thread_resume */
    ostd->resumed = 1;
    futex_wake_all(&ostd->resumed);

    if (ostd->retakeover) {
        ostd->retakeover = false;
        sig_take_over(sc);  /* no return */
        ASSERT_NOT_REACHED();
    }

    return false; /* do not pass to app */
}

/* PR 206278: for try/except we need to save the signal mask */
void
dr_setjmp_sigmask(dr_jmp_buf_t *buf)
{
    /* i#226/PR 492568: we rely on the kernel storing the prior mask in the
     * signal frame, so we do not need to store it on every setjmp, which
     * can be a performance hit.
     */
#ifdef DEBUG
    sigprocmask_syscall(SIG_SETMASK, NULL, &buf->sigmask, sizeof(buf->sigmask));
#endif
}

/* i#61/PR 211530: nudge on Linux.
 * Determines whether this is a nudge signal, and if so queues up a nudge,
 * or is an app signal.  Returns whether to pass the signal on to the app.
 */
static bool
handle_nudge_signal(dcontext_t *dcontext, siginfo_t *siginfo, kernel_ucontext_t *ucxt)
{
    struct sigcontext *sc = (struct sigcontext *) &(ucxt->uc_mcontext);
    nudge_arg_t *arg = (nudge_arg_t *) siginfo;
    instr_t instr;
    char buf[MAX_INSTR_LENGTH];

    /* Distinguish a nudge from an app signal.  An app using libc sigqueue()
     * will never have its signal mistaken as libc does not expose the siginfo_t
     * and always passes 0 for si_errno, so we're only worried beyond our
     * si_code check about an app using a raw syscall that is deliberately
     * trying to fool us.
     * While there is a lot of padding space in siginfo_t, the kernel doesn't
     * copy it through on SYS_rt_sigqueueinfo so we don't have room for any
     * dedicated magic numbers.  The client id could function as a magic
     * number for client nudges, but I don't think we want to kill the app
     * if an external nudger types the client id wrong.
     */
    if (siginfo->si_signo != NUDGESIG_SIGNUM
        /* PR 477454: remove the IF_NOT_VMX86 once we have nudge-arg support */
        IF_NOT_VMX86(|| siginfo->si_code != SI_QUEUE
                     || siginfo->si_errno == 0)) {
        return true; /* pass to app */
    }
#if defined(CLIENT_INTERFACE) && !defined(VMX86_SERVER)
    DODEBUG({
        if (TEST(NUDGE_GENERIC(client), arg->nudge_action_mask) &&
            !is_valid_client_id(arg->client_id)) {
            SYSLOG_INTERNAL_WARNING("received client nudge for invalid id=0x%x",
                                    arg->client_id);
        }
    });
#endif
    if (dynamo_exited || !dynamo_initialized || dcontext == NULL) {
        /* Ignore the nudge: too early, or too late.
         * Xref Windows handling of such cases in nudge.c: old case 5702, etc.
         * We do this before the illegal-instr check b/c it's unsafe to decode
         * if too early or too late.
         */
        SYSLOG_INTERNAL_WARNING("too-early or too-late nudge: ignoring");
        return false; /* do not pass to app */
    }

    /* As a further check, try to detect whether this was raised synchronously
     * from a real illegal instr: though si_code for that should not be
     * SI_QUEUE.  It's possible a nudge happened to come at a bad instr before
     * it faulted, or maybe the instr after a syscall or other wait spot is
     * illegal, but we'll live with that risk.
     */
    ASSERT(NUDGESIG_SIGNUM == SIGILL); /* else this check makes no sense */
    instr_init(dcontext, &instr);
    if (safe_read((byte *)sc->SC_XIP, sizeof(buf), buf) &&
        (decode(dcontext, (byte *)buf, &instr) == NULL ||
         /* check for ud2 (xref PR 523161) */
         instr_is_undefined(&instr))) {
        instr_free(dcontext, &instr);
        return true; /* pass to app */
    }
    instr_free(dcontext, &instr);

#ifdef VMX86_SERVER
    /* Treat as a client nudge until we have PR 477454 */
    if (siginfo->si_errno == 0) {
        arg->version = NUDGE_ARG_CURRENT_VERSION;
        arg->flags = 0;
        arg->nudge_action_mask = NUDGE_GENERIC(client);
        arg->client_id = 0;
        arg->client_arg = 0;
    }
#endif

    LOG(THREAD, LOG_ASYNCH, 1,
        "received nudge version=%u flags=0x%x mask=0x%x id=0x%08x arg=0x"
        ZHEX64_FORMAT_STRING"\n",
        arg->version, arg->flags, arg->nudge_action_mask,
        arg->client_id, arg->client_arg);
    SYSLOG_INTERNAL_INFO("received nudge mask=0x%x id=0x%08x arg=0x"ZHEX64_FORMAT_STRING,
                         arg->nudge_action_mask, arg->client_id, arg->client_arg);

    /* We need to handle the nudge at a safe, nolinking spot */
    if (safe_is_in_fcache(dcontext, (byte *)sc->SC_XIP, (byte*)sc->SC_XSP) &&
        dcontext->interrupted_for_nudge == NULL) {
        /* We unlink the interrupted fragment and skip any inlined syscalls to
         * bound the nudge delivery time.  If we already unlinked one we assume
         * that's sufficient.
         */
        fragment_t wrapper;
        fragment_t *f = fragment_pclookup(dcontext, (byte *)sc->SC_XIP, &wrapper);
        if (f != NULL) {
            if (unlink_fragment_for_signal(dcontext, f, (byte *)sc->SC_XIP))
                dcontext->interrupted_for_nudge = f;
        }
    }

    /* No lock is needed since thread-private and this signal is blocked now */
    nudge_add_pending(dcontext, arg);

    return false; /* do not pass to app */
}