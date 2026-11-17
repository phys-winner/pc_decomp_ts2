/* File blocks in memory for the game's small reads (readcache.c). */
#ifndef VOID_READCACHE_H
#define VOID_READCACHE_H

#include <stdint.h>
#include <sys/types.h>

uint64_t void_readcache_key(const char *host_path);
/* Bytes read at `offset`, -1 with errno, -2 when the cache does not take
 * this read (the caller reads the file itself), or -3 when `fd` is -1 (a
 * file not opened yet) and a block of the read is not in memory. */
ssize_t void_readcache_pread(int fd, uint64_t key, void *out, size_t size, off_t offset);
/* The file at the path (or with the key) changed: its cached blocks are stale. */
void void_readcache_changed(const char *host_path);
void void_readcache_changed_key(uint64_t key);
/* Forget every block (a rename can put another file under a cached path). */
void void_readcache_clear(void);
void void_readcache_stats(uint64_t *hits, uint64_t *misses);
void void_readcache_write_stats(uint64_t *writes, uint64_t *runs);

/* Write-back: small writes stay in the blocks until written back. */
int void_readcache_active(void);
/* `size` bytes written, -1 with errno, or -2 when the write is not cached
 * (the caller writes the file itself, after void_readcache_flush_key). */
ssize_t void_readcache_pwrite(int fd, uint64_t key, const char *host_path, const void *in, size_t size, off_t offset);
/* SetEndOfFile at `size`: 0 when the cache took it (a longer file, or the
 * same size), 1 when the caller changes the file itself (a shorter one,
 * after void_readcache_flush_key). */
int void_readcache_set_end(int fd, uint64_t key, const char *host_path, off_t size);
/* 1 and the file's size with its dirty blocks, or 0 when none are dirty. */
int void_readcache_size(uint64_t key, off_t *size);
/* Write the dirty blocks of a file, of the files in a folder, of all files. */
void void_readcache_flush_key(uint64_t key);
void void_readcache_flush_path(const char *host_path);
void void_readcache_flush_dir(const char *host_dir);
void void_readcache_flush_all(void);

#endif
