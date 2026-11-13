# tests/Makefile - one binary per test file.
#   test_protocol : framing + socket helpers (common/ only)
#   test_tracker  : tracker state rules + a real in-process tracker server
#   test_client   : client's tracker link (failover, re-login) vs in-process trackers
include ../common/flags.mk

TRACKER_DIR := ../tracker
CLIENT_DIR  := ../client
CPPFLAGS    += -I$(TRACKER_DIR) -I$(CLIENT_DIR)
vpath %.cpp . $(COMMON_DIR) $(TRACKER_DIR) $(CLIENT_DIR)

# Every tracker/client source except its main.cpp (each test has its own main).
TRACKER_SRCS := $(filter-out main.cpp,$(notdir $(wildcard $(TRACKER_DIR)/*.cpp)))
CLIENT_SRCS  := $(filter-out main.cpp,$(notdir $(wildcard $(CLIENT_DIR)/*.cpp)))
TRACKER_OBJS := $(addprefix $(OBJDIR)/,$(TRACKER_SRCS:.cpp=.o))
CLIENT_OBJS  := $(addprefix $(OBJDIR)/,$(CLIENT_SRCS:.cpp=.o))

TESTS := test_protocol$(SUFFIX) test_tracker$(SUFFIX) test_client$(SUFFIX)

all: $(TESTS)

test_protocol$(SUFFIX): $(OBJDIR)/test_protocol.o $(COMMON_OBJS)
	$(CXX) $^ $(LDFLAGS) -o $@

test_tracker$(SUFFIX): $(OBJDIR)/test_tracker.o $(TRACKER_OBJS) $(COMMON_OBJS)
	$(CXX) $^ $(LDFLAGS) -o $@

test_client$(SUFFIX): $(OBJDIR)/test_client.o $(CLIENT_OBJS) $(TRACKER_OBJS) $(COMMON_OBJS)
	$(CXX) $^ $(LDFLAGS) -o $@

clean:
	rm -rf obj obj-asan obj-tsan test_protocol test_protocol-* test_tracker test_tracker-* \
	       test_client test_client-*

-include $(wildcard $(OBJDIR)/*.d)

.PHONY: all clean
