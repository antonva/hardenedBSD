/*-
 * Copyright (c) 2015-2017 Oliver Pinter <oliver.pinter@HardenedBSD.org>
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
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD$
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include "opt_pax.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/extattr.h>
#include <sys/fcntl.h>
#include <sys/imgact.h>
#include <sys/imgact_elf.h>
#include <sys/ktr.h>
#include <sys/libkern.h>
#include <sys/limits.h>
#include <sys/namei.h>
#include <sys/pax.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/uio.h>
#include <sys/vnode.h>

FEATURE(hbsdcontrol, "HardenedBSD's FS-EA based control subsystem.");

static int pax_hbsdcontrol_status = PAX_FEATURE_SIMPLE_ENABLED;
TUNABLE_INT("hardening.hbsdcontrol.status", &pax_hbsdcontrol_status);

static bool pax_hbsdcontrol_active(void);

struct pax_feature_entry {
	const char		*fs_ea_attribute;
	const pax_flag_t	 feature_bit;
};

const struct pax_feature_entry pax_features[] = {
	{"hbsd.pax.aslr",			PAX_NOTE_ASLR},
	{"hbsd.pax.noaslr",			PAX_NOTE_NOASLR},
	{"hbsd.pax.segvguard",			PAX_NOTE_SEGVGUARD},
	{"hbsd.pax.nosegvguard",		PAX_NOTE_NOSEGVGUARD},
	{"hbsd.pax.pageexec",			PAX_NOTE_PAGEEXEC},
	{"hbsd.pax.nopageexec",			PAX_NOTE_NOPAGEEXEC},
	{"hbsd.pax.mprotect",			PAX_NOTE_MPROTECT},
	{"hbsd.pax.nomprotect",			PAX_NOTE_NOMPROTECT},
	{"hbsd.pax.shlibrandom",		PAX_NOTE_SHLIBRANDOM},
	{"hbsd.pax.noshlibrandom",		PAX_NOTE_NOSHLIBRANDOM},
	{"hbsd.pax.disallow_map32bit",		PAX_NOTE_DISALLOWMAP32BIT},
	{"hbsd.pax.nodisallow_map32bit",	PAX_NOTE_NODISALLOWMAP32BIT},
	{NULL, 0}
};

#ifdef PAX_SYSCTLS
SYSCTL_DECL(_hardening_pax);

SYSCTL_NODE(_hardening_pax, OID_AUTO, hbsdcontrol, CTLFLAG_RD, 0,
    "FS-EA based control subsystem.");

SYSCTL_INT(_hardening_pax_hbsdcontrol, OID_AUTO, status,
    CTLFLAG_RDTUN|CTLFLAG_SECURE,
    &pax_hbsdcontrol_status, 0,
    "status: "
    "0 - disabled, "
    "1 - enabled");
#endif /* PAX_SYSCTLS */

int
pax_hbsdcontrol_parse_fsea_flags(struct thread *td, struct image_params *imgp)
{
	struct uio uio;
	struct iovec iov;
	unsigned char feature_status;
	bool feature_present[nitems(pax_features)];
	unsigned char *fsea_list;
	size_t fsea_list_size;
	size_t fsea_attrname_len;
	pax_flag_t parsed_flags;
	unsigned char entry_size;
	int i, j;
	int error;

	if (!pax_hbsdcontrol_active()) {
		imgp->pax.req_extattr_flags = 0;
		return (0);
	}

	feature_status = 0;
	fsea_list = NULL;
	fsea_list_size = 0;
	fsea_attrname_len = 0;
	parsed_flags = 0;

	for (i = 0; i < nitems(feature_present); i++)
		feature_present[i] = false;

	/* Query the size of extended attribute names list. */
	error = VOP_LISTEXTATTR(imgp->vp, EXTATTR_NAMESPACE_SYSTEM, NULL,
	    &fsea_list_size, NULL, td);

	/*
	 *  Fast path:
	 *   - FS-EA not supported by the FS, or
	 *   - other error, or
	 *   - no FS-EA assigned for the file.
	 */
	if (error != 0 || fsea_list_size == 0) {
		/* Use system defaults. */
		imgp->pax.req_extattr_flags = 0;

		if (error == EOPNOTSUPP) {
			/*
			 * Use system default, without
			 * returning an error.
			 */
			return (0);
		}

		return (error);
	}

	if (fsea_list_size > IOSIZE_MAX) {
		error = ENOMEM;
		goto out;
	}

	fsea_list = malloc(fsea_list_size, M_TEMP, M_WAITOK|M_ZERO);

	memset(&uio, 0, sizeof(uio));
	memset(&iov, 0, sizeof(iov));
	iov.iov_base = fsea_list;
	iov.iov_len = fsea_list_size;
	uio.uio_iov = &iov;
	uio.uio_iovcnt = 1;
	uio.uio_offset = 0;
	uio.uio_rw = UIO_READ;
	uio.uio_segflg = UIO_SYSSPACE;
	uio.uio_td = td;
	uio.uio_resid = fsea_list_size;

	/* Query the FS-EA list. */
	error = VOP_LISTEXTATTR(imgp->vp, EXTATTR_NAMESPACE_SYSTEM, &uio, NULL, NULL, td);
	if (error != 0)
		goto out;

	/*
	 * Create a filter from existing hbsd.pax attributes.
	 */
	for (i = 0; i < fsea_list_size; ) {
		/* See VOP_LISTEXTATTR(9) for the details. */
		entry_size = fsea_list[i++];

		for (j = 0; pax_features[j].fs_ea_attribute != NULL; j++) {
			fsea_attrname_len = strlen(pax_features[j].fs_ea_attribute);

			/* 
			 * Fast path:
			 * compare the string's len without the the ending zero
			 * with the attribute name stored without zero.
			 */
			if (fsea_attrname_len != entry_size)
				continue;

			/*
			 * Compare the strings as byte arrays without the ending zeros
			 * after we checked its lengths.
			 */
			if (memcmp(pax_features[j].fs_ea_attribute, &fsea_list[i],
			    fsea_attrname_len) == 0) {
				feature_present[j] = true;
				/*
				 * Once we found the relevant enty,
				 * start over a new round for a new entry.
				 */
				break;
			}
		}
		i += entry_size;
	}

	for (i = 0; pax_features[i].fs_ea_attribute != NULL; i++) {
		if (feature_present[i] == false)
			continue;

		memset(&uio, 0, sizeof(uio));
		memset(&iov, 0, sizeof(iov));
		feature_status = 0;

		iov.iov_base = &feature_status;
		iov.iov_len = sizeof(feature_status);
		uio.uio_iov = &iov;
		uio.uio_iovcnt = 1;
		uio.uio_offset = 0;
		uio.uio_rw = UIO_READ;
		uio.uio_segflg = UIO_SYSSPACE;
		uio.uio_td = td;
		uio.uio_resid = sizeof(feature_status);

		/*
		 * Use NOCRED as credential to always get the extended attributes,
		 * even if the user execs a program.
		 */
		error = VOP_GETEXTATTR(imgp->vp, EXTATTR_NAMESPACE_SYSTEM,
		    pax_features[i].fs_ea_attribute, &uio, NULL, NOCRED, td);

		if (error == 0) {
			switch (feature_status) {
			case '0':
				parsed_flags &= ~pax_features[i].feature_bit;
				break;
			case '1':
				parsed_flags |= pax_features[i].feature_bit;
				break;
			default:
				printf("%s: unknown state: %c [0x%x]\n",
				    pax_features[i].fs_ea_attribute,
				    feature_status, feature_status);
				break;
			}
		} else {
			switch (error) {
			case ENOATTR:
				/* Ignore non-existing attribute error. */
				break;
			default:
				/*
				 * For other errors, reset the parsed_flags
				 * and use the system defaults.
				 */
				goto out;
			}
		}
	}

out:
	free(fsea_list, M_TEMP);

	/* In case of error, reset to the system defaults. */
	if (error)
		parsed_flags = 0;

	imgp->pax.req_extattr_flags = parsed_flags;

	return (error);
}

static bool
pax_hbsdcontrol_active(void)
{

	if ((pax_hbsdcontrol_status & PAX_FEATURE_SIMPLE_ENABLED) == PAX_FEATURE_SIMPLE_ENABLED)
		return (true);

	if ((pax_hbsdcontrol_status & PAX_FEATURE_SIMPLE_DISABLED) == PAX_FEATURE_SIMPLE_DISABLED)
		return (false);

	return (true);
}

static void
pax_hbsdcontrol_sysinit(void)
{

	switch (pax_hbsdcontrol_status) {
	case PAX_FEATURE_SIMPLE_DISABLED:
	case PAX_FEATURE_SIMPLE_ENABLED:
		break;
	default:
		printf("[HBSD CONTROL] WARNING, invalid settings in loader.conf!"
		    " (pax_hbsdcontrol_status = %d)\n", pax_hbsdcontrol_status);
		pax_hbsdcontrol_status = PAX_FEATURE_SIMPLE_ENABLED;
		break;
	}
	if (bootverbose) {
		printf("[HBSD CONTROL] status: %s\n",
		    pax_status_simple_str[pax_hbsdcontrol_status]);
	}
}
SYSINIT(pax_hbsdcontrol, SI_SUB_PAX, SI_ORDER_SECOND, pax_hbsdcontrol_sysinit, NULL);

