#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

extern int32_t masked_guard_allfalse(int32_t *inaccessible);
extern int32_t masked_guard_sparse(int32_t *boundary, int32_t *observed);

enum { LANES = 7 };

static uint8_t before[65536];

static int byte_is_part_of_active_lane(size_t byte, size_t boundary_offset) {
    const size_t lane0 = boundary_offset;
    const size_t lane2 = boundary_offset + 2U * sizeof(int32_t);
    return (byte >= lane0 && byte < lane0 + sizeof(int32_t)) ||
           (byte >= lane2 && byte < lane2 + sizeof(int32_t));
}

static int run_case(
    int run_allfalse,
    int run_sparse,
    uint64_t case_seed,
    unsigned alignment_offset) {
    const long page_size_long = sysconf(_SC_PAGESIZE);
    if (page_size_long <= 0) return 10;
    const size_t page_size = (size_t)page_size_long;
    if (page_size < 256U || page_size % sizeof(int32_t) != 0U) return 11;

    uint8_t *mapping = (uint8_t *)mmap(
        0, page_size * 3U, PROT_NONE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) return 12;
    uint8_t *rw_page = mapping + page_size;
    if (mprotect(rw_page, page_size, PROT_READ | PROT_WRITE) != 0) return 13;
    const uint8_t canary = (uint8_t)(0x80U | ((case_seed >> 17U) & 0x7fU));
    memset(rw_page, canary, page_size);

    if (run_allfalse) {
        const size_t aligned_slots = (page_size - 128U) / 64U;
        const size_t slot = aligned_slots == 0U
                                ? 0U
                                : (size_t)(case_seed % aligned_slots);
        const size_t inaccessible_offset =
            64U + slot * 64U + (size_t)alignment_offset;
        int32_t *inaccessible =
            (int32_t *)(void *)(mapping + inaccessible_offset);
        if (masked_guard_allfalse(inaccessible) != 143) return 20;
    }

    if (run_sparse) {
        const size_t boundary_offset = page_size - 3U * sizeof(int32_t);
        int32_t *boundary = (int32_t *)(void *)(rw_page + boundary_offset);
        const int32_t seed_delta = (int32_t)(case_seed & UINT64_C(0x3ff));
        boundary[0] = 101 + seed_delta;
        boundary[1] = 202;
        boundary[2] = 303 - seed_delta;

        if (page_size > sizeof(before)) return 14;
        memcpy(before, rw_page, page_size);

        int32_t observed[LANES] = {0};
        const int32_t result = masked_guard_sparse(boundary, observed);
        const int32_t expected_observed[LANES] = {
            101 + seed_delta, 223, 303 - seed_delta, 229, 233, 239, 241,
        };
        if (result != 1569) return 30;
        for (unsigned lane = 0; lane < LANES; ++lane)
            if (observed[lane] != expected_observed[lane])
                return 31 + (int)lane;

        if (boundary[0] != 1001 || boundary[1] != 202 || boundary[2] != 1003)
            return 40;
        for (size_t byte = 0; byte < page_size; ++byte) {
            if (!byte_is_part_of_active_lane(byte, boundary_offset) &&
                rw_page[byte] != before[byte])
                return 41;
        }
    }

    return munmap(mapping, page_size * 3U) == 0 ? 0 : 50;
}

int main(int argc, char **argv) {
    if (argc < 2 || argc > 4) return 2;
    uint64_t case_seed = 0;
    unsigned alignment_offset = 0;
    if (argc >= 3) {
        char *end = 0;
        case_seed = (uint64_t)strtoull(argv[2], &end, 0);
        if (end == argv[2] || *end != '\0') return 4;
    }
    if (argc >= 4) {
        char *end = 0;
        const unsigned long parsed = strtoul(argv[3], &end, 0);
        if (end == argv[3] || *end != '\0' || parsed > 60UL ||
            (parsed & 3UL) != 0UL)
            return 5;
        alignment_offset = (unsigned)parsed;
    }
    if (strcmp(argv[1], "allfalse") == 0)
        return run_case(1, 0, case_seed, alignment_offset);
    if (strcmp(argv[1], "sparse") == 0)
        return run_case(0, 1, case_seed, alignment_offset);
    return 3;
}
