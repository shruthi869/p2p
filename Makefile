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

test: all tests
	./tests/test_protocol$(SUFFIX)

asan:
	$(MAKE) test SAN=address

tsan:
	$(MAKE) test SAN=thread

valgrind: all tests
	valgrind --quiet --leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=all \
	    --error-exitcode=1 ./tests/test_protocol

clean:
	for d in $(SUBDIRS); do $(MAKE) -C $$d clean; done

.PHONY: all tracker client tests test asan tsan valgrind clean
