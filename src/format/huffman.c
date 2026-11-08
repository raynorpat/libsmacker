/**
	libsmacker - A C library for decoding .smk Smacker Video files
	Copyright (C) 2012-2021 Greg Kennedy

	See smacker.h for more information.

	smacker.c
		Main implementation file of libsmacker.
		Open, close, query, render, advance and seek an smk
*/

#include "huffman.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t freq;
    int left;
    int right;
    uint16_t symbol;
    uint8_t leaf;
} hnode;

static int heap_less(const hnode *nodes, int a, int b)
{
    if (nodes[a].freq != nodes[b].freq)
        return nodes[a].freq < nodes[b].freq;
    return a < b;
}

static void heap_push(int *heap, int *count, int value, const hnode *nodes)
{
    int i = (*count)++;

    heap[i] = value;
    while (i > 0) {
        int parent = (i - 1) / 2;
        if (!heap_less(nodes, heap[i], heap[parent]))
            break;
        {
            int tmp = heap[i];
            heap[i] = heap[parent];
            heap[parent] = tmp;
        }
        i = parent;
    }
}

static int heap_pop(int *heap, int *count, const hnode *nodes)
{
    int result = heap[0];
    int i = 0;

    (*count)--;
    if (*count == 0)
        return result;
    heap[0] = heap[*count];

    for (;;) {
        int left = i * 2 + 1;
        int right = left + 1;
        int smallest = i;

        if (left < *count && heap_less(nodes, heap[left], heap[smallest]))
            smallest = left;
        if (right < *count && heap_less(nodes, heap[right], heap[smallest]))
            smallest = right;
        if (smallest == i)
            break;
        {
            int tmp = heap[i];
            heap[i] = heap[smallest];
            heap[smallest] = tmp;
        }
        i = smallest;
    }
    return result;
}

static int assign_codes(const hnode *nodes, int index, uint32_t code,
                        unsigned depth, uint32_t *codes, uint8_t *lens)
{
    if (nodes[index].leaf) {
        if (depth > 31)
            return -1;
        codes[nodes[index].symbol] = code;
        lens[nodes[index].symbol] = (uint8_t)depth;
        return 0;
    }

    if (depth >= 31)
        return -1;
    if (assign_codes(nodes, nodes[index].left, code << 1, depth + 1,
                     codes, lens) != 0)
        return -1;
    if (assign_codes(nodes, nodes[index].right, (code << 1) | 1u, depth + 1,
                     codes, lens) != 0)
        return -1;
    return 0;
}

static int balanced_rec(hnode *nodes, int *count, const uint16_t *symbols,
                        int n, const uint32_t *freq)
{
    int index;
    int left;
    int right;

    if (n == 1) {
        index = (*count)++;
        nodes[index].freq = freq[symbols[0]];
        nodes[index].left = -1;
        nodes[index].right = -1;
        nodes[index].symbol = symbols[0];
        nodes[index].leaf = 1;
        return index;
    }

    left = balanced_rec(nodes, count, symbols, n / 2, freq);
    right = balanced_rec(nodes, count, symbols + n / 2, n - n / 2, freq);
    index = (*count)++;
    nodes[index].freq = nodes[left].freq + nodes[right].freq;
    nodes[index].left = left;
    nodes[index].right = right;
    nodes[index].symbol = 0;
    nodes[index].leaf = 0;
    return index;
}

static int build_tree(const uint32_t *freq, unsigned max_symbols,
                      hnode **out_nodes, int *out_count, int *out_root,
                      uint32_t *codes, uint8_t *lens)
{
    unsigned used = 0;
    unsigned i;
    int *heap;
    int heap_count = 0;
    int node_count = 0;
    hnode *nodes;
    int root;
    uint16_t *symbols = NULL;

    for (i = 0; i < max_symbols; i++) {
        if (freq[i] != 0)
            used++;
    }
    *out_nodes = NULL;
    *out_count = 0;
    *out_root = -1;
    if (used == 0)
        return 0;

    nodes = (hnode *)calloc(used * 2 - 1, sizeof(*nodes));
    heap = (int *)malloc(used * sizeof(*heap));
    if (nodes == NULL || heap == NULL) {
        free(nodes);
        free(heap);
        return -1;
    }

    for (i = 0; i < max_symbols; i++) {
        if (freq[i] == 0)
            continue;
        nodes[node_count].freq = freq[i];
        nodes[node_count].left = -1;
        nodes[node_count].right = -1;
        nodes[node_count].symbol = (uint16_t)i;
        nodes[node_count].leaf = 1;
        heap_push(heap, &heap_count, node_count, nodes);
        node_count++;
    }

    while (heap_count > 1) {
        int a = heap_pop(heap, &heap_count, nodes);
        int b = heap_pop(heap, &heap_count, nodes);

        nodes[node_count].freq = nodes[a].freq + nodes[b].freq;
        nodes[node_count].left = a;
        nodes[node_count].right = b;
        nodes[node_count].symbol = 0;
        nodes[node_count].leaf = 0;
        heap_push(heap, &heap_count, node_count, nodes);
        node_count++;
    }
    root = heap_pop(heap, &heap_count, nodes);
    free(heap);

    if (assign_codes(nodes, root, 0, 0, codes, lens) != 0) {
        /* Pathological frequency distribution: rebuild a balanced tree. */
        unsigned n = 0;

        symbols = (uint16_t *)malloc(used * sizeof(*symbols));
        if (symbols == NULL) {
            free(nodes);
            return -1;
        }
        for (i = 0; i < max_symbols; i++) {
            if (freq[i] != 0)
                symbols[n++] = (uint16_t)i;
        }
        node_count = 0;
        root = balanced_rec(nodes, &node_count, symbols, (int)used, freq);
        free(symbols);
        if (assign_codes(nodes, root, 0, 0, codes, lens) != 0) {
            free(nodes);
            return -1;
        }
    }

    *out_nodes = nodes;
    *out_count = node_count;
    *out_root = root;
    return 0;
}

void smk_huff_init(smk_huff *h)
{
    if (h != NULL)
        memset(h, 0, sizeof(*h));
}

void smk_huff_free(smk_huff *h)
{
    if (h == NULL)
        return;
    free(h->nodes);
    h->nodes = NULL;
    h->node_count = 0;
    h->root = -1;
    h->built = 0;
}

void smk_huff_clear(smk_huff *h)
{
    if (h == NULL)
        return;
    smk_huff_free(h);
    memset(h->freq, 0, sizeof(h->freq));
    memset(h->code, 0, sizeof(h->code));
    memset(h->len, 0, sizeof(h->len));
    h->used = 0;
}

void smk_huff_count(smk_huff *h, uint16_t symbol)
{
    if (h != NULL)
        h->freq[symbol]++;
}

int smk_huff_build(smk_huff *h)
{
    unsigned used = 0;
    unsigned i;

    if (h == NULL)
        return -1;
    smk_huff_free(h);
    for (i = 0; i < SMK_HUFF_MAX_SYMBOLS; i++) {
        if (h->freq[i] != 0)
            used++;
    }
    h->used = used;
    if (build_tree(h->freq, SMK_HUFF_MAX_SYMBOLS, (hnode **)&h->nodes,
                   &h->node_count, &h->root, h->code, h->len) != 0)
        return -1;
    h->built = 1;
    return 0;
}

unsigned smk_huff_packed_nodes(const smk_huff *h)
{
    if (h == NULL || h->node_count == 0)
        return 0;
    return (unsigned)h->node_count;
}

static int put_code(smk_bitwriter *bw, uint32_t code, uint8_t len)
{
    int i;

    for (i = (int)len - 1; i >= 0; i--) {
        if (smk_bw_put_bit(bw, (code >> i) & 1u) != 0)
            return -1;
    }
    return 0;
}

int smk_huff_encode(const smk_huff *h, uint16_t symbol, smk_bitwriter *bw)
{
    if (h == NULL || bw == NULL || !h->built)
        return -1;
    if (h->freq[symbol] == 0)
        return -1;
    return put_code(bw, h->code[symbol], h->len[symbol]);
}

static int serialize8(smk_bitwriter *bw, const hnode *nodes, int root,
                      uint32_t *codes, uint8_t *lens)
{
    (void)codes;
    (void)lens;
    if (nodes == NULL || root < 0) {
        return smk_bw_put_bit(bw, 0) || smk_bw_put_bit(bw, 0) ? -1 : 0;
    }

    {
        /* Iterative pre-order to avoid deep recursion on pathological trees. */
        int stack[512];
        int top = 0;

        if (smk_bw_put_bit(bw, 1) != 0)
            return -1;
        stack[top++] = root;
        while (top > 0) {
            int index = stack[--top];
            if (nodes[index].leaf) {
                if (smk_bw_put_bit(bw, 0) != 0 ||
                    smk_bw_put_bits(bw, nodes[index].symbol, 8) != 0)
                    return -1;
            } else {
                if (smk_bw_put_bit(bw, 1) != 0)
                    return -1;
                if (top + 2 > (int)(sizeof(stack) / sizeof(stack[0])))
                    return -1;
                /* Left must be serialized first: push right, then left. */
                stack[top++] = nodes[index].right;
                stack[top++] = nodes[index].left;
            }
        }
        if (smk_bw_put_bit(bw, 0) != 0)
            return -1;
    }
    return 0;
}

static int serialize16_body(smk_bitwriter *bw, const hnode *nodes, int index,
                            const hnode *low, int low_root,
                            const uint32_t *low_codes, const uint8_t *low_lens,
                            const hnode *high, int high_root,
                            const uint32_t *high_codes, const uint8_t *high_lens)
{
    if (nodes[index].leaf) {
        uint16_t symbol = nodes[index].symbol;

        if (smk_bw_put_bit(bw, 0) != 0)
            return -1;
        if (put_code(bw, low_codes[symbol & 0xff],
                     low_lens[symbol & 0xff]) != 0)
            return -1;
        if (put_code(bw, high_codes[symbol >> 8],
                     high_lens[symbol >> 8]) != 0)
            return -1;
        (void)low;
        (void)low_root;
        (void)high;
        (void)high_root;
        return 0;
    }

    if (smk_bw_put_bit(bw, 1) != 0)
        return -1;
    if (serialize16_body(bw, nodes, nodes[index].left, low, low_root,
                         low_codes, low_lens, high, high_root,
                         high_codes, high_lens) != 0)
        return -1;
    return serialize16_body(bw, nodes, nodes[index].right, low, low_root,
                            low_codes, low_lens, high, high_root,
                            high_codes, high_lens);
}

static int pick_escapes(const smk_huff *h, uint16_t escapes[3])
{
    unsigned count = 0;
    unsigned value;

    for (value = 0; value < SMK_HUFF_MAX_SYMBOLS && count < 3; value++) {
        if (h->freq[value] == 0)
            escapes[count++] = (uint16_t)value;
    }
    return count == 3 ? 0 : -1;
}

int smk_huff_serialize(const smk_huff *h, smk_bitwriter *bw)
{
    uint32_t low_freq[256];
    uint32_t high_freq[256];
    uint32_t low_codes[256];
    uint8_t low_lens[256];
    uint32_t high_codes[256];
    uint8_t high_lens[256];
    hnode *low_nodes = NULL;
    hnode *high_nodes = NULL;
    int low_count = 0;
    int high_count = 0;
    int low_root = -1;
    int high_root = -1;
    uint16_t escapes[3];
    unsigned i;
    int result = -1;

    if (h == NULL || bw == NULL || !h->built)
        return -1;

    if (h->node_count == 0) {
        if (smk_bw_put_bit(bw, 0) != 0)
            return -1;
        return smk_bw_put_bit(bw, 0);
    }

    memset(low_freq, 0, sizeof(low_freq));
    memset(high_freq, 0, sizeof(high_freq));
    memset(low_codes, 0, sizeof(low_codes));
    memset(low_lens, 0, sizeof(low_lens));
    memset(high_codes, 0, sizeof(high_codes));
    memset(high_lens, 0, sizeof(high_lens));

    for (i = 0; i < SMK_HUFF_MAX_SYMBOLS; i++) {
        if (h->freq[i] == 0)
            continue;
        low_freq[i & 0xff] += h->freq[i];
        high_freq[i >> 8] += h->freq[i];
    }

    if (build_tree(low_freq, 256, &low_nodes, &low_count, &low_root,
                   low_codes, low_lens) != 0)
        goto done;
    if (build_tree(high_freq, 256, &high_nodes, &high_count, &high_root,
                   high_codes, high_lens) != 0)
        goto done;

    if (pick_escapes(h, escapes) != 0)
        goto done; /* fewer than three unused 16-bit values */

    if (smk_bw_put_bit(bw, 1) != 0)
        goto done;
    if (serialize8(bw, low_nodes, low_root, low_codes, low_lens) != 0)
        goto done;
    if (serialize8(bw, high_nodes, high_root, high_codes, high_lens) != 0)
        goto done;
    for (i = 0; i < 3; i++) {
        if (smk_bw_put_bits(bw, escapes[i] & 0xff, 8) != 0 ||
            smk_bw_put_bits(bw, escapes[i] >> 8, 8) != 0)
            goto done;
    }
    if (serialize16_body(bw, (const hnode *)h->nodes, h->root,
                         low_nodes, low_root, low_codes, low_lens,
                         high_nodes, high_root, high_codes, high_lens) != 0)
        goto done;
    if (smk_bw_put_bit(bw, 0) != 0)
        goto done;

    result = 0;

done:
    free(low_nodes);
    free(high_nodes);
    return result;
}

/* ************************************************************************* */
/* HUFF8 Structure */
/* ************************************************************************* */
#define SMK_HUFF8_BRANCH 0x8000
#define SMK_HUFF8_LEAF_MASK 0x7FFF

/* ************************************************************************* */
/* HUFF8 Functions */
/* ************************************************************************* */
/* Recursive sub-func for building a tree into an array. */
static int _smk_huff8_build_rec(smk_huff8 * const t, smk_bitreader * const bs)
{
	int bit, value;
	assert(t);
	assert(bs);

	/* Make sure we aren't running out of bounds */
	if (t->size >= 511) {
		fputs("libsmacker::_smk_huff8_build_rec() - ERROR: size exceeded\n", stderr);
		return 0;
	}

	/* Read the next bit */
	if ((bit = smk_bs_read_1(bs)) < 0) {
		fputs("libsmacker::_smk_huff8_build_rec() - ERROR: get_bit returned -1\n", stderr);
		return 0;
	}

	if (bit) {
		/* Bit set: this forms a Branch node.
			what we have to do is build the left-hand branch,
			assign the "jump" address,
			then build the right hand branch from there.
		*/
		/* track the current index */
		value = t->size ++;

		/* go build the left branch */
		if (! _smk_huff8_build_rec(t, bs)) {
			fputs("libsmacker::_smk_huff8_build_rec() - ERROR: failed to build left sub-tree\n", stderr);
			return 0;
		}

		/* now go back to our current location, and
			mark our location as a "jump" */
		t->tree[value] = SMK_HUFF8_BRANCH | t->size;

		/* continue building the right side */
		if (! _smk_huff8_build_rec(t, bs)) {
			fputs("libsmacker::_smk_huff8_build_rec() - ERROR: failed to build right sub-tree\n", stderr);
			return 0;
		}
	} else {
		/* Bit unset signifies a Leaf node. */
		/* Attempt to read value */
		if ((value = smk_bs_read_8(bs)) < 0) {
			fputs("libsmacker::_smk_huff8_build_rec() - ERROR: get_byte returned -1\n", stderr);
			return 0;
		}

		/* store to tree */
		t->tree[t->size ++] = value;
	}

	return 1;
}

/**
	Build an 8-bit Hufftree out of a Bitstream.
*/
int smk_huff8_build(smk_huff8 * const t, smk_bitreader * const bs)
{
	int bit;
	/* null check */
	assert(t);
	assert(bs);

	/* Smacker huff trees begin with a set-bit. */
	if ((bit = smk_bs_read_1(bs)) < 0) {
		fputs("libsmacker::smk_huff8_build() - ERROR: initial get_bit returned -1\n", stderr);
		return 0;
	}

	/* OK to fill out the struct now */
	t->size = 0;

	/* First bit indicates whether a tree is present or not. */
	/*  Very small or audio-only files may have no tree. */
	if (bit) {
		if (! _smk_huff8_build_rec(t, bs)) {
			fputs("libsmacker::smk_huff8_build() - ERROR: tree build failed\n", stderr);
			return 0;
		}
	} else
		t->tree[0] = 0;

	/* huff trees end with an unset-bit */
	if ((bit = smk_bs_read_1(bs)) < 0) {
		fputs("libsmacker::smk_huff8_build() - ERROR: final get_bit returned -1\n", stderr);
		return 0;
	}

	/* a 0 is expected here, a 1 generally indicates a problem! */
	if (bit) {
		fputs("libsmacker::smk_huff8_build() - ERROR: final get_bit returned 1\n", stderr);
		return 0;
	}

	return 1;
}

/* Look up an 8-bit value from a basic huff tree.
	Return -1 on error. */
int smk_huff8_lookup(const smk_huff8 * const t, smk_bitreader * const bs)
{
	int bit, index = 0;
	/* null check */
	assert(t);
	assert(bs);

	while (t->tree[index] & SMK_HUFF8_BRANCH) {
		if ((bit = smk_bs_read_1(bs)) < 0) {
			fputs("libsmacker::smk_huff8_lookup() - ERROR: get_bit returned -1\n", stderr);
			return -1;
		}

		if (bit) {
			/* take the right branch */
			index = t->tree[index] & SMK_HUFF8_LEAF_MASK;
		} else {
			/* take the left branch */
			index ++;
		}
	}

	/* at leaf node.  return the value at this point. */
	return t->tree[index];
}

/* ************************************************************************* */
/* HUFF16 Structure */
/* ************************************************************************* */
#define SMK_HUFF16_BRANCH    0x80000000
#define SMK_HUFF16_CACHE     0x40000000
#define SMK_HUFF16_LEAF_MASK 0x3FFFFFFF

/* ************************************************************************* */
/* HUFF16 Functions */
/* ************************************************************************* */
/* Recursive sub-func for building a tree into an array. */
static int _smk_huff16_build_rec(smk_huff16 * const t, smk_bitreader * const bs, const smk_huff8 * const low8, const smk_huff8 * const hi8, const size_t limit)
{
	int bit, value;
	assert(t);
	assert(bs);
	assert(low8);
	assert(hi8);

	/* Make sure we aren't running out of bounds */
	if (t->size >= limit) {
		fputs("libsmacker::_smk_huff16_build_rec() - ERROR: size exceeded\n", stderr);
		return 0;
	}

	/* Read the first bit */
	if ((bit = smk_bs_read_1(bs)) < 0) {
		fputs("libsmacker::_smk_huff16_build_rec() - ERROR: get_bit returned -1\n", stderr);
		return 0;
	}

	if (bit) {
		/* See tree-in-array explanation for HUFF8 above */
		/* track the current index */
		value = t->size ++;

		/* go build the left branch */
		if (! _smk_huff16_build_rec(t, bs, low8, hi8, limit)) {
			fputs("libsmacker::_smk_huff16_build_rec() - ERROR: failed to build left sub-tree\n", stderr);
			return 0;
		}

		/* now go back to our current location, and
			mark our location as a "jump" */
		t->tree[value] = SMK_HUFF16_BRANCH | t->size;

		/* continue building the right side */
		if (! _smk_huff16_build_rec(t, bs, low8, hi8, limit)) {
			fputs("libsmacker::_smk_huff16_build_rec() - ERROR: failed to build right sub-tree\n", stderr);
			return 0;
		}
	} else {
		/* Bit unset signifies a Leaf node. */
		/* Attempt to read LOW value */
		if ((value = smk_huff8_lookup(low8, bs)) < 0) {
			fputs("libsmacker::_smk_huff16_build_rec() - ERROR: get LOW value returned -1\n", stderr);
			return 0;
		}

		t->tree[t->size] = value;

		/* now read HIGH value */
		if ((value = smk_huff8_lookup(hi8, bs)) < 0) {
			fputs("libsmacker::_smk_huff16_build_rec() - ERROR: get HIGH value returned -1\n", stderr);
			return 0;
		}

		/* Looks OK: we got low and hi values. Return a new LEAF */
		t->tree[t->size] |= (value << 8);

		/* Last: when building the tree, some Values may correspond to cache positions.
			Identify these values and set the Escape code byte accordingly. */
		if (t->tree[t->size] == t->cache[0])
			t->tree[t->size] = SMK_HUFF16_CACHE;
		else if (t->tree[t->size] == t->cache[1])
			t->tree[t->size] = SMK_HUFF16_CACHE | 1;
		else if (t->tree[t->size] == t->cache[2])
			t->tree[t->size] = SMK_HUFF16_CACHE | 2;

		t->size ++;
	}

	return 1;
}

/* Entry point for building a big 16-bit tree. */
int smk_huff16_build(smk_huff16 * const t, smk_bitreader * const bs, uint32_t alloc_size)
{
	smk_huff8 low8, hi8;
	size_t limit;
	int value, i, bit;
	/* null check */
	assert(t);
	assert(bs);

	/* Smacker huff trees begin with a set-bit. */
	if ((bit = smk_bs_read_1(bs)) < 0) {
		fputs("libsmacker::smk_huff16_build() - ERROR: initial get_bit returned -1\n", stderr);
		return 0;
	}

	t->size = 0;

	/* First bit indicates whether a tree is present or not. */
	/*  Very small or audio-only files may have no tree. */
	if (bit) {
		/* build low-8-bits tree */
		if (! smk_huff8_build(&low8, bs)) {
			fputs("libsmacker::smk_huff16_build() - ERROR: failed to build LOW tree\n", stderr);
			return 0;
		}

		/* build hi-8-bits tree */
		if (! smk_huff8_build(&hi8, bs)) {
			fputs("libsmacker::smk_huff16_build() - ERROR: failed to build HIGH tree\n", stderr);
			return 0;
		}

		/* Init the escape code cache. */
		for (i = 0; i < 3; i ++) {
			if ((value = smk_bs_read_8(bs)) < 0) {
				fprintf(stderr, "libsmacker::smk_huff16_build() - ERROR: get LOW value for cache %d returned -1\n", i);
				return 0;
			}

			t->cache[i] = value;

			/* now read HIGH value */
			if ((value = smk_bs_read_8(bs)) < 0) {
				fprintf(stderr, "libsmacker::smk_huff16_build() - ERROR: get HIGH value for cache %d returned -1\n", i);
				return 0;
			}

			t->cache[i] |= (value << 8);
		}

		/* Everything looks OK so far. Time to malloc structure. */
		if (alloc_size < 12 || alloc_size % 4) {
			fprintf(stderr, "libsmacker::smk_huff16_build() - ERROR: illegal value %u for alloc_size\n", alloc_size);
			return 0;
		}

		limit = (alloc_size - 12) / 4;

		if ((t->tree = malloc(limit * sizeof(uint32_t))) == NULL) {
			perror("libsmacker::smk_huff16_build() - ERROR: failed to malloc() huff16 tree");
			return 0;
		}

		/* Finally, call recursive function to retrieve the Bigtree. */
		if (! _smk_huff16_build_rec(t, bs, &low8, &hi8, limit)) {
			fputs("libsmacker::smk_huff16_build() - ERROR: failed to build huff16 tree\n", stderr);
			free(t->tree);
			t->tree = NULL;
			return 0;
		}

		/* check that we completely filled the tree */
		if (limit != t->size) {
			fputs("libsmacker::smk_huff16_build() - ERROR: failed to completely decode huff16 tree\n", stderr);
			free(t->tree);
			t->tree = NULL;
			return 0;
		}
	} else {
		if ((t->tree = malloc(sizeof(uint32_t))) == NULL) {
			perror("libsmacker::smk_huff16_build() - ERROR: failed to malloc() huff16 tree");
			return 0;
		}

		t->tree[0] = 0;
		//t->cache[0] = t->cache[1] = t->cache[2] = 0;
	}

	/* Check final end tag. */
	if ((bit = smk_bs_read_1(bs)) < 0) {
		fputs("libsmacker::smk_huff16_build() - ERROR: final get_bit returned -1\n", stderr);
		free(t->tree);
		t->tree = NULL;
		return 0;
	}

	/* a 0 is expected here, a 1 generally indicates a problem! */
	if (bit) {
		fputs("libsmacker::smk_huff16_build() - ERROR: final get_bit returned 1\n", stderr);
		free(t->tree);
		t->tree = NULL;
		return 0;
	}

	return 1;
}

/* Look up a 16-bit value from a large huff tree.
	Return -1 on error.
	Note that this also updates the recently-used-values cache. */
int smk_huff16_lookup(smk_huff16 * const t, smk_bitreader * const bs)
{
	int bit, value, index = 0;
	/* null check */
	assert(t);
	assert(bs);

	while (t->tree[index] & SMK_HUFF16_BRANCH) {
		if ((bit = smk_bs_read_1(bs)) < 0) {
			fputs("libsmacker::smk_huff16_lookup() - ERROR: get_bit returned -1\n", stderr);
			return -1;
		}

		if (bit) {
			/* take the right branch */
			index = t->tree[index] & SMK_HUFF16_LEAF_MASK;
		} else {
			/* take the left branch */
			index ++;
		}
	}

	/* Get the value at this point */
	value = t->tree[index];

	if (value & SMK_HUFF16_CACHE) {
		/* uses cached value instead of actual value */
		value = t->cache[value & SMK_HUFF16_LEAF_MASK];
	}

	if (t->cache[0] != value) {
		/* Update the cache, by moving val to the front of the queue,
			if it isn't already there. */
		t->cache[2] = t->cache[1];
		t->cache[1] = t->cache[0];
		t->cache[0] = value;
	}

	return value;
}


void smk_huff16_free(smk_huff16 *t)
{
    if (t == NULL)
        return;
    free(t->tree);
    t->tree = NULL;
    t->size = 0;
}
