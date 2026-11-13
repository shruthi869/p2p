# Top-level Makefile.
#   make            build tracker/tracker and client/client
#   make test       build and run all tests (unit tests + end-to-end integration)
#   make asan       same tests built with AddressSanitizer + UBSan
#   make tsan       same tests built with ThreadSanitizer
#   make valgrind   all tests under valgrind (memory errors, leaks, open fds)
#   make clean      remove all build outputs

ifeq ($(SAN),address)
SUFFIX := -asan
MODE   := asan
else ifeq ($(SAN),thread)
SUFFIX := -tsan
MODE   := tsan
else
MODE   := plain
endif

SUBDIRS   := tracker client tests
TEST_BINS := tests/test_protocol tests/test_tracker tests/test_client

all: tracker client

tracker client tests:
	$(MAKE) -C $@

test: all tests
	@for t in $(TEST_BINS); do echo "--- $$t$(SUFFIX)"; ./$$t$(SUFFIX) || exit 1; done
	@echo "--- integration ($(MODE))"; MODE=$(MODE) bash tests/integration.sh

asan:
	$(MAKE) test SAN=address

tsan:
	$(MAKE) test SAN=thread

valgrind: all tests
	@for t in $(TEST_BINS); do echo "--- valgrind $$t"; \
	    valgrind --quiet --leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=all \
	    --error-exitcode=1 ./$$t || exit 1; done
	@echo "--- integration (valgrind)"; MODE=valgrind bash tests/integration.sh

clean:
	for d in $(SUBDIRS); do $(MAKE) -C $$d clean; done
	rm -rf test_runs

.PHONY: all tracker client tests test asan tsan valgrind clean
