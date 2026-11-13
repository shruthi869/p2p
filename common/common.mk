# common.mk - build settings shared by tracker/, client/ and tests/.
# A sub-Makefile sets NAME (the binary name) and then includes this file.
#
#   make             -> optimised build:          ./NAME
#   make SAN=address -> AddressSanitizer + UBSan:  ./NAME-asan
#   make SAN=thread  -> ThreadSanitizer:           ./NAME-tsan
# Each flavour uses its own object directory, so they never get mixed.

COMMON_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))

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

OBJDIR := obj$(SUFFIX)
TARGET := $(NAME)$(SUFFIX)
SRCS   := $(wildcard *.cpp) $(wildcard $(COMMON_DIR)/*.cpp)
OBJS   := $(addprefix $(OBJDIR)/,$(notdir $(SRCS:.cpp=.o)))

vpath %.cpp . $(COMMON_DIR)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(OBJS) $(LDFLAGS) -o $@

# -MMD -MP: also write a .d file listing the headers each .cpp includes, so
# editing a header rebuilds everything that depends on it.
$(OBJDIR)/%.o: %.cpp | $(OBJDIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(OBJDIR):
	mkdir -p $@

clean:
	rm -rf obj obj-asan obj-tsan $(NAME) $(NAME)-asan $(NAME)-tsan

-include $(OBJS:.o=.d)

.PHONY: all clean
