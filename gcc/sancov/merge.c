#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#define SET_BIT(mem, pos)   (((uint8_t *)(mem))[(pos) / 8] |= ((uint8_t)1) << ((pos) % 8))
#define CHECK_BIT(mem, pos) ((((uint8_t *)(mem))[(pos) / 8] & ((uint8_t)1) << ((pos) % 8)) != 0)

// Per linux doc the user space virtual memory address space is as follows:
// start: 0000000000000000
// end: 00007fffffffffff
// E.g. we use the first bit of the address to store the actual bit
struct pc_to_bit {
	uint8_t bit : 1;
	uintptr_t pc : 63;
};

int pc_to_bit_comp(const void *a, const void *b) {
	// avoiding subtraction to avoid overflow
	uintptr_t v1 = ((struct pc_to_bit *)a)->pc;
	uintptr_t v2 = ((struct pc_to_bit *)b)->pc;
	if (v1 > v2) return 1;
	else if (v1 < v2) return -1;
	else return 0;
}

struct pc_to_bit_map {
	char *binary;
	struct pc_to_bit *map;
	size_t size;
};

struct merged_map {
	size_t size;
	size_t capacity;
	struct pc_to_bit_map map[];
};

int read_file(char *file, struct pc_to_bit_map *map) {
	// Format: [ 8 binary_name_size ][ binary_name_size binary_name ][ 8 num_edges ][ num_edges*8 PC's ][ (num_edges+1)/8+1 bitmap]
	
	// mmap file into VM to save memory
	int fd = open(file, O_RDONLY);
	if (fd <= -1) {
		perror("Failed to open object file");
		return 1;
	}
	int32_t size = lseek(fd, 0, SEEK_END);
	if (size <= -1) {
		perror("Failed to read size of object file");
		close(fd);
		return 1;
	}

	char *content = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (content == MAP_FAILED) {
		perror("Failed to map object file into virtual address space");
		return 2;
	}

	// read: [ 8 binary_name_size ]
	size_t binary_name_size = *((uintptr_t *)content);
	content += 8;

	// read: [ binary_name_size binary_name ]
	char *binary_name = malloc(binary_name_size + 1);
	if (!binary_name) {
		fprintf(stderr, "failed to allocate for binary name");
		return 1;
	}
	strncpy(binary_name, content, binary_name_size);
	binary_name[binary_name_size] = '\0';
	content += binary_name_size;

	// read: [ 8 num_edges ]
	size_t num_edges = *((uintptr_t *)content);
	content += 8;

	// read [ num_edges*8 PC's ][ (num_edges+1)/8+1 bitmap]
	//      ^ pcs_start         ^ bitmap_start
	uintptr_t *pcs_start = (uintptr_t *) content;
	uint8_t *bitmap_start = (uint8_t *) (pcs_start + num_edges);
	struct pc_to_bit *pcs_map = malloc(sizeof(struct pc_to_bit) * num_edges);
	if (!pcs_map) {
		fprintf(stderr, "failed to allocate for pcs map");
		return 1;
	}
	for (size_t i = 0; i < num_edges; i++) {
		assert((pcs_start[i] & 0x8000000000000000) == 0 && "Non user-space address");
		pcs_map[i].pc = pcs_start[i];
		pcs_map[i].bit = CHECK_BIT(bitmap_start, i + 1);
	}
	qsort(pcs_map, num_edges, sizeof(struct pc_to_bit), pc_to_bit_comp);

	if (munmap(content, size)) {
		fprintf(stderr, "failed to unmap object file");
		return 2;
	}


	map->binary = binary_name;
	map->map = pcs_map;
	map->size = num_edges;

	return 0;
}

int merge_into(struct merged_map **merged_map_ptr, struct pc_to_bit_map *pc_to_bit_map) {
	struct merged_map *merged_map = *merged_map_ptr;
	// first we check if this binary file has alread a predecessor;
	int target;
	for (target = 0; target < merged_map->size; target++) {
		if (strcmp(merged_map->map[target].binary, pc_to_bit_map->binary) == 0) break;
	}

	// new binary we haven't had before
	if (target == merged_map->size) {
		if (merged_map->size == merged_map->capacity) {
			merged_map->capacity *= 2;
			merged_map = realloc(
				merged_map,
				offsetof(struct merged_map, map) + merged_map->capacity * sizeof(struct pc_to_bit_map)
			);

			if (!merged_map) {
				fprintf(stderr, "failed to re-allocate for merged_map");
				return 1;
			}
		}
		memcpy(&merged_map->map[merged_map->size], pc_to_bit_map, sizeof(struct pc_to_bit_map));
		merged_map->size += 1;
	} else { // already seen this binary e.g. Merge
		struct pc_to_bit_map *m1 = &merged_map->map[target], *m2 = pc_to_bit_map;
		assert(m1 != m2 && "never merge the same to maps");
		assert(m1->map != m2->map && "never merge the same to sub maps");
		struct pc_to_bit_map new = {};
		new.binary = m1->binary /*= m2->binary */;
		// we iterate from top down and handle the following cases:
		// m1[i1].pc = m2[i2].pc => new[i].pc = m1[i1].pc; new[i].bit = m1[i1].bit || m2[i1].bit; i1++; i2++
		// m1[i1].pc < m2[i2].pc => new[i].pc = m1[i1].pc; new[i].bin = m1[i1].bit; i1++;
		// m1[i1].pc > m2[i2].pc => new[i].pc = m2[i2].pc; new[i].bin = m2[i2].bit; i2++;
		// but first we need to find the new.size
		size_t i1 = 0, i2 = 0, size = 0;
		while (i1 < m1->size && i2 < m2->size) {
			if (m1->map[i1].pc == m2->map[i2].pc) {
				size += 1;
				i1 += 1;
				i2 += 1;
			} else if (m1->map[i1].pc < m2->map[i2].pc) {
				size += 1;
				i1 += 1;
			} else {
				size += 1;
				i2 += 1;
			}
		}

		// do the remainder of either side
		if (i2 < m2->size) {
			size += m2->size - i2;
		} else if (i1 < m1->size) {
			size += m1->size - i1;
		}

		new.size = size;
		new.map = malloc(sizeof(struct pc_to_bit) * size);
		if (!new.map) {
			fprintf(stderr, "failed to allocate for new pc_to_bit_map");
			return 1;
		}
		size_t i = 0;
		i1 = 0;
		i2 = 0;

		while (i1 < m1->size && i2 < m2->size) {
			if (m1->map[i1].pc == m2->map[i2].pc) {
				new.map[i].pc = m1->map[i1].pc;
				new.map[i].bit = m1->map[i1].bit | m2->map[i2].bit;
				i1 += 1;
				i2 += 1;
			} else if (m1->map[i1].pc < m2->map[i2].pc) {
				new.map[i].pc = m1->map[i1].pc;
				new.map[i].bit = m1->map[i1].bit;
				i1 += 1;
			} else {
				new.map[i].pc = m2->map[i2].pc;
				new.map[i].bit = m2->map[i2].bit;
				i2 += 1;
			}
			i += 1;
		}

		// do the remainder of either side
		if (i2 < m2->size) {
			memcpy(&new.map[i], &m2->map[i2], m2->size - i2);
		} else if (i1 < m1->size) {
			memcpy(&new.map[i], &m1->map[i1], m1->size - i1);
		}

		free(m1->map);
		// not freeing m1->binary since the pointer is reused in new
		free(m2->map);
		free(m2->binary);
		memcpy(&merged_map->map[target], &new, sizeof(struct pc_to_bit_map));
	}

	*merged_map_ptr = merged_map;
	return 0;
}

int write_merged(char *merged_dir, char *merged_name, struct merged_map *merged_map) {
	// Format: [ 8 binary_name_size ][ binary_name_size binary_name ][ 8 num_edges ][ num_edges*8 PC's ][ (num_edges+1)/8+1 bitmap]

	for (size_t i = 0; i < merged_map->size; i++) {
		char merge_path[4096];
		// sure hope all paths are valid
		snprintf(merge_path, sizeof(merge_path), "%s/%s-%ld.bin", merged_dir, merged_name, i);

		FILE *f = fopen(merge_path, "wb");
		if (!f) {
			perror("[write_merged] fopen failed");
			return 1;
		}

		size_t binary_length = strlen(merged_map->map[i].binary);
		// dump size of the binary name
		fwrite(&binary_length, sizeof(binary_length), 1, f);

		// dump binary name
		fwrite(merged_map->map[i].binary, 1, binary_length, f);

		// dump total number of edges
		fwrite(&merged_map->map[i].size, sizeof(merged_map->map[i].size), 1, f);

		// dump PC-table and bitmap
		uint8_t *bitmap = (uint8_t *) calloc((merged_map->map[i].size + 1) / 8 + 1, 1);

		for (size_t j = 0; j < merged_map->map[i].size; j++) {
			if (merged_map->map[i].map[j].bit) SET_BIT(bitmap, j + 1);
			uintptr_t pc = merged_map->map[i].map[j].pc & 0x7FFFFFFFFFFFFFFF;
			fwrite(&pc, sizeof(uintptr_t), 1, f);
		}
		size_t rc = fwrite(bitmap, 1, (merged_map->map[i].size + 1) / 8 + 1, f);

		free(bitmap);
		fclose(f);
	}
	return 0;
}

struct merged_map *merge_live() {
	struct merged_map *merged_map = malloc(offsetof(struct merged_map, map) + 2 * sizeof(struct pc_to_bit_map));
	if (!merged_map) {
		fprintf(stderr, "failed to allocate for first merge_map");
		return NULL;
	}

	merged_map->capacity = 3;
	merged_map->size = 0;

	int ch = ~EOF;
	char path[4096];
	size_t pp = 0;
	while (ch != EOF) {
		pp = 0;
		while ((ch = fgetc(stdin)) != '\n' && ch != EOF) {
			path[pp++] = ch;
		}
		path[pp] = '\0';
		if (path[0] == '\0') break;
		struct pc_to_bit_map map;
		if (read_file(path, &map)) break;
		if (merge_into(&merged_map, &map)) abort();
	}
	return merged_map;
}

int main(int argc, char *argv[]) {
	if (argc != 3) {
		fprintf(stderr, "Usage %s <merge_dir> <merge_name>", argv[0]);
		return 1;
	}
	struct merged_map *mm = merge_live();
	if (!mm) return 1;
	if (write_merged(argv[1], argv[2], mm)) return 1;

	// freeing to get nice Valgrind output not actually nessessary
	for (size_t i = 0; i < mm->size; i++) {
		free(mm->map[i].binary);
		free(mm->map[i].map);
	}

	free(mm);

	return 0;
}
