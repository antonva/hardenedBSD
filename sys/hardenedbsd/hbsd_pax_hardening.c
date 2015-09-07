/*-
 * Copyright (c) 2014, by Shawn Webb <shawn.webb at hardenedbsd.org>
 * Copyright (c) 2014, by Oliver Pinter <oliver.pinter@hardenedbsd.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * $FreeBSD$
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include "opt_compat.h"
#include "opt_pax.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/imgact.h>
#include <sys/imgact_elf.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/sysent.h>
#include <sys/syslimits.h>
#include <sys/stat.h>
#include <sys/pax.h>
#include <sys/proc.h>
#include <sys/elf_common.h>
#include <sys/mount.h>
#include <sys/sysctl.h>
#include <sys/vnode.h>
#include <sys/queue.h>
#include <sys/libkern.h>
#include <sys/jail.h>
#include <sys/mman.h>
#include <sys/libkern.h>
#include <sys/exec.h>
#include <sys/kthread.h>

#include <vm/pmap.h>
#include <vm/vm_map.h>
#include <vm/vm_extern.h>

#include <machine/elf.h>

FEATURE(pax_hardening, "Various hardening features.");

#if __FreeBSD_version < 1100000
#define	kern_unsetenv	unsetenv
#endif

#ifdef PAX_HARDENING
static int pax_disallow_map32bit_status_global = PAX_FEATURE_OPTOUT;
static int pax_procfs_harden_global = PAX_FEATURE_SIMPLE_ENABLED;
static int pax_randomize_pids_global = PAX_FEATURE_SIMPLE_ENABLED;
static int pax_init_hardening_global = PAX_FEATURE_SIMPLE_ENABLED;
#else
static int pax_disallow_map32bit_status_global = PAX_FEATURE_OPTIN;
static int pax_procfs_harden_global = PAX_FEATURE_SIMPLE_DISABLED;
static int pax_randomize_pids_global = PAX_FEATURE_SIMPLE_DISABLED;
static int pax_init_hardening_global = PAX_FEATURE_SIMPLE_DISABLED;
#endif

static int sysctl_pax_disallow_map32bit(SYSCTL_HANDLER_ARGS);
static int sysctl_pax_procfs(SYSCTL_HANDLER_ARGS);

#ifdef PAX_SYSCTLS
SYSCTL_DECL(_hardening_pax);

SYSCTL_NODE(_hardening_pax, OID_AUTO, disallow_map32bit, CTLFLAG_RD, 0,
    "Disallow MAP_32BIT mode mmap(2) calls.");

SYSCTL_PROC(_hardening_pax_disallow_map32bit, OID_AUTO, status,
    CTLTYPE_INT|CTLFLAG_RWTUN|CTLFLAG_PRISON|CTLFLAG_SECURE,
    NULL, 0, sysctl_pax_disallow_map32bit, "I",
    "Restriction status:"
    "0 - disabled, "
    "1 - opt-in, "
    "2 - opt-out, "
    "3 - force enabled.");

SYSCTL_PROC(_hardening, OID_AUTO, procfs_harden,
    CTLTYPE_INT|CTLFLAG_RWTUN|CTLFLAG_SECURE,
    NULL, 0, sysctl_pax_procfs, "I",
    "Harden procfs, disabling write of /proc/pid/mem. "
    "0 - disabled, "
    "1 - enabled.");
#endif

TUNABLE_INT("hardening.pax.disallow_map32bit.status", &pax_disallow_map32bit_status_global);
TUNABLE_INT("hardening.procfs_harden", &pax_procfs_harden_global);
TUNABLE_INT("hardening.randomize_pids", &pax_randomize_pids_global);

static void
pax_hardening_sysinit(void)
{
	switch (pax_disallow_map32bit_status_global) {
	case PAX_FEATURE_DISABLED:
	case PAX_FEATURE_OPTIN:
	case PAX_FEATURE_OPTOUT:
	case PAX_FEATURE_FORCE_ENABLED:
		break;
	default:
		printf("[PAX HARDENING] WARNING, invalid settings in loader.conf!"
		    " (hardening.pax.disallow_map32bit.status = %d)\n",
		    pax_disallow_map32bit_status_global);
		pax_disallow_map32bit_status_global = PAX_FEATURE_FORCE_ENABLED;
	}
	printf("[PAX HARDENING] disallow MAP_32BIT mode mmap: %s\n",
	    pax_status_str[pax_disallow_map32bit_status_global]);

	switch (pax_procfs_harden_global) {
	case PAX_FEATURE_SIMPLE_DISABLED:
	case PAX_FEATURE_SIMPLE_ENABLED:
		break;
	default:
		printf("[PAX HARDENING] WARNING, invalid settings in loader.conf!"
		    " (hardening.procfs_harden = %d)\n", pax_procfs_harden_global);
		pax_procfs_harden_global = PAX_FEATURE_SIMPLE_ENABLED;
	}
	printf("[PAX HARDENING] procfs hardening: %s\n",
	    pax_status_simple_str[pax_procfs_harden_global]);

	switch (pax_randomize_pids_global) {
	case PAX_FEATURE_SIMPLE_DISABLED:
	case PAX_FEATURE_SIMPLE_ENABLED:
		break;
	default:
		printf("[PAX HARDENING] WARNING, invalid settings in loader.conf!"
		    " (hardening.randomize_pids = %d)\n", pax_randomize_pids_global);
		pax_randomize_pids_global = PAX_FEATURE_SIMPLE_ENABLED;
	}
	printf("[PAX HARDENING] randomize pids: %s\n",
	    pax_status_simple_str[pax_randomize_pids_global]);

	switch (pax_init_hardening_global) {
	case PAX_FEATURE_SIMPLE_DISABLED:
	case PAX_FEATURE_SIMPLE_ENABLED:
		break;
	default:
		pax_init_hardening_global = PAX_FEATURE_SIMPLE_ENABLED;
	}
	printf("[PAX HARDENING] unset insecure init variables: %s\n",
	    pax_status_simple_str[pax_init_hardening_global]);
}
SYSINIT(pax_hardening, SI_SUB_PAX, SI_ORDER_SECOND, pax_hardening_sysinit, NULL);

#ifdef PAX_SYSCTLS
static int
sysctl_pax_disallow_map32bit(SYSCTL_HANDLER_ARGS)
{
	struct prison *pr;
	int err, val;

	pr = pax_get_prison_td(req->td);

	val = pr->pr_hardening.hr_pax_disallow_map32bit_status;
	err = sysctl_handle_int(oidp, &val, sizeof(int), req);
	if (err || (req->newptr == NULL))
		return (err);

	switch (val) {
	case PAX_FEATURE_DISABLED:
	case PAX_FEATURE_OPTIN:
	case PAX_FEATURE_OPTOUT:
	case PAX_FEATURE_FORCE_ENABLED:
		if ((pr == NULL) || (pr == &prison0))
			pax_disallow_map32bit_status_global = val;

		pr->pr_hardening.hr_pax_disallow_map32bit_status = val;
		break;
	default:
		return (EINVAL);
	}

	return (0);
}

static int
sysctl_pax_procfs(SYSCTL_HANDLER_ARGS)
{
	struct prison *pr;
	int err, val;

	pr = pax_get_prison_td(req->td);

	val = pr->pr_hardening.hr_pax_procfs_harden;
	err = sysctl_handle_int(oidp, &val, sizeof(int), req);
	if (err || (req->newptr == NULL))
		return (err);

	if (val > 1 || val < -1)
		return (EINVAL);

	if (pr == &prison0)
		pax_procfs_harden_global = val;

	pr->pr_hardening.hr_pax_procfs_harden = val;

	return (0);
}
#endif

void
pax_hardening_init_prison(struct prison *pr)
{
	struct prison *pr_p;

	CTR2(KTR_PAX, "%s: Setting prison %s PaX variables\n",
	    __func__, pr->pr_name);

	if (pr == &prison0) {
		/* prison0 has no parent, use globals */
#ifdef MAP_32BIT
		pr->pr_hardening.hr_pax_disallow_map32bit_status =
		    pax_disallow_map32bit_status_global;
#endif
		pr->pr_hardening.hr_pax_procfs_harden =
		    pax_procfs_harden_global;
	} else {
		KASSERT(pr->pr_parent != NULL,
		   ("%s: pr->pr_parent == NULL", __func__));
		pr_p = pr->pr_parent;

#ifdef MAP_32BIT
		pr->pr_hardening.hr_pax_disallow_map32bit_status =
		    pr_p->pr_hardening.hr_pax_disallow_map32bit_status;
#endif
		pr->pr_hardening.hr_pax_procfs_harden =
		    pr_p->pr_hardening.hr_pax_procfs_harden;
	}
}

int
pax_disallow_map32bit_active(struct thread *td, int mmap_flags)
{
	uint32_t flags;

	if ((mmap_flags & MAP_32BIT) != MAP_32BIT)
		/*
		 * Fast path, the mmap request does not
		 * contains MAP_32BIT flag.
		 */
		return (false);

	/* XXXOP: pax_get_flags_td(...) here? */
	pax_get_flags(td->td_proc, &flags);

	CTR3(KTR_PAX, "%S: pid = %d p_pax = %x",
	    __func__, td->td_proc->p_pid, flags);

	if ((flags & PAX_NOTE_DISALLOWMAP32BIT) == PAX_NOTE_DISALLOWMAP32BIT)
		return (true);

	if ((flags & PAX_NOTE_NODISALLOWMAP32BIT) == PAX_NOTE_NODISALLOWMAP32BIT)
		return (false);

	return (true);
}

int
pax_procfs_harden(struct thread *td)
{
	struct prison *pr;

	pr = pax_get_prison_td(td);

	return (pr->pr_hardening.hr_pax_procfs_harden ? EPERM : 0);
}

uint32_t
pax_hardening_setup_flags(struct image_params *imgp, uint32_t mode)
{
	struct prison *pr;
	uint32_t flags, status;

	flags = 0;
	status = 0;

	pr = pax_get_prison(imgp->proc);
	status = pr->pr_hardening.hr_pax_disallow_map32bit_status;

	if (status == PAX_FEATURE_DISABLED) {
		flags &= ~PAX_NOTE_DISALLOWMAP32BIT;
		flags |= PAX_NOTE_NODISALLOWMAP32BIT;

		return (flags);
	}

	if (status == PAX_FEATURE_FORCE_ENABLED) {
		flags &= ~PAX_NOTE_NODISALLOWMAP32BIT;
		flags |= PAX_NOTE_DISALLOWMAP32BIT;

		return (flags);
	}

	if (status == PAX_FEATURE_OPTIN) {
		if (mode & PAX_NOTE_DISALLOWMAP32BIT) {
			flags |= PAX_NOTE_DISALLOWMAP32BIT;
			flags &= ~PAX_NOTE_NODISALLOWMAP32BIT;
		} else {
			flags &= ~PAX_NOTE_DISALLOWMAP32BIT;
			flags |= PAX_NOTE_NODISALLOWMAP32BIT;
		}

		return (flags);
	}

	if (status == PAX_FEATURE_OPTOUT) {
		if (mode & PAX_NOTE_NODISALLOWMAP32BIT) {
			flags |= PAX_NOTE_NODISALLOWMAP32BIT;
			flags &= ~PAX_NOTE_DISALLOWMAP32BIT;
		} else {
			flags &= ~PAX_NOTE_NODISALLOWMAP32BIT;
			flags |= PAX_NOTE_DISALLOWMAP32BIT;
		}

		return (flags);
	}

	/* Unknown status, force MAP32 restriction. */
	flags |= PAX_NOTE_DISALLOWMAP32BIT;
	flags &= ~PAX_NOTE_NODISALLOWMAP32BIT;

	return (flags);
}

extern int randompid;

static void
pax_randomize_pids(void *dummy __unused)
{
	int modulus;

	if (pax_randomize_pids_global == PAX_FEATURE_SIMPLE_DISABLED)
		return;

	modulus = pid_max - 200;

	sx_xlock(&allproc_lock);
	randompid = arc4random() % modulus + 100;
	sx_xunlock(&allproc_lock);
}
SYSINIT(pax_randomize_pids, SI_SUB_KTHREAD_INIT, SI_ORDER_MIDDLE+1,
    pax_randomize_pids, NULL);

static void
pax_init_hardening(void *dummy __unused)
{
	/*
	 * Never should be made available from the loader / outside
	 * the pax_init_hardening_global variable.
	 */
	if (pax_init_hardening_global == PAX_FEATURE_SIMPLE_DISABLED)
		return;

	kern_unsetenv("init_chroot");
	kern_unsetenv("init_path");
	kern_unsetenv("init_script");
	kern_unsetenv("init_shell");
}
SYSINIT(pax_init_hardening, SI_SUB_PAX, SI_ORDER_ANY,
    pax_init_hardening, NULL);

