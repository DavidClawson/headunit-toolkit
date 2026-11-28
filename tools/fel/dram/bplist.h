/* Apple binary property lists (bplist00): parse into a small node pool, build and
   write. Enough for AirPlay/CarPlay RTSP bodies. Written for this project; host-testable. */
#ifndef BPLIST_H
#define BPLIST_H
#include <stdint.h>

enum { BP_NULL, BP_BOOL, BP_INT, BP_REAL, BP_DATA, BP_STR, BP_ARRAY, BP_DICT, BP_UID, BP_DATE };

typedef struct {
	uint8_t t;
	int n;                 /* bytes (data/str), items (array), pairs (dict) */
	int64_t i;             /* bool/int/uid */
	double r;              /* real/date */
	const uint8_t *p;      /* data/str bytes (not NUL-terminated for parsed ASCII) */
	int kid;               /* first index in bp_kids: array items, or dict keys then values */
} bp_node;

#ifndef BP_MAX_NODES
#define BP_MAX_NODES 2048
#endif
#ifndef BP_MAX_KIDS
#define BP_MAX_KIDS  4096
#endif
extern bp_node bp_nodes[BP_MAX_NODES];
extern int bp_kids[BP_MAX_KIDS];

void bp_reset(void);                                  /* empty the pool (both parse and build use it) */

/* parsing: returns the root node index, or -1 */
int bp_parse(const uint8_t *b, int n);
int bp_get(int dict, const char *key);                /* value node for key, or -1 */
int bp_key(int dict, int k);                          /* k-th key node */
int bp_val(int dict, int k);                          /* k-th value node */
int bp_item(int array, int k);
int bp_str_is(int node, const char *s);
int64_t bp_int_of(int node, int64_t dflt);

/* building: nodes are added to the pool; containers take children in order */
int bp_new_dict(int max_pairs);
int bp_new_array(int max_items);
int bp_new_str(const char *s);
int bp_new_int(int64_t v);
int bp_new_bool(int v);
int bp_new_real(double v);
int bp_new_data(const uint8_t *p, int n);
int bp_set(int dict, const char *key, int val);       /* returns val, or -1 */
int bp_push(int array, int val);
/* serialise the tree under root; returns length or -1 */
int bp_write(int root, uint8_t *out, int cap);

#endif
