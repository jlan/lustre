/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License version 2 for more details (a copy is included
 * in the LICENSE file that accompanied this code).
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; If not, see
 * http://www.sun.com/software/products/lustre/docs/GPLv2.pdf
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa Clara,
 * CA 95054 USA or visit www.sun.com if you need additional information or
 * have any questions.
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2007, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/include/obd_ost.h
 *
 * Data structures for object storage targets and client: OST & OSC's
 *
 * See also lustre_idl.h for wire formats of requests.
 */

#ifndef _LUSTRE_OST_H
#define _LUSTRE_OST_H

#include <obd_class.h>

struct osc_brw_async_args {
        struct obdo       *aa_oa;
        int                aa_requested_nob;
        int                aa_nio_count;
        obd_count          aa_page_count;
        int                aa_resends;
        struct brw_page  **aa_ppga;
        struct client_obd *aa_cli;
        cfs_list_t         aa_oaps;
        struct obd_capa   *aa_ocapa;
        struct cl_req     *aa_clerq;
};

#define osc_grant_args osc_brw_async_args
struct osc_async_args {
        struct obd_info   *aa_oi;
};

struct osc_setattr_args {
        struct obdo         *sa_oa;
        obd_enqueue_update_f sa_upcall;
        void                *sa_cookie;
};

struct osc_enqueue_args {
        struct obd_export        *oa_exp;
        int                      *oa_flags;
        obd_enqueue_update_f      oa_upcall;
        void                     *oa_cookie;
        struct ost_lvb           *oa_lvb;
        struct lustre_handle     *oa_lockh;
        struct ldlm_enqueue_info *oa_ei;
};

#if 0
int osc_extent_blocking_cb(struct ldlm_lock *lock,
                           struct ldlm_lock_desc *new, void *data,
                           int flag);
#endif

#define POSTID LPU64":"LPU64
/** 
 * Build DLM resource name from object id & group for osc-ost extent lock.
 */
static inline struct ldlm_res_id *osc_build_res_name(__u64 id, __u64 gr,
                                                     struct ldlm_res_id *name)
{
        memset(name, 0, sizeof *name);
        name->name[0] = id;
        name->name[1] = gr;
        return name;
}

/**
 * Return true if the resource is for the object identified by this id & group.
 */
static inline int osc_res_name_eq(__u64 id, __u64 gr, struct ldlm_res_id *name)
{
        return name->name[0] == id && name->name[1] == gr;
}

#endif
