/*-
 * Copyright (c) 1993, David Greenman
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
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	$Id: imgact.h,v 1.17 1998/03/02 05:47:43 peter Exp $
 */

#ifndef _SYS_IMGACT_H_
#define	_SYS_IMGACT_H_

struct image_params {
	struct proc *proc;	/* our process struct */
	struct execve_args *uap; /* syscall arguments */
	struct vnode *vp;	/* pointer to vnode of file to exec */
	struct vattr *attr;	/* attributes of file */
	const char *image_header; /* head of file to exec */
	char *stringbase;	/* base address of tmp string storage */
	char *stringp;		/* current 'end' pointer of tmp strings */
	int stringspace;	/* space left in tmp string storage area */
	int argc, envc;		/* count of argument and environment strings */
	char *argv0;		/* Replacement for argv[0] when interpreting */
	unsigned long entry_addr; /* entry address of target executable */
	char vmspace_destroyed;	/* flag - we've blown away original vm space */
	char interpreted;	/* flag - this executable is interpreted */
	char interpreter_name[64]; /* name of the interpreter */
	void *auxargs;		/* ELF Auxinfo structure pointer */
	struct vm_page *firstpage;	/* first page that we mapped */
	char *fname;            /* pointer to filename of executable (user space) */
};

#ifdef KERNEL
int	exec_check_permissions __P((struct image_params *));
int	exec_extract_strings __P((struct image_params *));
int	exec_new_vmspace __P((struct image_params *));
#endif

#endif /* !_SYS_IMGACT_H_ */
