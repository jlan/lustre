/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  lustre/fid/fid_handler.c
 *  Lustre Sequence Manager
 *
 *  Copyright (c) 2006 Cluster File Systems, Inc.
 *   Author: Yury Umanets <umka@clusterfs.com>
 *
 *   This file is part of the Lustre file system, http://www.lustre.org
 *   Lustre is a trademark of Cluster File Systems, Inc.
 *
 *   You may have signed or agreed to another license before downloading
 *   this software.  If so, you are bound by the terms and conditions
 *   of that agreement, and the following does not apply to you.  See the
 *   LICENSE file included with this distribution for more information.
 *
 *   If you did not agree to a different license, then this copy of Lustre
 *   is open source software; you can redistribute it and/or modify it
 *   under the terms of version 2 of the GNU General Public License as
 *   published by the Free Software Foundation.
 *
 *   In either case, Lustre is distributed in the hope that it will be
 *   useful, but WITHOUT ANY WARRANTY; without even the implied warranty
 *   of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   license text for more details.
 */

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_FID

#ifdef __KERNEL__
# include <libcfs/libcfs.h>
# include <linux/module.h>
#else /* __KERNEL__ */
# include <liblustre.h>
#endif

#include <obd.h>
#include <obd_class.h>
#include <dt_object.h>
#include <md_object.h>
#include <obd_support.h>
#include <lustre_req_layout.h>
#include <lustre_fid.h>
#include "fid_internal.h"

#ifdef __KERNEL__
/*
 * Sequence space, starts from 0x400 to have first 0x400 sequences used for
 * special purposes. This means that if we have seq-with 10000 fids, we have
 * ~10M fids reserved for special purposes (igifs, etc.).
 */
const struct lu_range LUSTRE_SEQ_SPACE_RANGE = {
        (0x400),
        ((__u64)~0ULL)
};
EXPORT_SYMBOL(LUSTRE_SEQ_SPACE_RANGE);

/* zero range, used for init and other purposes */
const struct lu_range LUSTRE_SEQ_ZERO_RANGE = {
        0,
        0
};
EXPORT_SYMBOL(LUSTRE_SEQ_ZERO_RANGE);

/* assigns client to sequence controller node */
int seq_server_set_cli(struct lu_server_seq *seq,
                       struct lu_client_seq *cli,
                       const struct lu_context *ctx)
{
        int rc = 0;
        ENTRY;

        if (cli == NULL) {
                CDEBUG(D_INFO|D_WARNING, "%s: detached "
                       "sequence mgr client %s\n",
                       seq->lss_name, cli->lcs_name);
                seq->lss_cli = cli;
                RETURN(0);
        }

        if (seq->lss_cli) {
                CERROR("%s: sequence-controller is already "
                       "assigned\n", seq->lss_name);
                RETURN(-EINVAL);
        }

        CDEBUG(D_INFO|D_WARNING, "%s: attached "
               "sequence client %s\n", seq->lss_name,
               cli->lcs_name);

        /* asking client for new range, assign that range to ->seq_super and
         * write seq state to backing store should be atomic. */
        down(&seq->lss_sem);

        /* assign controller */
        seq->lss_cli = cli;

        /* get new range from controller only if super-sequence is not yet
         * initialized from backing store or something else. */
        if (range_is_zero(&seq->lss_super)) {
                rc = seq_client_alloc_super(cli);
                if (rc) {
                        up(&seq->lss_sem);
                        CERROR("can't allocate super-sequence, "
                               "rc %d\n", rc);
                        RETURN(rc);
                }

                /* take super-seq from client seq mgr */
                LASSERT(range_is_sane(&cli->lcs_range));

                seq->lss_super = cli->lcs_range;

                /* save init seq to backing store. */
                rc = seq_store_write(seq, ctx);
                if (rc) {
                        CERROR("can't write sequence state, "
                               "rc = %d\n", rc);
                }
        }

        up(&seq->lss_sem);
        RETURN(rc);
}
EXPORT_SYMBOL(seq_server_set_cli);

/* on controller node, allocate new super sequence for regular sequence
 * server. */
static int __seq_server_alloc_super(struct lu_server_seq *seq,
                                    struct lu_range *in,
                                    struct lu_range *out,
                                    const struct lu_context *ctx)
{
        struct lu_range *space = &seq->lss_space;
        int rc;
        ENTRY;

        LASSERT(range_is_sane(space));

        if (in != NULL) {
                CDEBUG(D_INFO, "%s: recovery - use input range "
                       DRANGE"\n", seq->lss_name, PRANGE(in));

                if (in->lr_start > space->lr_start)
                        space->lr_start = in->lr_start;
                *out = *in;
        } else {
                if (range_space(space) < seq->lss_super_width) {
                        CWARN("sequences space is going to exhaust soon. "
                              "Can allocate only "LPU64" sequences\n",
                              range_space(space));
                        *out = *space;
                        space->lr_start = space->lr_end;
                } else if (range_is_exhausted(space)) {
                        CERROR("sequences space is exhausted\n");
                        RETURN(-ENOSPC);
                } else {
                        range_alloc(out, space, seq->lss_super_width);
                }
        }

        rc = seq_store_write(seq, ctx);
        if (rc) {
                CERROR("can't save state, rc = %d\n",
                       rc);
                RETURN(rc);
        }

        CDEBUG(D_INFO, "%s: allocated super-sequence "
               DRANGE"\n", seq->lss_name, PRANGE(out));

        RETURN(rc);
}

int seq_server_alloc_super(struct lu_server_seq *seq,
                           struct lu_range *in,
                           struct lu_range *out,
                           const struct lu_context *ctx)
{
        int rc;
        ENTRY;

        down(&seq->lss_sem);
        rc = __seq_server_alloc_super(seq, in, out, ctx);
        up(&seq->lss_sem);

        RETURN(rc);
}

static int __seq_server_alloc_meta(struct lu_server_seq *seq,
                                   struct lu_range *in,
                                   struct lu_range *out,
                                   const struct lu_context *ctx)
{
        struct lu_range *super = &seq->lss_super;
        int rc = 0;
        ENTRY;

        LASSERT(range_is_sane(super));

        /* 
         * This is recovery case. Adjust super range if input range looks like
         * it is allocated from new super.
         */
        if (in != NULL) {
                CDEBUG(D_INFO, "%s: recovery - use input range "
                       DRANGE"\n", seq->lss_name, PRANGE(in));

                if (range_is_exhausted(super)) {
                        LASSERT(in->lr_start > super->lr_start);

                        /* 
                         * Server cannot send to client empty range, this is why
                         * we check here that range from client is "newer" than
                         * exhausted super.
                         */
                        super->lr_start = in->lr_start;
                        
                        super->lr_end = super->lr_start +
                                LUSTRE_SEQ_SUPER_WIDTH;
                } else {
                        /* 
                         * Update super start by start from client's range. End
                         * should not be changed if range was not exhausted.
                         */
                        if (in->lr_start > super->lr_start)
                                super->lr_start = in->lr_start;
                }

                *out = *in;
        } else {
                /*
                 * XXX: avoid cascading RPCs using kind of async preallocation
                 * when meta-sequence is close to exhausting.
                 */
                if (range_is_exhausted(super)) {
                        if (!seq->lss_cli) {
                                CERROR("no seq-controller client is setup\n");
                                RETURN(-EOPNOTSUPP);
                        }

                        rc = seq_client_alloc_super(seq->lss_cli);
                        if (rc) {
                                CERROR("can't allocate new super-sequence, "
                                       "rc %d\n", rc);
                                RETURN(rc);
                        }

                        /* saving new range into allocation space. */
                        *super = seq->lss_cli->lcs_range;
                        LASSERT(range_is_sane(super));
                }
                range_alloc(out, super, seq->lss_meta_width);
        }

        rc = seq_store_write(seq, ctx);
        if (rc) {
                CERROR("can't save state, rc = %d\n",
		       rc);
        }

        if (rc == 0) {
                CDEBUG(D_INFO, "%s: allocated meta-sequence "
                       DRANGE"\n", seq->lss_name, PRANGE(out));
        }

        RETURN(rc);
}

int seq_server_alloc_meta(struct lu_server_seq *seq,
                          struct lu_range *in,
                          struct lu_range *out,
                          const struct lu_context *ctx)
{
        int rc;
        ENTRY;

        down(&seq->lss_sem);
        rc = __seq_server_alloc_meta(seq, in, out, ctx);
        up(&seq->lss_sem);

        RETURN(rc);
}

static int seq_server_handle(struct lu_site *site,
                             const struct lu_context *ctx,
                             __u32 opc, struct lu_range *in,
                             struct lu_range *out)
{
        int rc;
        ENTRY;
        
        switch (opc) {
        case SEQ_ALLOC_META:
                if (!site->ls_server_seq) {
                        CERROR("sequence-server is not "
                               "initialized\n");
                        RETURN(-EINVAL);
                }
                rc = seq_server_alloc_meta(site->ls_server_seq,
                                           in, out, ctx);
                break;
        case SEQ_ALLOC_SUPER:
                if (!site->ls_control_seq) {
                        CERROR("sequence-controller is not "
                               "initialized\n");
                        RETURN(-EINVAL);
                }
                rc = seq_server_alloc_super(site->ls_control_seq,
                                            in, out, ctx);
                break;
        default:
                rc = -EINVAL;
                break;
        }

        RETURN(rc);
}

static int seq_req_handle(struct ptlrpc_request *req,
                          struct seq_thread_info *info)
{
        struct lu_range *out, *in = NULL;
        struct lu_site *site;
        int rc = -EPROTO;
        __u32 *opc;
        ENTRY;

        site = req->rq_export->exp_obd->obd_lu_dev->ld_site;
        LASSERT(site != NULL);
			
        rc = req_capsule_pack(&info->sti_pill);
        if (rc)
                RETURN(rc);

        opc = req_capsule_client_get(&info->sti_pill,
                                     &RMF_SEQ_OPC);
        if (opc != NULL) {
                const struct lu_context *ctx;
                
                out = req_capsule_server_get(&info->sti_pill,
                                             &RMF_SEQ_RANGE);
                if (out == NULL)
                        RETURN(-EPROTO);

                if (lustre_msg_get_flags(req->rq_reqmsg) & MSG_REPLAY) {
                        in = req_capsule_client_get(&info->sti_pill,
                                                    &RMF_SEQ_RANGE);

                        LASSERT(!range_is_zero(in) && range_is_sane(in));
                }
        
                ctx = req->rq_svc_thread->t_ctx;
                rc = seq_server_handle(site, ctx, *opc, in, out);
        } else
                rc = -EPROTO;

        RETURN(rc);
}

static void *seq_thread_init(const struct lu_context *ctx,
                             struct lu_context_key *key)
{
        struct seq_thread_info *info;

        /*
         * check that no high order allocations are incurred.
         */
        CLASSERT(CFS_PAGE_SIZE >= sizeof *info);
        OBD_ALLOC_PTR(info);
        if (info == NULL)
                info = ERR_PTR(-ENOMEM);
        return info;
}

static void seq_thread_fini(const struct lu_context *ctx,
                            struct lu_context_key *key, void *data)
{
        struct seq_thread_info *info = data;
        OBD_FREE_PTR(info);
}

struct lu_context_key seq_thread_key = {
        .lct_tags = LCT_MD_THREAD,
        .lct_init = seq_thread_init,
        .lct_fini = seq_thread_fini
};

static void seq_thread_info_init(struct ptlrpc_request *req,
                                 struct seq_thread_info *info)
{
        int i;

        /* mark rep buffer as req-layout stuff expects */
        for (i = 0; i < ARRAY_SIZE(info->sti_rep_buf_size); i++)
                info->sti_rep_buf_size[i] = -1;

        /* init request capsule */
        req_capsule_init(&info->sti_pill, req, RCL_SERVER,
                         info->sti_rep_buf_size);

        req_capsule_set(&info->sti_pill, &RQF_SEQ_QUERY);
}

static void seq_thread_info_fini(struct seq_thread_info *info)
{
        req_capsule_fini(&info->sti_pill);
}

static int seq_handle(struct ptlrpc_request *req)
{
        const struct lu_context *ctx;
        struct seq_thread_info *info;
        int rc;
        
        ctx = req->rq_svc_thread->t_ctx;
        LASSERT(ctx != NULL);
        LASSERT(ctx->lc_thread == req->rq_svc_thread);

        info = lu_context_key_get(ctx, &seq_thread_key);
        LASSERT(info != NULL);

        seq_thread_info_init(req, info);
        rc = seq_req_handle(req, info);
        seq_thread_info_fini(info);

        return rc;
}

/*
 * Entry point for handling FLD RPCs called from MDT.
 */
int seq_query(struct com_thread_info *info)
{
        return seq_handle(info->cti_pill.rc_req);
}
EXPORT_SYMBOL(seq_query);

static void seq_server_proc_fini(struct lu_server_seq *seq);

#ifdef LPROCFS
static int seq_server_proc_init(struct lu_server_seq *seq)
{
        int rc;
        ENTRY;

        seq->lss_proc_dir = lprocfs_register(seq->lss_name,
                                             proc_lustre_root,
                                             NULL, NULL);
        if (IS_ERR(seq->lss_proc_dir)) {
                rc = PTR_ERR(seq->lss_proc_dir);
                RETURN(rc);
        }

        rc = lprocfs_add_vars(seq->lss_proc_dir,
                              seq_server_proc_list, seq);
        if (rc) {
                CERROR("can't init sequence manager "
                       "proc, rc %d\n", rc);
                GOTO(out_cleanup, rc);
        }

        RETURN(0);

out_cleanup:
        seq_server_proc_fini(seq);
        return rc;
}

static void seq_server_proc_fini(struct lu_server_seq *seq)
{
        ENTRY;
        if (seq->lss_proc_dir != NULL) {
                if (!IS_ERR(seq->lss_proc_dir))
                        lprocfs_remove(seq->lss_proc_dir);
                seq->lss_proc_dir = NULL;
        }
        EXIT;
}
#else
static int seq_server_proc_init(struct lu_server_seq *seq)
{
        return 0;
}

static void seq_server_proc_fini(struct lu_server_seq *seq)
{
        return;
}
#endif

int seq_server_init(struct lu_server_seq *seq,
                    struct dt_device *dev,
                    const char *prefix,
                    enum lu_mgr_type type,
                    const struct lu_context *ctx)
{
        int rc, is_srv = (type == LUSTRE_SEQ_SERVER);
        ENTRY;

	LASSERT(dev != NULL);
        LASSERT(prefix != NULL);

        seq->lss_cli = NULL;
        seq->lss_type = type;
        sema_init(&seq->lss_sem, 1);

        seq->lss_super_width = LUSTRE_SEQ_SUPER_WIDTH;
        seq->lss_meta_width = LUSTRE_SEQ_META_WIDTH;

        snprintf(seq->lss_name, sizeof(seq->lss_name), "%s-%s-%s",
                 LUSTRE_SEQ_NAME, (is_srv ? "srv" : "ctl"),
                 prefix);

        seq->lss_space = LUSTRE_SEQ_SPACE_RANGE;
        seq->lss_super = LUSTRE_SEQ_ZERO_RANGE;

        rc = seq_store_init(seq, ctx, dev);
        if (rc)
                GOTO(out, rc);

        /* request backing store for saved sequence info */
        rc = seq_store_read(seq, ctx);
        if (rc == -ENODATA) {
                CDEBUG(D_INFO|D_WARNING, "%s: no data on "
                       "storage was found, %s\n", seq->lss_name,
                       is_srv ? "wait for controller attach" :
                       "this is first controller run");
        } else if (rc) {
		CERROR("can't read sequence state, rc = %d\n",
		       rc);
		GOTO(out, rc);
	}

        rc  = seq_server_proc_init(seq);
        if (rc)
		GOTO(out, rc);

	EXIT;
out:
	if (rc) {
		seq_server_fini(seq, ctx);
        } else {
                CDEBUG(D_INFO|D_WARNING, "%s Sequence Manager\n",
                       (is_srv ? "Server" : "Controller"));
        }
	return rc;
}
EXPORT_SYMBOL(seq_server_init);

void seq_server_fini(struct lu_server_seq *seq,
                     const struct lu_context *ctx)
{
        ENTRY;

        seq_server_proc_fini(seq);
        seq_store_fini(seq, ctx);

        EXIT;
}
EXPORT_SYMBOL(seq_server_fini);

static int fid_init(void)
{
	ENTRY;
	RETURN(0);
}

static int fid_fini(void)
{
	ENTRY;
	RETURN(0);
}

static int __init fid_mod_init(void)
{
        /* init caches if any */
        fid_init();
        return 0;
}

static void __exit fid_mod_exit(void)
{
        /* free caches if any */
        fid_fini();
        return;
}

MODULE_AUTHOR("Cluster File Systems, Inc. <info@clusterfs.com>");
MODULE_DESCRIPTION("Lustre FID Module");
MODULE_LICENSE("GPL");

cfs_module(fid, "0.1.0", fid_mod_init, fid_mod_exit);
#endif
