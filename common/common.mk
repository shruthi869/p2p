# common.mk - rules for a directory that builds ONE program (tracker/, client/).
# The directory's Makefile sets NAME and includes this file.
include $(dir $(lastword $(MAKEFILE_LIST)))flags.mk

TARGET := $(NAME)$(SUFFIX)
OBJS   := $(addprefix $(OBJDIR)/,$(patsubst %.cpp,%.o,$(wildcard *.cpp))) $(COMMON_OBJS)

vpath %.cpp . $(COMMON_DIR)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(OBJS) $(LDFLAGS) -o $@

clean:
	rm -rf obj obj-asan obj-tsan $(NAME) $(NAME)-asan $(NAME)-tsan

-include $(OBJS:.o=.d)

.PHONY: all clean
