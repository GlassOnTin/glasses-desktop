#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <json-c/json.h>
#include "xdg-shell-client-protocol.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "presenter.h"

#define IPC_MAGIC "i3-ipc"
#define IPC_HDR (6 + 4 + 4)
#define IPC_SUBSCRIBE 2
#define IPC_GET_TREE 4

static bool ipc_send(struct app *a, uint32_t type, const char *payload)
{
	uint32_t len = payload ? strlen(payload) : 0;
	char hdr[IPC_HDR];
	memcpy(hdr, IPC_MAGIC, 6);
	memcpy(hdr + 6, &len, 4);
	memcpy(hdr + 10, &type, 4);
	if (write(a->ipc_fd, hdr, IPC_HDR) != IPC_HDR)
		return false;
	if (len && write(a->ipc_fd, payload, len) != (ssize_t)len)
		return false;
	return true;
}

bool sway_connect(struct app *a)
{
	const char *sock = getenv("SWAYSOCK");
	if (!sock) {
		fprintf(stderr, "presenter: SWAYSOCK not set (need sway)\n");
		return false;
	}
	a->ipc_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	snprintf(addr.sun_path, sizeof addr.sun_path, "%s", sock);
	if (connect(a->ipc_fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
		fprintf(stderr, "presenter: cannot connect %s: %m\n", sock);
		return false;
	}
	return ipc_send(a, IPC_SUBSCRIBE, "[\"window\",\"workspace\",\"output\"]");
}

void sway_request_tree(struct app *a)
{
	if (a->tree_pending) {
		a->want_tree = true;
		return;
	}
	if (ipc_send(a, IPC_GET_TREE, NULL))
		a->tree_pending = true;
}

/* ---- tree parsing ---- */

struct tmpwin {
	int64_t id;
	int x, y, w, h;
	bool floating, focused;
	int rank;
};

struct parse_ctx {
	struct tmpwin v[MAX_WINDOWS];
	int n;
	int rank_counter;
};

static struct json_object *jget(struct json_object *o, const char *k)
{
	struct json_object *v;
	return json_object_object_get_ex(o, k, &v) ? v : NULL;
}

static int jint(struct json_object *o, const char *k, int def)
{
	struct json_object *v = jget(o, k);
	return v ? json_object_get_int(v) : def;
}

static bool is_view(struct json_object *node)
{
	struct json_object *pid = jget(node, "pid");
	return pid && !json_object_is_type(pid, json_type_null);
}

static void add_view(struct parse_ctx *c, struct json_object *node,
	bool floating)
{
	if (c->n >= MAX_WINDOWS)
		return;
	struct json_object *rect = jget(node, "rect");
	if (!rect)
		return;
	struct tmpwin *w = &c->v[c->n++];
	w->id = json_object_get_int64(jget(node, "id"));
	w->x = jint(rect, "x", 0);
	w->y = jint(rect, "y", 0);
	w->w = jint(rect, "width", 0);
	w->h = jint(rect, "height", 0);
	w->floating = floating;
	w->focused = json_object_get_boolean(jget(node, "focused"));
	w->rank = -1;
	/* the titlebar sits directly above rect */
	struct json_object *deco = jget(node, "deco_rect");
	int dh = deco ? jint(deco, "height", 0) : 0;
	if (dh > 0) {
		w->y -= dh;
		w->h += dh;
	}
}

static void collect(struct parse_ctx *c, struct json_object *node,
	bool floating)
{
	struct json_object *arr = jget(node, "nodes");
	for (size_t i = 0; arr && i < json_object_array_length(arr); i++) {
		struct json_object *n = json_object_array_get_idx(arr, i);
		if (is_view(n))
			add_view(c, n, floating);
		else
			collect(c, n, floating);
	}
	arr = jget(node, "floating_nodes");
	for (size_t i = 0; arr && i < json_object_array_length(arr); i++) {
		struct json_object *n = json_object_array_get_idx(arr, i);
		if (is_view(n))
			add_view(c, n, true);
		else
			collect(c, n, true);
	}
}

static struct tmpwin *find_tmp(struct parse_ctx *c, int64_t id)
{
	for (int i = 0; i < c->n; i++)
		if (c->v[i].id == id)
			return &c->v[i];
	return NULL;
}

static struct json_object *find_child(struct json_object *node, int64_t id)
{
	const char *lists[] = { "nodes", "floating_nodes" };
	for (int l = 0; l < 2; l++) {
		struct json_object *arr = jget(node, lists[l]);
		for (size_t i = 0; arr && i < json_object_array_length(arr); i++) {
			struct json_object *n = json_object_array_get_idx(arr, i);
			if (json_object_get_int64(jget(n, "id")) == id)
				return n;
		}
	}
	return NULL;
}

/* focus arrays order children most-recent-first; DFS in that order yields
 * the global focus-recency chain */
static void rank_views(struct parse_ctx *c, struct json_object *node)
{
	struct json_object *focus = jget(node, "focus");
	for (size_t i = 0; focus && i < json_object_array_length(focus); i++) {
		int64_t id = json_object_get_int64(
			json_object_array_get_idx(focus, i));
		struct json_object *child = find_child(node, id);
		if (!child)
			continue;
		struct tmpwin *w = find_tmp(c, id);
		if (w) {
			if (w->rank < 0)
				w->rank = c->rank_counter++;
		} else {
			rank_views(c, child);
		}
	}
}

static void parse_tree(struct app *a, struct json_object *root)
{
	struct json_object *outputs = jget(root, "nodes"), *out = NULL;
	for (size_t i = 0; outputs && i < json_object_array_length(outputs); i++) {
		struct json_object *n = json_object_array_get_idx(outputs, i);
		const char *name = json_object_get_string(jget(n, "name"));
		if (name && !strcmp(name, a->src_name)) {
			out = n;
			break;
		}
	}
	if (!out) {
		a->nwins = 0;
		a->tree_dirty = true;
		return;
	}
	struct json_object *rect = jget(out, "rect");
	a->desk_x = jint(rect, "x", 0);
	a->desk_y = jint(rect, "y", 0);
	a->desk_w = jint(rect, "width", 1920);
	a->desk_h = jint(rect, "height", 1080);
	a->have_desk = true;

	/* current workspace = first entry of the output's focus array */
	struct json_object *ws = NULL, *focus = jget(out, "focus");
	if (focus && json_object_array_length(focus) > 0)
		ws = find_child(out, json_object_get_int64(
			json_object_array_get_idx(focus, 0)));
	if (!ws) {
		struct json_object *arr = jget(out, "nodes");
		if (arr && json_object_array_length(arr) > 0)
			ws = json_object_array_get_idx(arr, 0);
	}
	if (!ws) {
		a->nwins = 0;
		a->tree_dirty = true;
		return;
	}

	struct parse_ctx c = { 0 };
	collect(&c, ws, false);
	rank_views(&c, ws);
	for (int i = 0; i < c.n; i++)      /* views the focus DFS missed */
		if (c.v[i].rank < 0)
			c.v[i].rank = c.rank_counter++;

	/* paint back-to-front: tiled (never overlap), then floating by
	 * descending rank so the most recently focused lands on top */
	a->nwins = 0;
	for (int i = 0; i < c.n && a->nwins < MAX_WINDOWS; i++) {
		if (c.v[i].floating)
			continue;
		struct win *w = &a->wins[a->nwins++];
		w->x = c.v[i].x - a->desk_x;
		w->y = c.v[i].y - a->desk_y;
		w->w = c.v[i].w;
		w->h = c.v[i].h;
		w->rank = c.v[i].rank;
		w->focused = c.v[i].focused;
	}
	/* floating: highest rank (least recent) first, focused ends on top */
	for (int r = c.rank_counter; r >= 0; r--) {
		for (int i = 0; i < c.n && a->nwins < MAX_WINDOWS; i++) {
			if (!c.v[i].floating || c.v[i].rank != r)
				continue;
			struct win *w = &a->wins[a->nwins++];
			w->x = c.v[i].x - a->desk_x;
			w->y = c.v[i].y - a->desk_y;
			w->w = c.v[i].w;
			w->h = c.v[i].h;
			w->rank = c.v[i].rank;
			w->focused = c.v[i].focused;
		}
	}
	a->tree_dirty = true;
	LOGF(a, 2, "tree: %d windows on %s (%dx%d)", a->nwins, a->src_name,
		a->desk_w, a->desk_h);
}

/* ---- message pump ---- */

static void handle_msg(struct app *a, uint32_t type, const char *payload,
	uint32_t len)
{
	if (type == IPC_GET_TREE) {
		a->tree_pending = false;
		struct json_object *root = json_tokener_parse(payload);
		if (root) {
			parse_tree(a, root);
			json_object_put(root);
		}
		if (a->want_tree) {
			a->want_tree = false;
			sway_request_tree(a);
		}
	} else if (type & 0x80000000u) {
		/* title changes don't move rects and fire constantly */
		if ((type & 0xff) == 3) { /* window event */
			struct json_object *ev = json_tokener_parse(payload);
			if (ev) {
				struct json_object *ch = jget(ev, "change");
				const char *s = json_object_get_string(ch);
				bool skip = s && !strcmp(s, "title");
				json_object_put(ev);
				if (skip)
					return;
			}
		}
		sway_request_tree(a); /* any other subscribed event → refresh */
	}
	/* subscribe reply and anything else: ignore */
}

bool sway_handle_readable(struct app *a)
{
	char buf[65536];
	ssize_t n = read(a->ipc_fd, buf, sizeof buf);
	if (n == 0)
		return false;
	if (n < 0)
		return errno == EAGAIN || errno == EINTR;

	if (a->ipc_len + n + 1 > a->ipc_cap) {
		a->ipc_cap = (a->ipc_len + n) * 2;
		a->ipc_buf = realloc(a->ipc_buf, a->ipc_cap);
	}
	memcpy(a->ipc_buf + a->ipc_len, buf, n);
	a->ipc_len += n;

	size_t off = 0;
	while (a->ipc_len - off >= IPC_HDR) {
		uint32_t plen, type;
		memcpy(&plen, a->ipc_buf + off + 6, 4);
		memcpy(&type, a->ipc_buf + off + 10, 4);
		if (a->ipc_len - off < IPC_HDR + plen)
			break;
		/* NUL-terminate payload in place for json-c */
		char saved = a->ipc_buf[off + IPC_HDR + plen];
		a->ipc_buf[off + IPC_HDR + plen] = '\0';
		handle_msg(a, type, a->ipc_buf + off + IPC_HDR, plen);
		a->ipc_buf[off + IPC_HDR + plen] = saved;
		off += IPC_HDR + plen;
	}
	if (off) {
		memmove(a->ipc_buf, a->ipc_buf + off, a->ipc_len - off);
		a->ipc_len -= off;
	}
	return true;
}
