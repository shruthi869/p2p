# flags.mk - compiler flags and build flavours, shared by every Makefile.
#
#   make             -> optimised build          (objects in obj/,      suffix "")
#   make SAN=address -> AddressSanitizer + UBSan (objects in obj-asan/, suffix -asan)
#   make SAN=thread  -> ThreadSanitizer          (objects in obj-tsan/, suffix -tsan)
# Each flavour has its own object directory, so they never get mixed.

COMMON_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))

# The rules below would otherwise become the default target; `make` = `make all`.
.DEFAULT_GOAL := all

CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic -g -pthread
CPPFLAGS := -I$(COMMON_DIR)
LDFLAGS  := -pthread

ifeq ($(SAN),address)
  CXXFLAGS += -O1 -fsanitize=address,undefined -fno-omit-frame-pointer
  LDFLAGS  += -fsanitize=address,undefined
  SUFFIX   := -asan
else ifeq ($(SAN),thread)
  CXXFLAGS += -O1 -fsanitize=thread
  LDFLAGS  += -fsanitize=thread
  SUFFIX   := -tsan
else
  CXXFLAGS += -O2
  SUFFIX   :=
endif

OBJDIR      := obj$(SUFFIX)
COMMON_SRCS := $(notdir $(wildcard $(COMMON_DIR)/*.cpp))
COMMON_OBJS := $(addprefix $(OBJDIR)/,$(COMMON_SRCS:.cpp=.o))

# -MMD -MP: also write a .d file listing the headers each .cpp includes, so
# editing a header rebuilds everything that depends on it.
$(OBJDIR)/%.o: %.cpp | $(OBJDIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(OBJDIR):
	mkdir -p $@
