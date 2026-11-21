/* SPDX-License-Identifier: GPL-2.0-only */
/* cp_diskserver RAM-disk and UBI write semantics. */
#include "nv_ubi_backend.h"
#include "nv_vendor.h"
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <sys/ioctl.h>
#include <mtd/ubi-user.h>
#include <unistd.h>
#include <lz4.h>
#define CHUNK 65536u

struct vendor_block {
	uint8_t *data;
	uint64_t sum;
	uint32_t bytes;
	unsigned packed;
};

struct vendor_state {
	int fd[2];
	uint32_t size;
	unsigned bits, loaded, dirty_count, work_index;
	uint8_t header[NV_HEADER], dirty[NV_MAX_IMAGE / 512u / 8u];
	struct vendor_block block[NV_MAX_IMAGE / CHUNK];
	uint8_t *work, *packed_work;
};

/* Each block is independent. A transaction needs only one decoded block,
 * compression scratch, and the existing UBI output chunk/LEB buffer. */
struct nv_store {
	struct vendor_state cache[2];
	bool runtime_force_full;
};

struct nv_store *nv_store_create(void)
{
	return calloc(1, sizeof(struct nv_store));
}

void nv_vendor_runtime_timestamp(struct nv_store *store, uint32_t fix, uint32_t runtime)
{
	store->runtime_force_full = fix != runtime;
}

static void release_work(struct vendor_state *v)
{
	free(v->work);
	free(v->packed_work);
	v->work = v->packed_work = NULL;
	v->work_index = UINT32_MAX;
}

static void clear_blocks(struct vendor_state *v)
{
	for (unsigned i = 0; i < NV_MAX_IMAGE / CHUNK; i++) {
		free(v->block[i].data);
		memset(&v->block[i], 0, sizeof(v->block[i]));
	}
	v->loaded = 0;
	v->work_index = UINT32_MAX;
}

void nv_store_destroy(struct nv_store *store)
{
	if (!store)
		return;
	for (unsigned i = 0; i < 2; i++) {
		clear_blocks(&store->cache[i]);
		release_work(&store->cache[i]);
		memset(&store->cache[i], 0, sizeof(store->cache[i]));
	}
	free(store);
}

static size_t block_length(const struct vendor_state *v, unsigned index)
{
	size_t left = (size_t)v->size - (size_t)index * CHUNK;
	return left > CHUNK ? CHUNK : left;
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put32(uint8_t *p, uint32_t n)
{
	for (unsigned i = 0; i < 4; i++)
		p[i] = (uint8_t)(n >> (8 * i));
}

static int work_buffer(struct vendor_state *v)
{
	if (!v->work) {
		v->work = malloc(CHUNK);
		v->work_index = UINT32_MAX;
		if (!v->work)
			return NVU_IO;
	}
	return 0;
}

static int store_block(struct vendor_state *v, unsigned index)
{
	size_t length = block_length(v, index);
	if (!v->packed_work) {
		v->packed_work = malloc(CHUNK);
		if (!v->packed_work)
			return NVU_IO;
	}
	int n = LZ4_compress_default((const char *)v->work, (char *)v->packed_work, (int)length,
				     (int)length);
	unsigned packed = n > 0 && (size_t)n < length;
	size_t bytes = packed ? (size_t)n : length;
	uint8_t *data = malloc(bytes);
	if (!data)
		return NVU_IO;
	memcpy(data, packed ? v->packed_work : v->work, bytes);
	struct vendor_block *block = &v->block[index];
	free(block->data);
	*block =
	    (struct vendor_block){data, nv_checksum_sum(v->work, length), (uint32_t)bytes, packed};
	v->work_index = index;
	return 0;
}

static int restore_block(struct vendor_state *v, unsigned index)
{
	if (work_buffer(v))
		return NVU_IO;
	if (v->work_index == index)
		return 0;
	struct vendor_block *block = &v->block[index];
	size_t length = block_length(v, index);
	if (!block->data || !block->bytes || block->bytes > length)
		return NVU_IMAGE;
	if (block->packed) {
		int n = LZ4_decompress_safe((const char *)block->data, (char *)v->work,
					    (int)block->bytes, (int)length);
		if (n != (int)length)
			return NVU_IMAGE;
	} else {
		if (block->bytes != length)
			return NVU_IMAGE;
		memcpy(v->work, block->data, length);
	}
	v->work_index = index;
	return 0;
}

static int fail(struct nv_ubi_backend *b, int error, enum nv_ubi_state state)
{
	b->last_error = error;
	b->state = state;
	return error;
}

/* Check cancellation and the transaction deadline between blocking operations. */
static int progress(struct nv_ubi_backend *b)
{
	uint64_t now;
	if (b->io.now_ms(b->io.ctx, &now))
		return NVU_IO;
	if (now < b->last_ms || now >= b->deadline_ms)
		return NVU_DEADLINE;
	b->last_ms = now;
	return b->io.check_io(b->io.ctx) ? NVU_OWNERSHIP : 0;
}

static int read_exact(struct nv_ubi_backend *b, int fd, uint8_t *p, size_t n, uint64_t off)
{
	while (n) {
		int e = progress(b);
		if (e)
			return e;
		size_t take = n > CHUNK ? CHUNK : n;
		long got = b->io.pread(b->io.ctx, fd, p, take, off);
		if (got <= 0 || (size_t)got > take)
			return NVU_IO;
		p += (size_t)got;
		n -= (size_t)got;
		off += (size_t)got;
	}
	return progress(b);
}

static int load_image(struct nv_ubi_backend *b)
{
	struct vendor_state *v = b->vendor_state;
	if (v->loaded) {
		b->checksum_bits = v->bits;
		return 0;
	}
	int e = work_buffer(v);
	if (e)
		return e;
	unsigned first = (rand() & 1) ? 0u : 1u;
	for (unsigned attempt = 0; attempt < 2; attempt++) {
		unsigned i = first ^ attempt;
		clear_blocks(v);
		e = read_exact(b, b->fd[i], v->header, NV_HEADER, 0);
		if (e == NVU_DEADLINE || e == NVU_OWNERSHIP)
			return e;
		if (e)
			continue;
		uint32_t stored_size = nv_vendor_payload_size(v->header, v->size);
		if (!stored_size)
			continue;
		uint64_t sum = 0;
		for (uint32_t off = 0; off < b->payload_size; off += CHUNK) {
			unsigned index = off / CHUNK;
			size_t length = block_length(v, index);
			/* OEM fixnv loads into a fixed-capacity, erased-byte RAM disk. */
			memset(v->work, 0xff, length);
			size_t stored = off < stored_size ? stored_size - off : 0;
			if (stored > length)
				stored = length;
			if (stored &&
			    (e = read_exact(b, b->fd[i], v->work, stored, (uint64_t)NV_HEADER + off)))
				break;
			sum += nv_checksum_sum(v->work, stored);
			if ((e = store_block(v, index)))
				return e;
		}
		if (e == NVU_DEADLINE || e == NVU_OWNERSHIP)
			return e;
		if (e)
			continue;
		uint32_t wanted = get32(v->header + 8);
		if (wanted == nv_checksum_finish(sum, 64))
			v->bits = 64;
		else if (wanted == nv_checksum_finish(sum, 32))
			v->bits = 32;
		else
			continue;
		v->loaded = 1;
		b->checksum_bits = v->bits;
		return 0;
	}
	clear_blocks(v);
	release_work(v);
	return NVU_IMAGE;
}

static int cached_copy(struct nv_ubi_backend *b, uint64_t off, uint8_t *out, size_t n)
{
	struct vendor_state *v = b->vendor_state;
	uint64_t total = (uint64_t)v->size + NV_HEADER;
	if (!v->loaded || off > total || n > total - off)
		return NVU_BOUNDS;
	if (off < NV_HEADER) {
		size_t take = NV_HEADER - (size_t)off;
		if (take > n)
			take = n;
		memcpy(out, v->header + (size_t)off, take);
		off += take;
		out += take;
		n -= take;
	}
	while (n) {
		unsigned index = (unsigned)((off - NV_HEADER) / CHUNK);
		size_t within = (size_t)((off - NV_HEADER) % CHUNK);
		size_t take = block_length(v, index) - within;
		if (take > n)
			take = n;
		int e = restore_block(v, index);
		if (e)
			return e;
		memcpy(out, v->work + within, take);
		off += take;
		out += take;
		n -= take;
	}
	return 0;
}

static int update_header(struct nv_ubi_backend *b)
{
	struct vendor_state *v = b->vendor_state;
	uint64_t sum = 0;
	for (unsigned index = 0; (uint64_t)index * CHUNK < v->size; index++) {
		int e = progress(b);
		if (e)
			return e;
		/* store_block updates this sum whenever RAM bytes change. */
		sum += v->block[index].sum;
	}
	memset(v->header, 0, NV_HEADER);
	put32(v->header, NV_IMAGE_MAGIC);
	put32(v->header + 4, v->size);
	put32(v->header + 8, nv_checksum_finish(sum, v->bits));
	put32(v->header + 12, NV_IMAGE_FORMAT);
	free(v->packed_work);
	v->packed_work = NULL;
	return 0;
}

int nv_ubi_bind(struct nv_ubi_backend *b, struct nv_store *store, const struct nv_ubi_ops *io,
		int p, int q, const struct nv_ubi_identity expected[2], uint32_t size, int allow,
		uint64_t deadline)
{
	if (!b || !store || !io || !expected || !size || size > NV_MAX_IMAGE ||
	    p < 0 || q < 0 || p == q || !io->pread || !io->write || !io->begin_update ||
	    !io->sync || !io->now_ms || !io->check_io || !io->prepare_direct || !io->write_leb)
		return NVU_CONFIG;
	memset(b, 0, sizeof(*b));
	struct vendor_state *v = NULL;
	for (unsigned i = 0; i < 2; i++)
		if (store->cache[i].size == size && store->cache[i].fd[0] == p &&
		    store->cache[i].fd[1] == q) {
			v = &store->cache[i];
			break;
		}
	if (!v)
		for (unsigned i = 0; i < 2; i++)
			if (!store->cache[i].size) {
				v = &store->cache[i];
				v->fd[0] = p;
				v->fd[1] = q;
				v->size = size;
				break;
			}
	if (!v)
		return NVU_CONFIG;
	b->store = store;
	b->io = *io;
	b->fd[0] = p;
	b->fd[1] = q;
	b->identity[0] = expected[0];
	b->identity[1] = expected[1];
	b->payload_size = size;
	b->allow_write = allow;
	b->deadline_ms = deadline;
	b->vendor_state = v;
	b->checksum_bits = v->bits;
	b->state = NVU_READY;
	return 0;
}

int nv_ubi_begin(struct nv_ubi_backend *b)
{
	if (!b || b->state != NVU_READY)
		return NVU_STATE;
	b->state = NVU_STAGING;
	return 0;
}

int nv_ubi_stage(struct nv_ubi_backend *b, uint32_t off, const uint8_t *data, size_t n)
{
	if (!b || b->state != NVU_STAGING)
		return NVU_STATE;
	if (!data || !n || off > b->payload_size || n > b->payload_size - off)
		return fail(b, NVU_BOUNDS, NVU_REJECTED);
	int e = load_image(b);
	if (e)
		return fail(b, e, NVU_RECOVERY_REQUIRED);
	struct vendor_state *v = b->vendor_state;
	uint32_t pos = off;
	size_t left = n;
	while (left) {
		unsigned index = pos / CHUNK;
		size_t within = pos % CHUNK, take = block_length(v, index) - within;
		if (take > left)
			take = left;
		if ((e = restore_block(v, index)))
			return fail(b, e, NVU_RECOVERY_REQUIRED);
		memcpy(v->work + within, data, take);
		v->work_index = UINT32_MAX;
		if ((e = store_block(v, index)))
			return fail(b, e, NVU_RECOVERY_REQUIRED);
		pos += (uint32_t)take;
		data += take;
		left -= take;
	}
	for (uint32_t sector = off / 512u; sector <= (off + (uint32_t)n - 1u) / 512u; sector++) {
		uint8_t mask = (uint8_t)(1u << (sector % 8u));
		if (!(v->dirty[sector / 8u] & mask)) {
			v->dirty[sector / 8u] |= mask;
			v->dirty_count++;
		}
	}
	return 0;
}

static int put_volume(struct nv_ubi_backend *b, unsigned i)
{
	size_t total = (size_t)b->payload_size + NV_HEADER;
	int e = progress(b);
	if (e)
		return e;
	uint8_t *output = malloc(CHUNK);
	if (!output)
		return NVU_IO;
	b->possibly_changed |= 1u << i;
	b->step[i] = NVU_UPDATE_ATTEMPTED;
	if (b->io.begin_update(b->io.ctx, b->fd[i], total)) {
		e = NVU_IO;
		goto out;
	}
	for (size_t off = 0; off < total;) {
		if ((e = progress(b)))
			goto out;
		size_t take = total - off > CHUNK ? CHUNK : total - off;
		if ((e = cached_copy(b, off, output, take)))
			goto out;
		long written = b->io.write(b->io.ctx, b->fd[i], output, take);
		if (written <= 0 || (size_t)written > take) {
			e = NVU_IO;
			goto out;
		}
		off += (size_t)written;
	}
	b->step[i] = NVU_STREAM_FINISHED;
out:
	free(output);
	return e;
}

/* OEM ubi_leb_write: header LEB, then dirty payload LEBs. Preserve bytes beyond
 * the image in its final LEB instead of reading beyond the RAM allocation. */
static int put_lebs(struct nv_ubi_backend *b, unsigned i)
{
	struct vendor_state *v = b->vendor_state;
	uint32_t leb = b->identity[i].leb_size;
	uint64_t total = (uint64_t)b->payload_size + NV_HEADER;
	int e = 0;
	if (leb <= NV_HEADER || leb > total)
		return NVU_CONFIG;
	uint8_t *block = malloc(leb);
	if (!block)
		return NVU_IO;
	if ((e = progress(b)))
		goto out;
	if (b->io.prepare_direct(b->io.ctx, b->fd[i])) {
		e = NVU_IO;
		goto out;
	}
	for (uint64_t off = 0; off < total; off += leb) {
		unsigned needed = off == 0;
		uint32_t first = off > NV_HEADER ? (uint32_t)((off - NV_HEADER) / 512u) : 0;
		uint32_t last =
		    (uint32_t)(((off + leb < total ? off + leb : total) - NV_HEADER - 1u) / 512u);
		for (uint32_t sector = first; !needed && sector <= last; sector++)
			needed = !!(v->dirty[sector / 8u] & (1u << (sector % 8u)));
		if (!needed)
			continue;
		size_t take = total - off < leb ? (size_t)(total - off) : leb;
		if (take < leb && (e = read_exact(b, b->fd[i], block, leb, off)))
			goto out;
		if ((e = cached_copy(b, off, block, take)))
			goto out;
		if ((e = progress(b)))
			goto out;
		b->possibly_changed |= 1u << i;
		b->step[i] = NVU_UPDATE_ATTEMPTED;
		if (b->io.write_leb(b->io.ctx, b->fd[i], off, block, leb)) {
			e = NVU_IO;
			goto out;
		}
	}
	b->step[i] = NVU_STREAM_FINISHED;
out:
	free(block);
	return e;
}

static int stock_put(struct nv_ubi_backend *b, unsigned i, int expand_image)
{
	struct vendor_state *v = b->vendor_state;
	int full = expand_image || b->payload_size / 512u < 2u * v->dirty_count ||
		   (b->payload_size == 9437184u && b->store->runtime_force_full);
	int e = full ? put_volume(b, i) : put_lebs(b, i);
	if (e)
		return e;
	if (b->io.sync(b->io.ctx, b->fd[i]))
		return NVU_IO;
	b->step[i] = NVU_SYNCED;
	return progress(b);
}

int nvc_verify_noop(struct nv_ubi_backend *b, uint32_t group, uint32_t sub)
{
	if (!b || b->state != NVU_STAGING)
		return NVU_STATE;
	b->group = group;
	b->sub = sub;
	b->storage_verified = 1;
	b->state = NVU_BOTH_VERIFIED;
	return 0;
}

int nv_ubi_commit(struct nv_ubi_backend *b, uint32_t group, uint32_t sub)
{
	if (!b || b->state != NVU_STAGING)
		return NVU_STATE;
	if (!b->allow_write)
		return fail(b, NVU_READONLY, NVU_REJECTED);
	struct vendor_state *v = b->vendor_state;
	int e;
	b->group = group;
	b->sub = sub;
	if (!v->dirty_count)
		return nvc_verify_noop(b, group, sub);
	/* The first write to a short image publishes the padded logical disk. */
	int expand_image = get32(v->header + 4) != v->size;
	if ((e = update_header(b)))
		return fail(b, e, NVU_REJECTED);
	b->state = NVU_UPDATING_BACKUP;
	if ((e = stock_put(b, 1, expand_image)))
		return fail(b, e, NVU_UNCERTAIN);
	/* The original protocol checks the backup header checksum before updating
	 * the primary. A full payload readback is not part of this commit path. */
	uint8_t header[NV_HEADER];
	if ((e = read_exact(b, b->fd[1], header, sizeof(header), 0)))
		return fail(b, e, NVU_UNCERTAIN);
	if (memcmp(header + 8, v->header + 8, 4))
		return fail(b, NVU_IMAGE, NVU_UNCERTAIN);
	b->state = NVU_UPDATING_PRIMARY;
	if ((e = stock_put(b, 0, expand_image)))
		return fail(b, e, NVU_UNCERTAIN);
	memset(v->dirty, 0, sizeof(v->dirty));
	v->dirty_count = 0;
	if (b->payload_size == 9437184u)
		b->store->runtime_force_full = 0;
	b->storage_verified = 1;
	b->state = NVU_BOTH_VERIFIED;
	return 0;
}

int nv_ubi_prepare_final_ack(struct nv_ubi_backend *b, uint8_t out[NV_RESPONSE_SIZE])
{
	if (!b || !out || b->state != NVU_BOTH_VERIFIED || !b->storage_verified || b->ack_attempted)
		return NVU_STATE;
	struct nv_frame f = {0};
	f.group = b->group;
	f.sub = b->sub;
	if (nv_response_encode(&f, 1, out))
		return fail(b, NVU_REPLY_ERROR, NVU_REPLY_LOST);
	b->ack_attempted = 1;
	b->state = NVU_REPLY_PREPARED;
	return 0;
}

int nv_ubi_note_ack_result(struct nv_ubi_backend *b, size_t n)
{
	if (!b || b->state != NVU_REPLY_PREPARED)
		return NVU_STATE;
	b->state = n == NV_RESPONSE_SIZE ? NVU_REPLY_WRITTEN : NVU_REPLY_LOST;
	b->last_error = n == NV_RESPONSE_SIZE ? 0 : NVU_REPLY_ERROR;
	if (!b->last_error)
		release_work(b->vendor_state);
	return b->last_error;
}

void nv_ubi_abort(struct nv_ubi_backend *b)
{
	if (!b)
		return;
	if (b->state == NVU_STAGING) {
		struct vendor_state *v = b->vendor_state;
		if (v) {
			v->loaded = 0;
			memset(v->dirty, 0, sizeof(v->dirty));
			v->dirty_count = 0;
			clear_blocks(v);
			release_work(v);
		}
	}
	b->state = b->storage_verified	 ? NVU_REPLY_LOST
		   : b->possibly_changed ? NVU_UNCERTAIN
					 : NVU_REJECTED;
}

void nv_ubi_forget(struct nv_ubi_backend *b)
{
	if (!b)
		return;
	/* Successful END preserves the cached bytes, possibly compressed. Aborted
	 * transactions invalidate them so the next WRITE reloads the stored image. */
	b->vendor_state = NULL;
	b->state = NVU_CLOSED;
}
