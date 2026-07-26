#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <sanitizer/coverage_interface.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint8_t *bitmap = NULL;
static uintptr_t *edge_to_pc = NULL;
static uint64_t num_edges = 0;
static uint64_t num_pcs = 0;

#define SET_BIT(mem, pos)   (((uint8_t *)(mem))[(pos) / 8] |= ((uint8_t)1) << ((pos) % 8))
#define CHECK_BIT(mem, pos) ((((uint8_t *)(mem))[(pos) / 8] & ((uint8_t)1) << ((pos) % 8)) != 0)

void __sanitizer_cov_pcs_init(const uintptr_t *start, const uintptr_t *stop) {
	if (start == stop) return;

	num_pcs = (stop - start) / 2;
	edge_to_pc = (uintptr_t *) calloc(num_pcs + 1, sizeof(uintptr_t));
	if (!edge_to_pc) {
		perror("[__sanitizer_cov_pcs_init] calloc failed");
		abort();
	}

	for (size_t i = 0; i < num_pcs; i++) {
		assert(stop > &start[2 * i]);
		edge_to_pc[i + 1] = start[2 * i];
		//printf("%p\n", (void *)edge_to_pc[i + 1]);
	}
}

void __sanitizer_cov_trace_pc_guard_init(uint32_t *start, uint32_t *stop) {
	if (bitmap) return;
	if (start == stop || *start) return;
	for (uint32_t *x = start; x < stop; x++) {
		*x = ++num_edges;
	}

	bitmap = (uint8_t *) calloc((num_edges + 1) / 8 + 1, 1);
	if (!bitmap) {
		perror("[__sanitizer_cov_trace_pc_guard_init] calloc failed");
		abort();
	}
}

void __sanitizer_cov_trace_pc_guard(uint32_t *guard) {
	if (!*guard) return;

	uint32_t idx = *guard;

	if (idx == 0) return;
	SET_BIT(bitmap, idx);

	//void *PC = __builtin_return_address(0);
	//char PcDescr[1024];

	//__sanitizer_symbolize_pc(PC, "%p %F %L", PcDescr, sizeof(PcDescr));
	//printf("guard: %p, %x, PC %p\n", (void *) guard, *guard, PC);
	//printf("%p\n", PC);

	*guard = 0;
}

size_t binary_name(char buff[static 1024]) {
	__pid_t pid = getpid();

	char proc_file_buff[1024];
	sprintf(proc_file_buff, "/proc/%d/cmdline", pid);
	FILE *f = fopen(proc_file_buff, "r");
	if (!f) {
		perror("[binary_name] Failed to open proc file");
		abort();
	}

	char ch;
	size_t count = 0;
	while ((ch = fgetc(f)) != EOF) {
		buff[count++] = ch;
		if (ch == '\0') break;
		if (count == 1024) {
			fprintf(stderr, "Binary name to long");
			abort();
		}
	}

	fclose(f);
	return count - 1;
}

void cov_dump(void) {
	//printf("%ld == %ld\n", num_pcs, num_edges);
	assert(num_pcs == num_edges);

	size_t total = num_edges;

	char binary[1024];
	size_t binary_length = binary_name(binary);

	char *path_prefix = getenv("SAN_COV_PATH_PREFIX");
	if (!path_prefix) return;
	
	char path[4096];
	snprintf(path, sizeof(path), "%s/coverage-%d.bin", path_prefix, getpid());

	FILE *f = fopen(path, "wb");
	if (!f) {
		perror("[cov_dump] fopen failed");
		return;
	}

	// dump size of the binary name
	fwrite(&binary_length, sizeof(binary_length), 1, f);

	// dump binary name
	fwrite(binary, 1, binary_length, f);

	// dump total number of edges
	fwrite(&total, sizeof(total), 1, f);

	// dump PC-tabel
	fwrite(edge_to_pc + 1, sizeof(uintptr_t), num_pcs, f);

	// dump bitmap
	fwrite(bitmap, 1, (total + 1) / 8 + 1, f);

	fclose(f);
}


__attribute__((constructor))
static void cov_register_exit(void) {
	atexit(cov_dump);
}
