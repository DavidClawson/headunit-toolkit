/* Apple binary property lists, see bplist.h. Format: "bplist00", objects, offset
   table, 32-byte trailer. Object marker: high nibble type, low nibble count
   (0xF: an int object with the real count follows). */
#include "bplist.h"

bp_node bp_nodes[BP_MAX_NODES];
int bp_kids[BP_MAX_KIDS];
static int nn, nk;
static int cap_of[BP_MAX_NODES];                       /* builder: reserved child slots (0 for parsed) */

void bp_reset(void) { nn = 0; nk = 0; }

static int new_node(uint8_t t) { if (nn >= BP_MAX_NODES) return -1; cap_of[nn] = 0; bp_node *x = &bp_nodes[nn]; x->t = t; x->n = 0; x->i = 0; x->r = 0; x->p = 0; x->kid = 0; return nn++; }

/* ---- parsing ---- */
static const uint8_t *pb; static int pn, osz, rsz, nobj; static uint64_t otab;

static uint64_t be(const uint8_t *p, int n) { uint64_t v = 0; for (int i = 0; i < n; i++) v = v << 8 | p[i]; return v; }

static int parse_obj(uint64_t ref, int depth);

static int obj_off(uint64_t ref, uint64_t *off)
{
	if (ref >= (uint64_t)nobj) return -1;
	uint64_t at = otab + ref * (uint64_t)osz;
	if (at + (uint64_t)osz > (uint64_t)pn) return -1;
	*off = be(pb + at, osz);
	return *off < (uint64_t)pn - 32 ? 0 : -1;
}

static int count_at(uint64_t *o, uint8_t lo, int64_t *cnt)  /* the count, following 0xF if needed */
{
	if (lo != 15) { *cnt = lo; return 0; }
	if (*o >= (uint64_t)pn) return -1;
	uint8_t m = pb[*o];
	if ((m >> 4) != 1) return -1;
	int w = 1 << (m & 15);
	if (*o + 1 + (uint64_t)w > (uint64_t)pn || w > 8) return -1;
	*cnt = (int64_t)be(pb + *o + 1, w);
	*o += 1 + (uint64_t)w;
	return 0;
}

static int parse_obj(uint64_t ref, int depth)
{
	uint64_t o;
	if (depth > 16 || obj_off(ref, &o)) return -1;
	uint8_t m = pb[o++], hi = m >> 4, lo = m & 15;
	int x;
	switch (hi) {
	case 0x0:
		x = new_node(lo == 8 || lo == 9 ? BP_BOOL : BP_NULL); if (x < 0) return -1;
		bp_nodes[x].i = lo == 9; return x;
	case 0x1: {
		int w = 1 << lo; if (w > 16 || o + (uint64_t)w > (uint64_t)pn) return -1;
		x = new_node(BP_INT); if (x < 0) return -1;
		uint64_t v = be(pb + o + (w > 8 ? w - 8 : 0), w > 8 ? 8 : w);
		bp_nodes[x].i = w == 8 || w > 8 ? (int64_t)v : (int64_t)v;    /* 1,2,4 unsigned; 8 signed */
		return x;
	}
	case 0x2: case 0x3: {
		int w = hi == 3 ? 8 : 1 << lo; if (o + (uint64_t)w > (uint64_t)pn) return -1;
		x = new_node(hi == 3 ? BP_DATE : BP_REAL); if (x < 0) return -1;
		if (w == 8) { uint64_t v = be(pb + o, 8); double d; uint8_t *dp = (uint8_t *)&d; for (int i = 0; i < 8; i++) dp[i] = (uint8_t)(v >> (8 * i)); bp_nodes[x].r = d; }
		else if (w == 4) { uint32_t v = (uint32_t)be(pb + o, 4); float f; uint8_t *fp = (uint8_t *)&f; for (int i = 0; i < 4; i++) fp[i] = (uint8_t)(v >> (8 * i)); bp_nodes[x].r = f; }
		return x;
	}
	case 0x4: case 0x5: case 0x6: {
		int64_t c; if (count_at(&o, lo, &c)) return -1;
		int64_t bytes = hi == 6 ? c * 2 : c;
		if (c < 0 || o + (uint64_t)bytes > (uint64_t)pn) return -1;
		x = new_node(hi == 4 ? BP_DATA : BP_STR); if (x < 0) return -1;
		bp_nodes[x].p = pb + o; bp_nodes[x].n = (int)bytes;
		if (hi == 6) bp_nodes[x].i = 1;               /* UTF-16BE: flagged, bytes kept raw */
		return x;
	}
	case 0x8: {
		int w = lo + 1; if (o + (uint64_t)w > (uint64_t)pn) return -1;
		x = new_node(BP_UID); if (x < 0) return -1;
		bp_nodes[x].i = (int64_t)be(pb + o, w); return x;
	}
	case 0xA: case 0xD: {
		int64_t c; if (count_at(&o, lo, &c)) return -1;
		int per = hi == 0xD ? 2 : 1;
		if (c < 0 || c > 256 || o + (uint64_t)(c * per * rsz) > (uint64_t)pn || nk + c * per > BP_MAX_KIDS) return -1;
		x = new_node(hi == 0xD ? BP_DICT : BP_ARRAY); if (x < 0) return -1;
		int base = nk; nk += (int)(c * per);
		bp_nodes[x].n = (int)c; bp_nodes[x].kid = base;
		for (int i = 0; i < c * per; i++) {
			int k = parse_obj(be(pb + o + (uint64_t)(i * rsz), rsz), depth + 1);
			if (k < 0) return -1;
			bp_kids[base + i] = k;
		}
		return x;
	}
	}
	return -1;
}

int bp_parse(const uint8_t *b, int n)
{
	if (n < 8 + 32) return -1;
	for (int i = 0; i < 8; i++) if (b[i] != "bplist00"[i]) return -1;
	const uint8_t *t = b + n - 32;
	pb = b; pn = n; osz = t[6]; rsz = t[7];
	uint64_t no = be(t + 8, 8), top = be(t + 16, 8); otab = be(t + 24, 8);
	if (osz < 1 || osz > 8 || rsz < 1 || rsz > 8 || no > 100000 || otab + no * (uint64_t)osz > (uint64_t)n - 32) return -1;
	nobj = (int)no;
	return parse_obj(top, 0);
}

static int eq(const bp_node *x, const char *s)
{
	if (x->t != BP_STR || x->i) return 0;
	int i = 0;
	for (; i < x->n; i++) if (!s[i] || s[i] != (char)x->p[i]) return 0;
	return s[i] == 0;
}
int bp_str_is(int node, const char *s) { return node >= 0 && eq(&bp_nodes[node], s); }
int bp_key(int d, int k) { return d >= 0 && bp_nodes[d].t == BP_DICT && k < bp_nodes[d].n ? bp_kids[bp_nodes[d].kid + k] : -1; }
int bp_val(int d, int k) { return d >= 0 && bp_nodes[d].t == BP_DICT && k < bp_nodes[d].n ? bp_kids[bp_nodes[d].kid + bp_nodes[d].n + k] : -1; }
int bp_item(int a, int k) { return a >= 0 && bp_nodes[a].t == BP_ARRAY && k < bp_nodes[a].n ? bp_kids[bp_nodes[a].kid + k] : -1; }
int bp_get(int d, const char *key)
{
	if (d < 0 || bp_nodes[d].t != BP_DICT) return -1;
	for (int k = 0; k < bp_nodes[d].n; k++) if (bp_str_is(bp_key(d, k), key)) return bp_val(d, k);
	return -1;
}
int64_t bp_int_of(int node, int64_t dflt)
{
	if (node < 0) return dflt;
	const bp_node *x = &bp_nodes[node];
	if (x->t == BP_INT || x->t == BP_BOOL || x->t == BP_UID) return x->i;
	if (x->t == BP_REAL) return (int64_t)x->r;
	return dflt;
}

/* ---- building ---- */
static int container(uint8_t t, int slots)
{
	int x = new_node(t);
	if (x < 0 || nk + slots > BP_MAX_KIDS) return -1;
	bp_nodes[x].kid = nk; cap_of[x] = slots; nk += slots;
	return x;
}
int bp_new_dict(int max_pairs) { return container(BP_DICT, 2 * max_pairs); }
int bp_new_array(int max_items) { return container(BP_ARRAY, max_items); }
int bp_new_str(const char *s) { int x = new_node(BP_STR); if (x < 0) return -1; int n = 0; while (s[n]) n++; bp_nodes[x].p = (const uint8_t *)s; bp_nodes[x].n = n; return x; }
int bp_new_int(int64_t v) { int x = new_node(BP_INT); if (x >= 0) bp_nodes[x].i = v; return x; }
int bp_new_bool(int v) { int x = new_node(BP_BOOL); if (x >= 0) bp_nodes[x].i = !!v; return x; }
int bp_new_real(double v) { int x = new_node(BP_REAL); if (x >= 0) bp_nodes[x].r = v; return x; }
int bp_new_data(const uint8_t *p, int n) { int x = new_node(BP_DATA); if (x >= 0) { bp_nodes[x].p = p; bp_nodes[x].n = n; } return x; }
int bp_set(int d, const char *key, int val)
{
	if (d < 0 || val < 0 || bp_nodes[d].t != BP_DICT) return -1;
	int half = cap_of[d] / 2, k = bp_nodes[d].n;
	if (k >= half) return -1;
	int kn = bp_new_str(key); if (kn < 0) return -1;
	bp_kids[bp_nodes[d].kid + k] = kn;                 /* keys in the first half */
	bp_kids[bp_nodes[d].kid + half + k] = val;          /* values in the second */
	bp_nodes[d].n++;
	return val;
}
int bp_push(int a, int val)
{
	if (a < 0 || val < 0 || bp_nodes[a].t != BP_ARRAY || bp_nodes[a].n >= cap_of[a]) return -1;
	bp_kids[bp_nodes[a].kid + bp_nodes[a].n++] = val;
	return val;
}

/* ---- writing: every node in the pool reachable from root becomes one object ---- */
static uint8_t *wo; static int wn, wcap, wrsz;
static int objnum[BP_MAX_NODES];
static int order[BP_MAX_NODES], norder;

static int put(const void *d, int n) { if (wn + n > wcap) return -1; const uint8_t *s = d; for (int i = 0; i < n; i++) wo[wn++] = s[i]; return 0; }
static int put_be(uint64_t v, int w) { uint8_t b[8]; for (int i = 0; i < w; i++) b[i] = (uint8_t)(v >> (8 * (w - 1 - i))); return put(b, w); }
static int put_marker(uint8_t hi, int64_t cnt)
{
	if (cnt < 15) { uint8_t m = (uint8_t)(hi << 4 | cnt); return put(&m, 1); }
	uint8_t m = (uint8_t)(hi << 4 | 15); if (put(&m, 1)) return -1;
	if (cnt < 256) { uint8_t i[2] = { 0x10, (uint8_t)cnt }; return put(i, 2); }
	uint8_t i = 0x11; if (put(&i, 1)) return -1; return put_be((uint64_t)cnt, 2);
}
static int kid_of(int x, int i)                        /* i-th child in serialisation order */
{
	const bp_node *n = &bp_nodes[x];
	if (n->t == BP_ARRAY) return bp_kids[n->kid + i];
	int half = cap_of[x] ? cap_of[x] / 2 : n->n;       /* built dicts reserve slots; parsed ones are packed */
	return i < n->n ? bp_kids[n->kid + i] : bp_kids[n->kid + half + (i - n->n)];
}
static int visit(int x, int depth)
{
	if (depth > 16 || objnum[x] >= 0) return 0;
	objnum[x] = norder; order[norder++] = x;
	const bp_node *n = &bp_nodes[x];
	int k = n->t == BP_DICT ? 2 * n->n : n->t == BP_ARRAY ? n->n : 0;
	for (int i = 0; i < k; i++) if (visit(kid_of(x, i), depth + 1)) return -1;
	return 0;
}

int bp_write(int root, uint8_t *out, int cap)
{
	if (root < 0) return -1;
	for (int i = 0; i < nn; i++) objnum[i] = -1;
	norder = 0;
	if (visit(root, 0)) return -1;
	wo = out; wn = 0; wcap = cap; wrsz = norder < 256 ? 1 : 2;
	static uint32_t offs[BP_MAX_NODES];
	if (put("bplist00", 8)) return -1;
	for (int j = 0; j < norder; j++) {
		int x = order[j]; const bp_node *n = &bp_nodes[x];
		offs[j] = (uint32_t)wn;
		int r = 0;
		switch (n->t) {
		case BP_NULL: { uint8_t m = 0; r = put(&m, 1); break; }
		case BP_BOOL: { uint8_t m = n->i ? 9 : 8; r = put(&m, 1); break; }
		case BP_INT:
			if (n->i >= 0 && n->i < 256) { uint8_t b[2] = { 0x10, (uint8_t)n->i }; r = put(b, 2); }
			else if (n->i >= 0 && n->i < 65536) { uint8_t m = 0x11; r = put(&m, 1) || put_be((uint64_t)n->i, 2); }
			else if (n->i >= 0 && n->i < 4294967296LL) { uint8_t m = 0x12; r = put(&m, 1) || put_be((uint64_t)n->i, 4); }
			else { uint8_t m = 0x13; r = put(&m, 1) || put_be((uint64_t)n->i, 8); }
			break;
		case BP_REAL: case BP_DATE: {
			uint8_t m = n->t == BP_DATE ? 0x33 : 0x23; double d = n->r; uint64_t v = 0; const uint8_t *dp = (const uint8_t *)&d;
			for (int i = 0; i < 8; i++) v |= (uint64_t)dp[i] << (8 * i);
			r = put(&m, 1) || put_be(v, 8); break;
		}
		case BP_DATA: r = put_marker(4, n->n) || put(n->p, n->n); break;
		case BP_STR: r = put_marker(n->i ? 6 : 5, n->i ? n->n / 2 : n->n) || put(n->p, n->n); break;
		case BP_UID: { uint8_t m = 0x83; r = put(&m, 1) || put_be((uint64_t)n->i, 4); break; }
		case BP_ARRAY: case BP_DICT: {
			int k = n->t == BP_DICT ? 2 * n->n : n->n;
			r = put_marker(n->t == BP_DICT ? 0xD : 0xA, n->n);
			for (int i = 0; i < k && !r; i++) r = put_be((uint64_t)objnum[kid_of(x, i)], wrsz);
			break;
		}
		}
		if (r) return -1;
	}
	int osz2 = wn < 256 ? 1 : wn < 65536 ? 2 : 4;
	uint32_t tab = (uint32_t)wn;
	for (int j = 0; j < norder; j++) if (put_be(offs[j], osz2)) return -1;
	uint8_t tr[32] = { 0 };
	tr[6] = (uint8_t)osz2; tr[7] = (uint8_t)wrsz;
	for (int i = 0; i < 8; i++) { tr[15 - i] = (uint8_t)((uint64_t)norder >> (8 * i)); tr[31 - i] = (uint8_t)((uint64_t)tab >> (8 * i)); }
	if (put(tr, 32)) return -1;
	return wn;
}
