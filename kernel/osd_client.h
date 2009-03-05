#ifndef _FS_CEPH_OSD_CLIENT_H
#define _FS_CEPH_OSD_CLIENT_H

#include <linux/radix-tree.h>
#include <linux/completion.h>

#include "types.h"
#include "osdmap.h"

/*
 * All data objects are stored within a cluster/cloud of OSDs, or
 * "object storage devices."  (Note that Ceph OSDs have _nothing_ to
 * do with the T10 OSD extensions to SCSI.)  Ceph OSDs are simply
 * remote daemons serving up and coordinating consistent and safe
 * access to storage.
 *
 * Cluster membership and the mapping of data objects onto storage devices
 * are described by the osd map.
 *
 * We keep track of pending OSD requests (read, write), resubmit
 * requests to different OSDs when the cluster topology/data layout
 * change, or retry the affected requests when the communications
 * channel with an OSD is reset.
 */

struct ceph_msg;
struct ceph_snap_context;
struct ceph_osd_request;

/*
 * completion callback for async writepages
 */
typedef void (*ceph_osdc_callback_t)(struct ceph_osd_request *);

/* an in-flight request */
struct ceph_osd_request {
	u64             r_tid;              /* unique for this client */
	struct ceph_msg  *r_request;
	struct ceph_msg  *r_reply;
	int               r_result;
	int               r_flags;     /* any additional flags for the osd */
	int               r_aborted;   /* set if we cancel this request */

	atomic_t          r_ref;
	struct completion r_completion;       /* on completion, or... */
	ceph_osdc_callback_t r_callback;      /* ...async callback. */
	struct inode *r_inode;                /* needed for async write */
	struct writeback_control *r_wbc;

	int               r_last_osd;   /* pg osds */
	struct ceph_entity_addr r_last_osd_addr;
	unsigned long     r_timeout_stamp;

	union ceph_pg     r_pgid;             /* placement group */
	struct ceph_snap_context *r_snapc;    /* snap context for writes */
	unsigned          r_num_pages;        /* size of page array (follows) */
	struct page      *r_pages[0];         /* pages for data payload */
};

struct ceph_osd_client {
	struct ceph_client     *client;

	struct ceph_osdmap     *osdmap;       /* current map */
	struct rw_semaphore    map_sem;
	struct completion      map_waiters;
	u64                    last_requested_map;

	struct mutex           request_mutex;
	u64                    timeout_tid;   /* tid of timeout triggering rq */
	u64                    last_tid;      /* tid of last request */
	struct radix_tree_root request_tree;  /* pending requests, by tid */
	int                    num_requests;
	struct delayed_work    timeout_work;
};

extern void ceph_osdc_init(struct ceph_osd_client *osdc,
			   struct ceph_client *client);
extern void ceph_osdc_stop(struct ceph_osd_client *osdc);

extern void ceph_osdc_handle_reset(struct ceph_osd_client *osdc,
				   struct ceph_entity_addr *addr);

extern void ceph_osdc_handle_reply(struct ceph_osd_client *osdc,
				   struct ceph_msg *msg);
extern void ceph_osdc_handle_map(struct ceph_osd_client *osdc,
				 struct ceph_msg *msg);

/* incoming read messages use this to discover which pages to read
 * the data payload into. */
extern int ceph_osdc_prepare_pages(void *p, struct ceph_msg *m, int want);

extern struct ceph_osd_request *ceph_osdc_new_request(struct ceph_osd_client *,
				      struct ceph_file_layout *layout,
				      struct ceph_vino vino,
				      u64 offset, u64 *len, int op,
				      struct ceph_snap_context *snapc,
				      int do_sync, u32 truncate_seq,
				      u64 truncate_size);
extern void ceph_osdc_put_request(struct ceph_osd_request *req);

extern int ceph_osdc_readpage(struct ceph_osd_client *osdc,
			      struct ceph_vino vino,
			      struct ceph_file_layout *layout,
			      u64 off, u64 len,
			      u32 truncate_seq, u64 truncate_size,
			      struct page *page);
extern int ceph_osdc_readpages(struct ceph_osd_client *osdc,
			       struct address_space *mapping,
			       struct ceph_vino vino,
			       struct ceph_file_layout *layout,
			       u64 off, u64 len,
			       u32 truncate_seq, u64 truncate_size,
			       struct list_head *page_list, int nr_pages);

extern int ceph_osdc_writepages(struct ceph_osd_client *osdc,
				struct ceph_vino vino,
				struct ceph_file_layout *layout,
				struct ceph_snap_context *sc,
				u64 off, u64 len,
				u32 truncate_seq, u64 truncate_size,
				struct page **pagevec, int nr_pages);
extern int ceph_osdc_writepages_start(struct ceph_osd_client *osdc,
				      struct ceph_osd_request *req,
				      u64 len,
				      int nr_pages);

extern int ceph_osdc_sync_read(struct ceph_osd_client *osdc,
			       struct ceph_vino vino,
			       struct ceph_file_layout *layout,
			       u64 off, u64 len,
			       u32 truncate_seq, u64 truncate_size,
			       char __user *data);
extern int ceph_osdc_sync_write(struct ceph_osd_client *osdc,
				struct ceph_vino vino,
				struct ceph_file_layout *layout,
				struct ceph_snap_context *sc,
				u64 off, u64 len,
				u32 truncate_seq, u64 truncate_size,
				const char __user *data);

#endif

