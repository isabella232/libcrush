#include <linux/module.h>
#include <linux/parser.h>
#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/seq_file.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/version.h>

/* debug levels; defined in super.h */

/*
 * global debug value.
 *  0 = quiet.
 *
 * if the per-file debug level >= 0, then that overrides this  global
 * debug level.
 */
int ceph_debug = 1;

/*
 * if true, send output to KERN_INFO (console) instead of KERN_DEBUG.
 */
int ceph_debug_console;

/* for this file */
int ceph_debug_super = -1;

#define DOUT_VAR ceph_debug_super
#define DOUT_PREFIX "super: "
#include "super.h"
#include "ktcp.h"

#include <linux/statfs.h>
#include "mon_client.h"

void ceph_dispatch(void *p, struct ceph_msg *msg);
void ceph_peer_reset(void *p, struct ceph_entity_name *peer_name);


/*
 * super ops
 */

static int ceph_write_inode(struct inode *inode, int unused)
{
	struct ceph_inode_info *ci = ceph_inode(inode);

	if (memcmp(&ci->i_old_atime, &inode->i_atime, sizeof(struct timeval))) {
		dout(30, "ceph_write_inode %llx .. atime updated\n",
		     ceph_ino(inode));
		/* eventually push this async to mds ... */
	}
	return 0;
}

static void ceph_put_super(struct super_block *s)
{
	struct ceph_client *cl = ceph_client(s);
	int rc;
	int seconds = 15;

	dout(30, "put_super\n");
	ceph_mdsc_stop(&cl->mdsc);
	ceph_monc_request_umount(&cl->monc);

	rc = wait_event_timeout(cl->mount_wq,
				(cl->mount_state == CEPH_MOUNT_UNMOUNTED),
				seconds*HZ);
	if (rc == 0)
		derr(0, "umount timed out after %d seconds\n", seconds);

	return;
}

static int ceph_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct ceph_client *client = ceph_inode_to_client(dentry->d_inode);
	struct ceph_statfs st;
	int err;

	dout(30, "ceph_statfs\n");
	err = ceph_monc_do_statfs(&client->monc, &st);
	if (err < 0)
		return err;

	/* fill in kstatfs */
	buf->f_type = CEPH_SUPER_MAGIC;  /* ?? */
	buf->f_bsize = 1 << CEPH_BLOCK_SHIFT;     /* 1 MB */
	buf->f_blocks = st.f_total >> (CEPH_BLOCK_SHIFT-10);
	buf->f_bfree = st.f_free >> (CEPH_BLOCK_SHIFT-10);
	buf->f_bavail = st.f_avail >> (CEPH_BLOCK_SHIFT-10);
	buf->f_files = st.f_objects;
	buf->f_ffree = -1;
	/* fsid? */
	buf->f_namelen = PATH_MAX;
	buf->f_frsize = 4096;

	return 0;
}


static int ceph_syncfs(struct super_block *sb, int wait)
{
	dout(10, "sync_fs %d\n", wait);
	return 0;
}


/**
 * ceph_show_options - Show mount options in /proc/mounts
 * @m: seq_file to write to
 * @mnt: mount descriptor
 */
static int ceph_show_options(struct seq_file *m, struct vfsmount *mnt)
{
	struct ceph_client *client = ceph_sb_to_client(mnt->mnt_sb);
	struct ceph_mount_args *args = &client->mount_args;

	if (ceph_debug != 0)
		seq_printf(m, ",debug=%d", ceph_debug);
	if (args->flags & CEPH_MOUNT_FSID)
		seq_printf(m, ",fsidmajor=%llu,fsidminor%llu",
			   args->fsid.major, args->fsid.minor);
	if (args->flags & CEPH_MOUNT_NOSHARE)
		seq_puts(m, ",noshare");
	return 0;
}


/*
 * inode cache
 */
static struct kmem_cache *ceph_inode_cachep;

static struct inode *ceph_alloc_inode(struct super_block *sb)
{
	struct ceph_inode_info *ci;
	int i;

	ci = kmem_cache_alloc(ceph_inode_cachep, GFP_NOFS);
	if (!ci)
		return NULL;

	dout(10, "alloc_inode %p vfsi %p\n", ci, &ci->vfs_inode);

	ci->i_version = 0;
	ci->i_time_warp_seq = 0;
	ci->i_symlink = 0;

	ci->i_lease_session = 0;
	ci->i_lease_mask = 0;
	ci->i_lease_ttl = 0;
	INIT_LIST_HEAD(&ci->i_lease_item);

	ci->i_fragtree = ci->i_fragtree_static;
	ci->i_fragtree->nsplits = 0;

	ci->i_frag_map_nr = 0;
	ci->i_frag_map = ci->i_frag_map_static;

	INIT_LIST_HEAD(&ci->i_caps);
	for (i = 0; i < STATIC_CAPS; i++)
		ci->i_static_caps[i].mds = -1;
	for (i = 0; i < CEPH_FILE_MODE_NUM; i++)
		ci->i_nr_by_mode[i] = 0;
	init_waitqueue_head(&ci->i_cap_wq);

	ci->i_wanted_max_size = 0;
	ci->i_requested_max_size = 0;

	ci->i_rd_ref = ci->i_rdcache_ref = 0;
	ci->i_wr_ref = 0;
	atomic_set(&ci->i_wrbuffer_ref, 0);
	ci->i_hold_caps_until = 0;
	INIT_LIST_HEAD(&ci->i_cap_delay_list);

	ci->i_hashval = 0;

	INIT_WORK(&ci->i_wb_work, ceph_inode_writeback);

	ci->i_vmtruncate_to = -1;
	INIT_WORK(&ci->i_vmtruncate_work, ceph_vmtruncate_work);

	return &ci->vfs_inode;
}

static void ceph_destroy_inode(struct inode *inode)
{
	struct ceph_inode_info *ci = ceph_inode(inode);
	dout(30, "destroy_inode %p ino %llx\n", inode, ceph_ino(inode));
	kfree(ci->i_symlink);
	kmem_cache_free(ceph_inode_cachep, ci);
}

static void init_once(struct kmem_cache *cachep, void *foo)
{
	struct ceph_inode_info *ci = foo;
	dout(10, "init_once on %p\n", &ci->vfs_inode);
	inode_init_once(&ci->vfs_inode);
}

static int init_inodecache(void)
{
	ceph_inode_cachep = kmem_cache_create("ceph_inode_cache",
					      sizeof(struct ceph_inode_info),
					      0, (SLAB_RECLAIM_ACCOUNT|
						  SLAB_MEM_SPREAD),
					      init_once);
	if (ceph_inode_cachep == NULL)
		return -ENOMEM;
	return 0;
}

static void destroy_inodecache(void)
{
	kmem_cache_destroy(ceph_inode_cachep);
}

static const struct super_operations ceph_super_ops = {
	.alloc_inode	= ceph_alloc_inode,
	.destroy_inode	= ceph_destroy_inode,
	.write_inode    = ceph_write_inode,
	.sync_fs        = ceph_syncfs,
	.put_super	= ceph_put_super,
	.show_options   = ceph_show_options,
	.statfs		= ceph_statfs,
};



/*
 * the monitor responds to monmap to indicate mount success.
 * (or, someday, to indicate a change in the monitor cluster?)
 */
static void handle_monmap(struct ceph_client *client, struct ceph_msg *msg)
{
	int err;
	int first = (client->monc.monmap->epoch == 0);
	void *new;

	dout(2, "handle_monmap had epoch %d\n", client->monc.monmap->epoch);
	new = ceph_monmap_decode(msg->front.iov_base,
				 msg->front.iov_base + msg->front.iov_len);
	if (IS_ERR(new)) {
		err = PTR_ERR(new);
		derr(0, "problem decoding monmap, %d\n", err);
		return;
	}
	kfree(client->monc.monmap);
	client->monc.monmap = new;

	if (first) {
		char name[10];
		client->whoami = le32_to_cpu(msg->hdr.dst.name.num);
		client->msgr->inst.name = msg->hdr.dst.name;
		sprintf(name, "client%d", client->whoami);
		dout(1, "i am %s, fsid is %llx.%llx\n", name,
		     le64_to_cpu(client->monc.monmap->fsid.major),
		     le64_to_cpu(client->monc.monmap->fsid.minor));

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,25)
		client->client_kobj = kobject_create_and_add(name, ceph_kobj);
		//client->fsid_kobj = kobject_create_and_add("fsid", 
		//client->client_kobj);
#endif
	}
}



const char *ceph_msg_type_name(int type)
{
	switch (type) {
	case CEPH_MSG_SHUTDOWN: return "shutdown";
	case CEPH_MSG_PING: return "ping";
	case CEPH_MSG_PING_ACK: return "ping_ack";
	case CEPH_MSG_MON_MAP: return "mon_map";
	case CEPH_MSG_MON_GET_MAP: return "mon_get_map";
	case CEPH_MSG_CLIENT_MOUNT: return "client_mount";
	case CEPH_MSG_CLIENT_UNMOUNT: return "client_unmount";
	case CEPH_MSG_STATFS: return "statfs";
	case CEPH_MSG_STATFS_REPLY: return "statfs_reply";
	case CEPH_MSG_MDS_GETMAP: return "mds_getmap";
	case CEPH_MSG_MDS_MAP: return "mds_map";
	case CEPH_MSG_CLIENT_SESSION: return "client_session";
	case CEPH_MSG_CLIENT_RECONNECT: return "client_reconnect";
	case CEPH_MSG_CLIENT_REQUEST: return "client_request";
	case CEPH_MSG_CLIENT_REQUEST_FORWARD: return "client_request_forward";
	case CEPH_MSG_CLIENT_REPLY: return "client_reply";
	case CEPH_MSG_CLIENT_FILECAPS: return "client_filecaps";
	case CEPH_MSG_CLIENT_LEASE: return "client_lease";
	case CEPH_MSG_OSD_GETMAP: return "osd_getmap";
	case CEPH_MSG_OSD_MAP: return "osd_map";
	case CEPH_MSG_OSD_OP: return "osd_op";
	case CEPH_MSG_OSD_OPREPLY: return "osd_opreply";
	}
	return "unknown";
}

void ceph_peer_reset(void *p, struct ceph_entity_name *peer_name)
{
	struct ceph_client *client = p;

	dout(30, "ceph_peer_reset peer_name = %s%d\n", ENTITY_NAME(*peer_name));

	/* write me */
}




/*
 * mount options
 */

enum {
	Opt_fsidmajor,
	Opt_fsidminor,
	Opt_debug,
	Opt_debug_console,
	Opt_debug_msgr,
	Opt_debug_tcp,
	Opt_debug_mdsc,
	Opt_debug_osdc,
	Opt_debug_addr,
	Opt_monport,
	Opt_port,
	Opt_wsize,
	Opt_osdtimeout,
	/* int args above */
	Opt_ip,
};

static match_table_t arg_tokens = {
	{Opt_fsidmajor, "fsidmajor=%ld"},
	{Opt_fsidminor, "fsidminor=%ld"},
	{Opt_debug, "debug=%d"},
	{Opt_debug_msgr, "debug_msgr=%d"},
	{Opt_debug_tcp, "debug_tcp=%d"},
	{Opt_debug_mdsc, "debug_mdsc=%d"},
	{Opt_debug_osdc, "debug_osdc=%d"},
	{Opt_debug_addr, "debug_addr=%d"},
	{Opt_monport, "monport=%d"},
	{Opt_port, "port=%d"},
	{Opt_wsize, "wsize=%d"},
	{Opt_osdtimeout, "osdtimeout=%d"},
	/* int args above */
	{Opt_ip, "ip=%s"},
	{Opt_debug_console, "debug_console"},
	{-1, NULL}
};

/*
 * FIXME: add error checking to ip parsing
 */
static int parse_ip(const char *c, int len, struct ceph_entity_addr *addr)
{
	int i;
	int v;
	unsigned ip = 0;
	const char *p = c;

	dout(15, "parse_ip on '%s' len %d\n", c, len);
	for (i = 0; *p && i < 4; i++) {
		v = 0;
		while (*p && *p != '.' && p < c+len) {
			if (*p < '0' || *p > '9')
				goto bad;
			v = (v * 10) + (*p - '0');
			p++;
		}
		ip = (ip << 8) + v;
		if (!*p)
			break;
		p++;
	}
	if (p < c+len)
		goto bad;

	*(__be32 *)&addr->ipaddr.sin_addr.s_addr = htonl(ip);
	dout(15, "parse_ip got %u.%u.%u.%u\n",
	     ip >> 24, (ip >> 16) & 0xff,
	     (ip >> 8) & 0xff, ip & 0xff);
	return 0;

bad:
	derr(1, "parse_ip bad ip '%s'\n", c);
	return -EINVAL;
}

static int parse_mount_args(int flags, char *options, const char *dev_name,
			    struct ceph_mount_args *args)
{
	char *c;
	int len, err;
	substring_t argstr[MAX_OPT_ARGS];

	dout(15, "parse_mount_args dev_name '%s'\n", dev_name);
	memset(args, 0, sizeof(*args));

	/* defaults */
	args->mntflags = flags;
	args->flags = 0;
	args->osd_timeout = 5;  /* seconds */

	/* ip1[,ip2...]:/server/path */
	c = strchr(dev_name, ':');
	if (c == NULL)
		return -EINVAL;

	/* get mon ip */
	/* er, just one for now. later, comma-separate... */
	len = c - dev_name;
	err = parse_ip(dev_name, len, &args->mon_addr[0]);
	if (err < 0)
		return err;
	args->mon_addr[0].ipaddr.sin_family = AF_INET;
	args->mon_addr[0].ipaddr.sin_port = htons(CEPH_MON_PORT);
	args->mon_addr[0].erank = 0;
	args->mon_addr[0].nonce = 0;
	args->num_mon = 1;

	/* path on server */
	c++;
	while (*c == '/') c++;  /* remove leading '/'(s) */
	if (strlen(c) >= sizeof(args->path))
		return -ENAMETOOLONG;
	strcpy(args->path, c);

	dout(15, "server path '%s'\n", args->path);

	/* parse mount options */
	while ((c = strsep(&options, ",")) != NULL) {
		int token, intval, ret, i;
		if (!*c)
			continue;
		token = match_token(c, arg_tokens, argstr);
		if (token < 0) {
			derr(0, "bad mount option at '%s'\n", c);
			return -EINVAL;

		}
		if (token < Opt_ip) {
			ret = match_int(&argstr[0], &intval);
			if (ret < 0) {
				dout(0, "bad mount arg, not int\n");
				continue;
			}
			dout(30, "got token %d intval %d\n", token, intval);
		}
		switch (token) {
		case Opt_fsidmajor:
			args->fsid.major = intval;
			break;
		case Opt_fsidminor:
			args->fsid.minor = intval;
			break;
		case Opt_monport:
			dout(25, "parse_mount_args monport=%d\n", intval);
			for (i = 0; i < args->num_mon; i++)
				args->mon_addr[i].ipaddr.sin_port =
					htons(intval);
			break;
		case Opt_port:
			args->my_addr.ipaddr.sin_port = htons(intval);
			break;
		case Opt_ip:
			err = parse_ip(argstr[0].from,
				       argstr[0].to-argstr[0].from,
				       &args->my_addr);
			if (err < 0)
				return err;
			args->flags |= CEPH_MOUNT_MYIP;
			break;

			/* debug levels */
		case Opt_debug:
			ceph_debug = intval;
			break;
		case Opt_debug_msgr:
			ceph_debug_msgr = intval;
			break;
		case Opt_debug_tcp:
			ceph_debug_tcp = intval;
			break;
		case Opt_debug_mdsc:
			ceph_debug_mdsc = intval;
			break;
		case Opt_debug_osdc:
			ceph_debug_osdc = intval;
			break;
		case Opt_debug_addr:
			ceph_debug_addr = intval;
			break;
		case Opt_debug_console:
			ceph_debug_console = 1;
			break;

			/* misc */
		case Opt_wsize:
			args->wsize = intval;
			break;
		case Opt_osdtimeout:
			args->osd_timeout = intval;
			break;

		default:
			BUG_ON(token);
		}
	}

	return 0;
}

/*
 * share work queue between clients.
 */
atomic_t ceph_num_clients = ATOMIC_INIT(0);

static void get_client_counter(void)
{
	if (atomic_add_return(1, &ceph_num_clients) == 1) {
		dout(10, "first client, setting up workqueues\n");
		ceph_workqueue_init();
	}
}

static void put_client_counter(void)
{
	if (atomic_dec_and_test(&ceph_num_clients)) {
		dout(10, "last client, shutting down workqueues\n");
		ceph_workqueue_shutdown();
	}
}

/*
 * create a fresh client instance
 */
struct ceph_client *ceph_create_client(struct ceph_mount_args *args,
				       struct super_block *sb)
{
	struct ceph_client *cl;
	struct ceph_entity_addr *myaddr = 0;
	int err = -ENOMEM;

	cl = kzalloc(sizeof(*cl), GFP_KERNEL);
	if (cl == NULL)
		return ERR_PTR(-ENOMEM);

	init_waitqueue_head(&cl->mount_wq);
	spin_lock_init(&cl->sb_lock);

	get_client_counter();

	cl->wb_wq = create_workqueue("ceph-writeback");
	if (cl->wb_wq == 0)
		goto fail;
	cl->trunc_wq = create_workqueue("ceph-trunc");
	if (cl->trunc_wq == 0)
		goto fail;

	/* messenger */
	if (args->flags & CEPH_MOUNT_MYIP)
		myaddr = &args->my_addr;
	cl->msgr = ceph_messenger_create(myaddr);
	if (IS_ERR(cl->msgr)) {
		err = PTR_ERR(cl->msgr);
		cl->msgr = 0;
		goto fail;
	}
	cl->msgr->parent = cl;
	cl->msgr->dispatch = ceph_dispatch;
	cl->msgr->prepare_pages = ceph_osdc_prepare_pages;
	cl->msgr->peer_reset = ceph_peer_reset;

	cl->whoami = -1;
	err = ceph_monc_init(&cl->monc, cl);
	if (err < 0)
		goto fail;
	ceph_mdsc_init(&cl->mdsc, cl);
	ceph_osdc_init(&cl->osdc, cl);

	cl->sb = sb;
	cl->mount_state = CEPH_MOUNT_MOUNTING;

	return cl;

fail:
	put_client_counter();
	kfree(cl);
	return ERR_PTR(err);
}

void ceph_destroy_client(struct ceph_client *cl)
{
	dout(10, "destroy_client %p\n", cl);

	/* unmount */
	/* ... */

	ceph_osdc_stop(&cl->osdc);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,25)
	if (cl->client_kobj)
		kobject_put(cl->client_kobj);
#endif
	if (cl->wb_wq)
		destroy_workqueue(cl->wb_wq);
	if (cl->trunc_wq)
		destroy_workqueue(cl->trunc_wq);
	ceph_messenger_destroy(cl->msgr);
	put_client_counter();
	kfree(cl);
	dout(10, "destroy_client %p done\n", cl);
}

static int have_all_maps(struct ceph_client *client)
{
	return client->osdc.osdmap && client->osdc.osdmap->epoch &&
		client->monc.monmap && client->monc.monmap->epoch &&
		client->mdsc.mdsmap && client->mdsc.mdsmap->m_epoch;
}

static struct dentry *open_root_dentry(struct ceph_client *client,
				       struct ceph_mount_args *args)
{
	struct ceph_mds_client *mdsc = &client->mdsc;
	struct ceph_mds_request *req = 0;
	struct ceph_mds_request_head *reqhead;
	int err;
	struct dentry *root;

	/* open dir */
	dout(30, "open_root_inode opening '%s'\n", args->path);
	req = ceph_mdsc_create_request(mdsc, CEPH_MDS_OP_OPEN,
				       1, args->path, 0, 0);
	if (IS_ERR(req))
		return ERR_PTR(PTR_ERR(req));
	req->r_expects_cap = 1;
	reqhead = req->r_request->front.iov_base;
	reqhead->args.open.flags = O_DIRECTORY;
	reqhead->args.open.mode = 0;
	err = ceph_mdsc_do_request(mdsc, req);
	if (err == 0) {
		root = req->r_last_dentry;
		dget(root);
		dout(30, "open_root_inode success, root dentry is %p\n", root);
	} else
		root = ERR_PTR(err);
	ceph_mdsc_put_request(req);
	return root;
}

/*
 * mount: join the ceph cluster.
 */
int ceph_mount(struct ceph_client *client, struct ceph_mount_args *args,
	       struct vfsmount *mnt)
{
	struct ceph_msg *mount_msg;
	struct dentry *root;
	int err;
	int attempts = 10;
	int which;
	char r;

	dout(10, "mount start\n");
	while (1) {
		get_random_bytes(&r, 1);
		which = r % args->num_mon;
		mount_msg = ceph_msg_new(CEPH_MSG_CLIENT_MOUNT, 0, 0, 0, 0);
		if (IS_ERR(mount_msg))
			return PTR_ERR(mount_msg);
		mount_msg->hdr.dst.name.type =
			cpu_to_le32(CEPH_ENTITY_TYPE_MON);
		mount_msg->hdr.dst.name.num = cpu_to_le32(which);
		mount_msg->hdr.dst.addr = args->mon_addr[which];

		ceph_msg_send(client->msgr, mount_msg, 0);
		dout(10, "mount from mon%d, %d attempts left\n",
		     which, attempts);

		/* wait */
		dout(10, "mount sent mount request, waiting for maps\n");
		err = wait_event_interruptible_timeout(client->mount_wq,
						       have_all_maps(client),
						       6*HZ);
		dout(10, "mount wait got %d\n", err);
		if (err == -EINTR)
			return err;
		if (have_all_maps(client))
			break;  /* success */
		dout(10, "mount still waiting for mount, attempts=%d\n",
		     attempts);
		if (--attempts == 0)
			return -EIO;
	}

	dout(30, "mount opening base mountpoint\n");
	root = open_root_dentry(client, args);
	if (IS_ERR(root))
		return PTR_ERR(root);
	mnt->mnt_root = root;
	mnt->mnt_sb = client->sb;
	client->mount_state = CEPH_MOUNT_MOUNTED;
	dout(10, "mount success\n");
	return 0;
}


/*
 * dispatch -- called with incoming messages.
 *
 * should be fast and non-blocking, as it is called with locks held.
 */
void ceph_dispatch(void *p, struct ceph_msg *msg)
{
	struct ceph_client *client = p;
	int had;
	int type = le32_to_cpu(msg->hdr.type);

	/* deliver the message */
	switch (type) {
		/* me */
	case CEPH_MSG_MON_MAP:
		had = client->monc.monmap->epoch ? 1:0;
		handle_monmap(client, msg);
		if (!had && client->monc.monmap->epoch && have_all_maps(client))
			wake_up(&client->mount_wq);
		break;

		/* mon client */
	case CEPH_MSG_STATFS_REPLY:
		ceph_monc_handle_statfs_reply(&client->monc, msg);
		break;
	case CEPH_MSG_CLIENT_UNMOUNT:
		ceph_monc_handle_umount(&client->monc, msg);
		break;

		/* mds client */
	case CEPH_MSG_MDS_MAP:
		had = client->mdsc.mdsmap ? 1:0;
		ceph_mdsc_handle_map(&client->mdsc, msg);
		if (!had && client->mdsc.mdsmap && have_all_maps(client))
			wake_up(&client->mount_wq);
		break;
	case CEPH_MSG_CLIENT_SESSION:
		ceph_mdsc_handle_session(&client->mdsc, msg);
		break;
	case CEPH_MSG_CLIENT_REPLY:
		ceph_mdsc_handle_reply(&client->mdsc, msg);
		break;
	case CEPH_MSG_CLIENT_REQUEST_FORWARD:
		ceph_mdsc_handle_forward(&client->mdsc, msg);
		break;
	case CEPH_MSG_CLIENT_FILECAPS:
		ceph_mdsc_handle_filecaps(&client->mdsc, msg);
		break;
	case CEPH_MSG_CLIENT_LEASE:
		ceph_mdsc_handle_lease(&client->mdsc, msg);
		break;

		/* osd client */
	case CEPH_MSG_OSD_MAP:
		had = client->osdc.osdmap ? 1:0;
		ceph_osdc_handle_map(&client->osdc, msg);
		if (!had && client->osdc.osdmap && have_all_maps(client))
			wake_up(&client->mount_wq);
		break;
	case CEPH_MSG_OSD_OPREPLY:
		ceph_osdc_handle_reply(&client->osdc, msg);
		break;

	default:
		derr(0, "received unknown message type %d\n", type);
	}

	ceph_msg_put(msg);
}


static int ceph_set_super(struct super_block *s, void *data)
{
	struct ceph_mount_args *args = data;
	struct ceph_client *client;
	int ret;

	dout(10, "set_super %p data %p\n", s, data);

	s->s_flags = args->mntflags;
	s->s_maxbytes = min((u64)MAX_LFS_FILESIZE, CEPH_FILE_MAX_SIZE);

	/* create client */
	client = ceph_create_client(args, s);
	if (IS_ERR(client))
		return PTR_ERR(client);
	s->s_fs_info = client;

	/* fill sbinfo */
	s->s_op = &ceph_super_ops;
	s->s_export_op = &ceph_export_ops;
	memcpy(&client->mount_args, args, sizeof(*args));

	/* set time granularity */
	s->s_time_gran = 1000;  /* 1000 ns == 1 us */

	ret = set_anon_super(s, 0);  /* what is the second arg for? */
	if (ret != 0)
		goto bail;

	return ret;

bail:
	ceph_destroy_client(client);
	s->s_fs_info = 0;
	return ret;
}

/*
 * share superblock if same fs AND options
 */
static int ceph_compare_super(struct super_block *sb, void *data)
{
	struct ceph_mount_args *args = (struct ceph_mount_args *)data;
	struct ceph_client *other = ceph_sb_to_client(sb);
	int i;
	dout(10, "ceph_compare_super %p\n", sb);

	/* either compare fsid, or specified mon_hostname */
	if (args->flags & CEPH_MOUNT_FSID) {
		if (!ceph_fsid_equal(&args->fsid, &other->fsid)) {
			dout(30, "fsid doesn't match\n");
			return 0;
		}
	} else {
		/* do we share (a) monitor? */
		for (i = 0; i < args->num_mon; i++)
			if (ceph_monmap_contains(other->monc.monmap,
						 &args->mon_addr[i]))
				break;
		if (i == args->num_mon) {
			dout(30, "mon ip not part of monmap\n");
			return 0;
		}
		dout(10, "mon ip matches existing sb %p\n", sb);
	}
	if (args->mntflags != other->mount_args.mntflags) {
		dout(30, "flags differ\n");
		return 0;
	}
	return 1;
}

static int ceph_get_sb(struct file_system_type *fs_type,
		       int flags, const char *dev_name, void *data,
		       struct vfsmount *mnt)
{
	struct super_block *sb;
	struct ceph_mount_args *mount_args;
	struct ceph_client *client;
	int err;
	int (*compare_super)(struct super_block *, void *) = ceph_compare_super;

	dout(25, "ceph_get_sb\n");

	mount_args = kmalloc(sizeof(struct ceph_mount_args), GFP_KERNEL);
	err = parse_mount_args(flags, data, dev_name, mount_args);
	if (err < 0)
		goto out;

	if (mount_args->flags & CEPH_MOUNT_NOSHARE)
		compare_super = 0;

	/* superblock */
	sb = sget(fs_type, compare_super, ceph_set_super, mount_args);
	if (IS_ERR(sb)) {
		err = PTR_ERR(sb);
		goto out;
	}
	client = ceph_sb_to_client(sb);

	err = ceph_mount(client, mount_args, mnt);
	if (err < 0)
		goto out_splat;
	dout(22, "root ino %llx\n", ceph_ino(mnt->mnt_root->d_inode));
	return 0;

out_splat:
	up_write(&sb->s_umount);
	deactivate_super(sb);
out:
	kfree(mount_args);
	dout(25, "ceph_get_sb fail %d\n", err);
	return err;
}

static void ceph_kill_sb(struct super_block *s)
{
	struct ceph_client *client = ceph_sb_to_client(s);
	dout(1, "kill_sb %p\n", s);
	ceph_mdsc_pre_umount(&client->mdsc);
	kill_anon_super(s);    /* will call put_super after sb is r/o */
	ceph_destroy_client(client);
}





/************************************/

static struct file_system_type ceph_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "ceph",
	.get_sb		= ceph_get_sb,
	.kill_sb	= ceph_kill_sb,
	.fs_flags	= FS_RENAME_DOES_D_MOVE,
};

struct kobject *ceph_kobj;

static int __init init_ceph(void)
{
	int ret = 0;

	dout(1, "init_ceph\n");

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,25)
	ceph_kobj = kobject_create_and_add("ceph", fs_kobj);
	if (!ceph_kobj)
		return -ENOMEM;
#endif
	ceph_proc_init();

	ret = init_inodecache();
	if (ret)
		goto out;
	ret = register_filesystem(&ceph_fs_type);
	if (ret)
		destroy_inodecache();
out:
	return ret;
}

static void __exit exit_ceph(void)
{
	dout(1, "exit_ceph\n");

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,25)
	kobject_put(ceph_kobj);
	ceph_kobj = 0;
#endif
	ceph_proc_cleanup();

	unregister_filesystem(&ceph_fs_type);
	destroy_inodecache();
}

module_init(init_ceph);
module_exit(exit_ceph);

MODULE_AUTHOR("Patience Warnick <patience@newdream.net>");
MODULE_AUTHOR("Sage Weil <sage@newdream.net>");
MODULE_DESCRIPTION("Ceph filesystem for Linux");
MODULE_LICENSE("GPL");
