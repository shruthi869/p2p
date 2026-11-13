# Top-level Makefile.
#   make            build tracker/tracker and client/client
#   make test       build and run all tests
#   make asan       same tests built with AddressSanitizer + UBSan
#   make tsan       same tests built with ThreadSanitizer
#   make valgrind   run the tests under valgrind (memory leaks, bad reads)
#   make clean      remove all build outputs

ifeq ($(SAN),address)
SUFFIX := -asan
else ifeq ($(SAN),thread)
SUFFIX := -tsan
endif

SUBDIRS := tracker client tests

all: tracker client

tracker client tests:
	$(MAKE) -C $@

TEST_BINS := tests/test_protocol tests/test_tracker

test: all tests
	@for t in $(TEST_BINS); do echo "--- $$t$(SUFFIX)"; ./$$t$(SUFFIX) || exit 1; done

asan:
	$(MAKE) test SAN=address

tsan:
	$(MAKE) test SAN=thread

valgrind: all tests
	@for t in $(TEST_BINS); do echo "--- valgrind $$t"; \
	    valgrind --quiet --leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=all \
	    --error-exitcode=1 ./$$t || exit 1; done

clean:
	for d in $(SUBDIRS); do $(MAKE) -C $$d clean; done

.PHONY: all tracker client tests test asan tsan valgrind clean
