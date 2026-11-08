#include "huffman.h"

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
