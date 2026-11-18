#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

/* No shared writable alias: the parent retains the original anonymous page. */
static uint32_t pattern(unsigned round, size_t page, size_t word)
{
	return 0x6d2b79f5U ^ (round * 0x9e3779b9U) ^
		((uint32_t)page * 0x85ebca6bU) ^ ((uint32_t)word * 0xc2b2ae35U);
}

static int verify(volatile uint32_t *p, size_t words, unsigned round,
		  size_t page, int copied)
{
	size_t i;
	for (i = 0; i < words; i++) {
		uint32_t expected = pattern(round, page, i);
		uint32_t actual = p[i];
		if (copied && !i)
			expected ^= 0xffffffffU;
		if (actual != expected) {
			fprintf(stderr, "FAIL round=%u page=%zu word=%zu copied=%d "
				"expected=%08x actual=%08x\n", round, page, i,
				copied, expected, actual);
			return -1;
		}
	}
	return 0;
}

static int pin(int cpu)
{
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	if (sched_setaffinity(0, sizeof(set), &set)) {
		perror("sched_setaffinity");
		return -1;
	}
	return 0;
}

static unsigned number(const char *s, unsigned max)
{
	char *end;
	unsigned long n;
	errno = 0;
	n = strtoul(s, &end, 10);
	if (errno || !*s || *end || !n || n > max) {
		fprintf(stderr, "Invalid argument: %s\n", s);
		exit(2);
	}
	return n;
}

int main(int argc, char **argv)
{
	unsigned rounds = argc > 1 ? number(argv[1], 100000) : 16;
	size_t pages = argc > 2 ? number(argv[2], 4096) : 256;
	int inject = argc == 4 && !strcmp(argv[3], "--inject");
	long page_size = sysconf(_SC_PAGESIZE);
	cpu_set_t allowed;
	int cpus[CPU_SETSIZE], ncpu = 0, cpu;
	unsigned round;
	size_t page, word, words, bytes;
	volatile uint32_t *area;
	if (argc > 4 || (argc == 4 && !inject) || page_size <= 0) {
		fprintf(stderr, "Usage: %s [rounds [pages [--inject]]]\n", argv[0]);
		return 2;
	}
	if (sched_getaffinity(0, sizeof(allowed), &allowed)) {
		perror("sched_getaffinity");
		return 2;
	}
	for (cpu = 0; cpu < CPU_SETSIZE; cpu++)
		if (CPU_ISSET(cpu, &allowed))
			cpus[ncpu++] = cpu;
	if (!ncpu)
		return 2;
	words = (size_t)page_size / sizeof(uint32_t);
	bytes = pages * (size_t)page_size;
	area = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (area == MAP_FAILED) {
		perror("mmap");
		return 2;
	}
	/* Prevent THP from replacing the base-page COW exercise on host tests. */
	if (madvise((void *)area, bytes, MADV_NOHUGEPAGE) && errno != EINVAL) {
		perror("madvise");
		return 2;
	}
	setvbuf(stdout, NULL, _IONBF, 0);
	printf("COW_REUSE BEGIN rounds=%u pages=%zu page_size=%ld cpus=%d "
	       "first_cpu=%d last_cpu=%d inject=%d\n", rounds, pages, page_size,
	       ncpu, cpus[0], cpus[ncpu - 1], inject);
	if (pin(cpus[0]))
		return 2;
	/* Keep one source page hot across reuse cycles before changing colour. */
	for (page = 0; page < pages; page++) {
		volatile uint32_t *p = area + page * words;
		for (word = 0; word < words; word++)
			p[word] = pattern(0, page, word);
		for (round = 0; round < rounds; round++) {
			pid_t pid;
			int status;
			int parent_cpu = cpus[round % 2 ? ncpu - 1 : 0];
			int child_cpu = cpus[round % 2 ? 0 : ncpu - 1];
			if (pin(parent_cpu))
				return 2;
			pid = fork();
			if (pid < 0) {
				perror("fork");
				return 2;
			}
			if (!pid) {
				if (pin(child_cpu))
					_exit(2);
				if (verify(p, words, round, page, 0))
					_exit(1);
				/* Force COW, then check all bytes the store did not change. */
				p[0] = pattern(round, page, 0) ^ 0xffffffffU;
				if (inject && !round && !page)
					p[1] ^= 1;
				if (verify(p, words, round, page, 1))
					_exit(1);
				_exit(0);
			}
			while (waitpid(pid, &status, 0) < 0) {
				if (errno == EINTR)
					continue;
				perror("waitpid");
				return 2;
			}
			if (!WIFEXITED(status) || WEXITSTATUS(status)) {
				fprintf(stderr, "FAIL child round=%u page=%zu status=%#x\n",
					round, page, status);
				return 1;
			}
			if (verify(p, words, round, page, 0))
				return 1;
			/* Original page now eligible for wp_page_reuse, not a copy. */
			for (word = 0; word < words; word++)
				p[word] = pattern(round + 1, page, word);
		}
		if (verify(p, words, rounds, page, 0))
			return 1;
		if ((page + 1) % 32 == 0)
			printf("COW_REUSE colours_completed=%zu\n", page + 1);
	}
	if (munmap((void *)area, bytes)) {
		perror("munmap");
		return 2;
	}
	printf("COW_REUSE PASS rounds=%u pages=%zu checked_cow=%lu\n",
	       rounds, pages, (unsigned long)rounds * pages);
	return 0;
}
