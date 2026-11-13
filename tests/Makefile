# tests/Makefile - one binary per test file.
#   test_protocol : framing + socket helpers (common/ only)
#   test_tracker  : tracker state rules + a real in-process tracker server
include ../common/flags.mk

TRACKER_DIR  := ../tracker
CPPFLAGS     += -I$(TRACKER_DIR)
vpath %.cpp . $(COMMON_DIR) $(TRACKER_DIR)

# Every tracker source except main.cpp (the test has its own main).
TRACKER_SRCS := $(filter-out main.cpp,$(notdir $(wildcard $(TRACKER_DIR)/*.cpp)))
TRACKER_OBJS := $(addprefix $(OBJDIR)/,$(TRACKER_SRCS:.cpp=.o))

TESTS := test_protocol$(SUFFIX) test_tracker$(SUFFIX)

all: $(TESTS)

test_protocol$(SUFFIX): $(OBJDIR)/test_protocol.o $(COMMON_OBJS)
	$(CXX) $^ $(LDFLAGS) -o $@

test_tracker$(SUFFIX): $(OBJDIR)/test_tracker.o $(TRACKER_OBJS) $(COMMON_OBJS)
	$(CXX) $^ $(LDFLAGS) -o $@

clean:
	rm -rf obj obj-asan obj-tsan test_protocol test_protocol-* test_tracker test_tracker-*

-include $(wildcard $(OBJDIR)/*.d)

.PHONY: all clean
